/*-------------------------------------------------------------------------
 *
 * lion_count_decode.c
 *		The groups of several columns, decoded key by key (DESIGN.md §34).
 *
 * Part of the count engine: lion_count.h is its interface, and
 * lion_count_int.h declares what its files share.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/xact.h"
#include "access/xlog.h"
#include "miscadmin.h"
#include "port/pg_bitutils.h"
#include "storage/buffile.h"
#include "storage/predicate.h"
#include "utils/memutils.h"

#include "lion_count_int.h"

/* ---------------------------------------------------------------------
 * The tally: a count per combination of groups
 * --------------------------------------------------------------------- */

/*
 * Combinations are counted in an array of every one the chunks make when it
 * takes at most a quarter of the tally's memory - a few low-cardinality
 * columns, the usual GROUP BY: 256k combinations at the default hash_mem of
 * 8 MB - and in a hash table of the ones that have rows otherwise.  An array
 * cell is an add; a hash entry a lookup, some ten times that (measured
 * 2026-09-30: 5M rows into 100,000 combinations, 250 ms in a hash table and
 * 30 in an array).  The array is read back whole, which is a scan of
 * memory the size of the tally.
 */
#define LION_DECODE_DENSE_SHARE	4

typedef struct LionTallyEnt
{
	uint64		code;
	int64		count;
	char		status;
} LionTallyEnt;

/* The splitmix64 finalizer: every bit of a code reaches the hash's. */
static inline uint32
lion_tally_hash_code(uint64 k)
{
	k ^= k >> 30;
	k *= UINT64CONST(0xbf58476d1ce4e5b9);
	k ^= k >> 27;
	k *= UINT64CONST(0x94d049bb133111eb);
	k ^= k >> 31;
	return (uint32) k;
}

/*
 * The most members the hash holds before it spills, whatever hash_mem says:
 * simplehash counts them in a uint32 and stops growing at 2^32 buckets,
 * which it fills to 98% at most.  Past it the quotient of hash_mem by the
 * entries' size was cast to a uint32 and wrapped - at 192 GB of hash_mem it
 * came out 0, and the tally spilled at every new combination (2026-10
 * review).  2^31 members take a table of 2^32 buckets, 96 GB, as much as the
 * hash_mem at which this bound begins to apply.
 */
#define LION_DECODE_TALLY_MAX_ENTS	((Size) PG_INT32_MAX)

#define SH_PREFIX		lion_tally
#define SH_ELEMENT_TYPE	LionTallyEnt
#define SH_KEY_TYPE		uint64
#define SH_KEY			code
#define SH_HASH_KEY(tb, key)	lion_tally_hash_code(key)
#define SH_EQUAL(tb, a, b)		((a) == (b))
#define SH_SCOPE		static inline
#define SH_DECLARE
#define SH_DEFINE
#include "lib/simplehash.h"

/*
 * A pass's counts.  A tally that outgrows maxbytes writes what it holds to a
 * temporary file and starts again: the same combination may then come out of
 * it more than once, which the Finalize Agg above the node adds up, as it
 * adds up a partitioned table's partial counts (DESIGN.md §16) - so memory
 * stays within hash_mem whatever the planner's estimate of the groups was.
 */
struct LionDecodeTally
{
	MemoryContext parent;		/* where the tally itself lives */
	MemoryContext cxt;			/* what one pass allocates */
	Size		maxbytes;
	int			ncol;
	uint64		ncells;			/* the combinations the radixes make */
	int64	   *dense;			/* ncells counts, or NULL */
	lion_tally_hash *hash;		/* ... or the combinations seen */
	uint32		maxents;		/* the hash's members before it spills */
	BufFile    *spill;			/* (code, count) pairs, or NULL */
	int64		nspilled;
	int64		nspills;

	/* reading it back */
	bool		reading;
	int			phase;			/* 0 memory, 1 the file, 2 done */
	uint64		densepos;
	lion_tally_iterator it;
	int64		nread;
};

LionDecodeTally *
lion_decode_tally_create(MemoryContext cxt, Size maxbytes)
{
	LionDecodeTally *t;

	t = (LionDecodeTally *) MemoryContextAllocZero(cxt, sizeof(LionDecodeTally));
	t->parent = cxt;
	t->cxt = AllocSetContextCreate(cxt, "lion decode tally",
								   ALLOCSET_DEFAULT_SIZES);
	t->maxbytes = Max(maxbytes, (Size) 64 * 1024);
	return t;
}

/*
 * Begin a pass whose column c has radix[c] values: every count zero.  False
 * when the combinations would not fit in a code (the caller takes smaller
 * chunks).
 */
bool
lion_decode_tally_begin(LionDecodeTally *t, int ncol, const int *radix)
{
	uint64		ncells = 1;
	int			c;

	for (c = 0; c < ncol; c++)
	{
		Assert(radix[c] > 0);
		if (ncells > (UINT64CONST(1) << 62) / (uint64) radix[c])
			return false;
		ncells *= (uint64) radix[c];
	}

	lion_decode_tally_end(t);
	t->ncol = ncol;
	t->ncells = ncells;
	t->nspilled = 0;
	t->reading = false;
	t->phase = 0;
	t->densepos = 0;
	t->nread = 0;

	if (ncells <= t->maxbytes / (sizeof(int64) * LION_DECODE_DENSE_SHARE) &&
		ncells <= MaxAllocSize / sizeof(int64))
	{
		t->dense = (int64 *) MemoryContextAllocZero(t->cxt,
													sizeof(int64) * ncells);
		t->hash = NULL;
	}
	else
	{
		t->dense = NULL;
		t->hash = lion_tally_create(t->cxt, 256, NULL);
		t->maxents = (uint32)
			Min(Max(t->maxbytes / (2 * sizeof(LionTallyEnt)), (Size) 1024),
				LION_DECODE_TALLY_MAX_ENTS);
	}
	return true;
}

/* What the hash holds, onto the end of the temporary file, and forget it. */
static void
lion_decode_tally_spill(LionDecodeTally *t)
{
	lion_tally_iterator it;
	LionTallyEnt *e;

	if (t->spill == NULL)
	{
		MemoryContext oldcxt = MemoryContextSwitchTo(t->parent);

		t->spill = BufFileCreateTemp(false);
		MemoryContextSwitchTo(oldcxt);
	}
	LION_SH_START_ITERATE(lion_tally, t->hash, &it);
	while ((e = lion_tally_iterate(t->hash, &it)) != NULL)
	{
		BufFileWrite(t->spill, &e->code, sizeof(e->code));
		BufFileWrite(t->spill, &e->count, sizeof(e->count));
		t->nspilled++;
	}
	lion_tally_reset(t->hash);
	t->nspills++;
}

static inline void
lion_decode_tally_add(LionDecodeTally *t, uint64 code, int64 n)
{
	Assert(code < t->ncells && !t->reading);
	if (t->dense != NULL)
		t->dense[code] += n;
	else
	{
		bool		found;
		LionTallyEnt *e = lion_tally_insert(t->hash, code, &found);

		if (!found)
		{
			e->count = n;
			if (t->hash->members > t->maxents)
				lion_decode_tally_spill(t);
		}
		else
			e->count += n;
	}
}

/*
 * The next combination with rows, and their number: those in memory, then
 * those of the file.  False once there is none left.
 */
bool
lion_decode_tally_next(LionDecodeTally *t, uint64 *code, int64 *count)
{
	if (!t->reading)
	{
		t->reading = true;
		if (t->hash != NULL)
			LION_SH_START_ITERATE(lion_tally, t->hash, &t->it);
	}

	if (t->phase == 0)
	{
		if (t->dense != NULL)
		{
			while (t->densepos < t->ncells)
			{
				uint64		i = t->densepos++;

				if (t->dense[i] != 0)
				{
					*code = i;
					*count = t->dense[i];
					return true;
				}
			}
		}
		else if (t->hash != NULL)
		{
			LionTallyEnt *e = lion_tally_iterate(t->hash, &t->it);

			if (e != NULL)
			{
				*code = e->code;
				*count = e->count;
				return true;
			}
		}
		t->phase = 1;
		if (t->spill != NULL &&
			BufFileSeek(t->spill, 0, 0, SEEK_SET) != 0)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not rewind the temporary file of a lion index count")));
	}

	if (t->phase == 1 && t->spill != NULL && t->nread < t->nspilled)
	{
		BufFileReadExact(t->spill, code, sizeof(*code));
		BufFileReadExact(t->spill, count, sizeof(*count));
		t->nread++;
		return true;
	}
	t->phase = 2;
	return false;
}

/* The pass is over: its memory and its file go. */
void
lion_decode_tally_end(LionDecodeTally *t)
{
	if (t->spill != NULL)
	{
		BufFileClose(t->spill);
		t->spill = NULL;
	}
	MemoryContextReset(t->cxt);
	t->dense = NULL;
	t->hash = NULL;
}

/* The value of each column a code names, radix[c] of them in column c. */
void
lion_decode_code_split(uint64 code, int ncol, const int *radix, int *vals)
{
	int			c;

	for (c = ncol - 1; c >= 0; c--)
	{
		vals[c] = (int) (code % (uint64) radix[c]);
		code /= (uint64) radix[c];
	}
}

/* What a cursor of the walk takes besides its page image, for a caller's budget. */
Size
lion_count_cursor_bytes(void)
{
	return sizeof(LionSetCursor) + sizeof(LionCountCtx) / LION_MAX_DECODE_COLS;
}

/* ---------------------------------------------------------------------
 * The walk
 * --------------------------------------------------------------------- */

/* What the walk keeps beside the recheck list: each TID's combination. */
typedef struct LionDecodeRecheck
{
	LionCountCtx *cx;			/* its tids, and the heap's answers */
	uint64	   *codes;
	bool	   *vis;
	int			cap;
	LionDecodeTally *tally;
	LionDecodeStats *dstats;
} LionDecodeRecheck;

/* The rows of the list the heap shows, into the tally; the list emptied. */
static void
lion_decode_recheck_flush(LionDecodeRecheck *rc)
{
	LionCountCtx *cx = rc->cx;
	int			i;

	if (cx->ntids == 0)
		return;
	memset(rc->vis, 0, sizeof(bool) * cx->ntids);
	(void) lion_recheck_visible(cx, rc->vis);
	for (i = 0; i < cx->ntids; i++)
	{
		if (rc->vis[i])
		{
			/*
			 * A code past the tally's is a row two entries of one column
			 * label (lion_count_cells()), which no row the snapshot sees can
			 * be: a heap TID is in one entry of a scalar index at a time.
			 */
			if (rc->codes[i] >= rc->tally->ncells)
				elog(ERROR, "lion index count: a visible row (%u,%u) in two entries of one column",
					 ItemPointerGetBlockNumber(&cx->tids[i]),
					 ItemPointerGetOffsetNumber(&cx->tids[i]));
			lion_decode_tally_add(rc->tally, rc->codes[i], 1);
			rc->dstats->rows++;
		}
	}
	rc->dstats->rechecked += cx->ntids;
	cx->ntids = 0;
}

/*
 * What the list keeps a row: its TID, its combination and the heap's answer.
 * Its budget is work_mem of those (lion_recheck_budget_for()): sized by the
 * TID alone, and doubled past it, it took two and a half to five times
 * work_mem (2026-10 review).
 */
#define LION_DECODE_RECHECK_ENTRY \
	(sizeof(ItemPointerData) + sizeof(uint64) + sizeof(bool))

/*
 * One row for the heap to decide on, with its combination.  The rows come key
 * by key and in lo order inside a key, so the list is in TID order; it is
 * rechecked, and emptied, before it outgrows its budget - at a block
 * boundary, as lion_recheck_add() does, and for the reason given there, which
 * holds here: the map has been asked about every block of it already, under
 * the pins.
 */
static void
lion_decode_recheck_add(LionDecodeRecheck *rc, uint32 ckey, uint32 lo,
						uint64 code)
{
	LionCountCtx *cx = rc->cx;
	ItemPointerData tid;

	lion_code_to_tid(((uint64) ckey << LION_CONTAINER_BITS) | lo, &tid);
	if (cx->ntids > 0 &&
		cx->ntids + LION_RECHECK_BLOCK_TIDS > cx->batchmax &&
		ItemPointerGetBlockNumber(&cx->tids[cx->ntids - 1]) !=
		ItemPointerGetBlockNumber(&tid))
		lion_decode_recheck_flush(rc);

	if (cx->ntids >= rc->cap)
	{
		MemoryContext oldcxt = MemoryContextSwitchTo(cx->cxt);
		int			newcap = (rc->cap == 0) ? Min(1024, cx->batchmax) :
			Min(rc->cap * 2, cx->batchmax);

		/* a block larger than the budget: no budget is that small */
		if (newcap <= rc->cap)
			newcap = rc->cap + LION_RECHECK_BLOCK_TIDS;

		if (rc->cap == 0)
		{
			cx->tids = (ItemPointerData *) palloc(sizeof(ItemPointerData) * newcap);
			rc->codes = (uint64 *) palloc(sizeof(uint64) * newcap);
			rc->vis = (bool *) palloc(sizeof(bool) * newcap);
		}
		else
		{
			cx->tids = (ItemPointerData *)
				repalloc(cx->tids, sizeof(ItemPointerData) * newcap);
			rc->codes = (uint64 *) repalloc(rc->codes, sizeof(uint64) * newcap);
			rc->vis = (bool *) repalloc(rc->vis, sizeof(bool) * newcap);
		}
		rc->cap = newcap;
		cx->maxtids = newcap;
		MemoryContextSwitchTo(oldcxt);
	}
	Assert(cx->ntids == 0 ||
		   ItemPointerCompare(&cx->tids[cx->ntids - 1], &tid) < 0);
	cx->tids[cx->ntids] = tid;
	rc->codes[cx->ntids] = code;
	cx->ntids++;
}

/*
 * THE GROUPS OF SEVERAL COLUMNS, DECODED KEY BY KEY (DESIGN.md §34).  One pass
 * over the container keys for every combination of the columns' values in
 * cols, each combination's rows - those in all of its sets and in `where` -
 * added to the tally.
 *
 * Counted as pairs (§20), a combination was an AND of its sets: as many ANDs
 * as combinations, each over every key its sets share - work that grows with
 * the product of the columns' values.  But at a key every row is in exactly
 * one value of each column (a scalar index files every row it holds under
 * one entry of the column, its NULL entry for NULL), so the key's rows can be
 * told apart instead: each column's containers there DECODED into the value
 * each row has - a store per member into an array of the key's 32768 row
 * positions - and then one pass over the rows the WHERE keeps, each row's
 * values read off the arrays and its combination counted.  That is two
 * passes over the key's rows whatever the number of combinations, a few
 * hundred nanoseconds a thousand rows where the ANDs took microseconds a
 * combination (measured 2026-09-30: 8 us a key against 80 for 512
 * combinations, 19 against 3,800 for 32,768).
 *
 * THE WALK.  Column 0 drives it, with `where`: every cursor of column 0 is on
 * one heap and the others' on another, each ordered by container key, and at
 * the smallest key column 0 stands at that the copy holds (or at every key of
 * column 0 without a WHERE), column 0's containers are decoded - the rows of
 * the pass are exactly its rows, since its values in the pass are the only
 * ones counted - the other columns' cursors are sought to the key (§22: a
 * step, or a descent past the keys between) and theirs decoded, each value
 * stamped with the key's generation so that no array is ever cleared.  A row
 * that some column holds no value of in the pass is not a row of any of its
 * combinations; a row column 0 has that another has lost is a dead one VACUUM
 * is removing (below).
 *
 * WHY IT IS EXACT, AND DESIGN.md §9.  Column 0's sets are located afresh for
 * the pass under pins of their own - an INLINE one keeps its leaf's, a CHAIN
 * one is read by a cursor that pins each page it copies - and every row the
 * walk counts is in exactly one of them: the pinned source a count needs, one
 * for each row.  The map is asked at a key after every column-0 cursor
 * standing there has copied its container and before any moves past it,
 * lion_count_container()'s order.  The other columns, and the copy, are read
 * with no pin at all, and are safe on the terms of lion_sources_collect()'s
 * copies: they only ever narrow or label a row of a pinned set.  A dead TID
 * a pinless column still lists is either gone from column 0's container, and
 * no row of the pass, or in it - so column 0's page was read before VACUUM's
 * ambulkdelete got past it, VACUUM has not set its heap page all-visible,
 * and the row goes to the heap, which does not show it.  A row a pinless
 * column has already lost is dead to every snapshot, and not counting it is
 * right.  A live row cannot be missing from any column: the pass is read
 * after the snapshot was taken, and a visible row was in every index before
 * its transaction committed.  A set of column 0 located past the pin budget
 * (NOPIN) or read from a copy carries nothing, and its rows go to the heap,
 * as its count alone would send them; so does every row in recovery and
 * under a row filter (§17), whose rows the heap has to test.  Rows on blocks
 * the map does not show all-visible go to the heap too, each with its
 * combination (lion_recheck_visible()), after the walk or when the list is
 * full; under SERIALIZABLE the all-visible blocks counted are
 * predicate-locked page-wise, as an index-only scan locks them.
 */
void
lion_count_groups_decode(Relation heap, Snapshot snapshot, int ncol,
						 LionDecodeCol *cols, const LionPostingSet *where,
						 LionDecodeTally *tally, LionCountStats *stats,
						 LionDecodeStats *dstats, LionVisCache *cache,
						 bool rel_read_only)
{
	const LionMatSet *mat = NULL;
	MemoryContext cxt;
	MemoryContext oldcxt;
	LionCountCtx base;
	LionCountCtx ccx[LION_MAX_DECODE_COLS];
	LionDecodeRecheck rc;
	int			radix[LION_MAX_DECODE_COLS];
	uint32	   *val[LION_MAX_DECODE_COLS];
	uint16	   *gen[LION_MAX_DECODE_COLS];
	uint16		curgen = 0;
	LionSetCursor *cur;
	int		   *curcol;
	int		   *curval;
	int			ncur = 0;
	int			ntotal = 0;
	LionGroupEnt *heap0;
	LionGroupEnt *heapr;
	int			nheap0 = 0;
	int			nheapr = 0;
	int		   *hot;
	bool	   *trust0;
	uint64	   *present;
	uint64	   *rows;
	uint64	   *wimg;
	uint16	   *members;
	LionContainer *buf = NULL;
	uint32		firstkey = 0;
	int			idx = 0;
	int			c;
	int			v;
	int			i;

	Assert(ncol >= 1 && ncol <= LION_MAX_DECODE_COLS);
	if (where != NULL)
	{
		if (!where->found || where->mat == NULL ||
			where->mat->ncontainers == 0)
			return;				/* the WHERE selects nothing */
		mat = where->mat;
		Assert(mat->collected);
		firstkey = lion_mat_key(mat, 0);
	}
	for (c = 0; c < ncol; c++)
	{
		radix[c] = cols[c].nsets;
		if (radix[c] <= 0)
			return;				/* a column with no value in the pass */
		ntotal += radix[c];
	}

	cxt = AllocSetContextCreate(CurrentMemoryContext,
								"lion index decoded walk",
								ALLOCSET_DEFAULT_SIZES);
	oldcxt = MemoryContextSwitchTo(cxt);

	/*
	 * A window of keys (lion_vis_cache_set_window()) confines the counts of
	 * a parallel count alone, never this walk's: refused rather than
	 * counted past.
	 */
	if (cache != NULL && cache->windowed)
		elog(ERROR, "lion index count: a decoded walk confined to a window of keys");

	/* what lion_count_groups_copy() sets up for its groups, once */
	memset(&base, 0, sizeof(base));
	base.cxt = cxt;
	base.heap = heap;
	base.snapshot = snapshot;
	base.vmbuf = InvalidBuffer;
	if (cache != NULL && BufferIsValid(cache->vmbuf))
	{
		if (cache->vmrelid == RelationGetRelid(heap))
		{
			base.vmbuf = cache->vmbuf;
			cache->vmbuf = InvalidBuffer;
			cache->vmrelid = InvalidOid;
		}
		else
			lion_vis_cache_release_vm(cache);
	}
	base.serializable = IsolationIsSerializable();
	base.in_recovery = RecoveryInProgress();
	base.rel_read_only = rel_read_only;
	base.raw = true;
	base.tids_sorted = true;
	lion_vis_cache_begin(cache, heap, snapshot);
	base.cache = cache;
	base.filter = (cache != NULL) ? cache->filter : NULL;
	if (base.filter != NULL && base.filter->heap != heap)
		elog(ERROR, "lion index count: a row filter for another relation");
	base.batchmax = lion_recheck_budget_for(LION_DECODE_RECHECK_ENTRY);

	memset(&rc, 0, sizeof(rc));
	rc.cx = &base;
	rc.tally = tally;
	rc.dstats = dstats;

	cur = (LionSetCursor *) palloc(sizeof(LionSetCursor) * ntotal);
	curcol = (int *) palloc(sizeof(int) * ntotal);
	curval = (int *) palloc(sizeof(int) * ntotal);
	heap0 = (LionGroupEnt *) palloc(sizeof(LionGroupEnt) * ntotal);
	heapr = (LionGroupEnt *) palloc(sizeof(LionGroupEnt) * ntotal);
	hot = (int *) palloc(sizeof(int) * ntotal);
	trust0 = (bool *) palloc(sizeof(bool) * radix[0]);
	present = (uint64 *) palloc(LION_BITSET_BYTES);
	rows = (uint64 *) palloc(LION_BITSET_BYTES);
	wimg = (uint64 *) palloc(LION_BITSET_BYTES);
	members = (uint16 *) palloc(sizeof(uint16) * LION_CONTAINER_RANGE);
	for (c = 0; c < ncol; c++)
	{
		val[c] = (uint32 *) palloc(sizeof(uint32) * LION_CONTAINER_RANGE);
		gen[c] = (c == 0) ? NULL :
			(uint16 *) palloc0(sizeof(uint16) * LION_CONTAINER_RANGE);
	}
	if (mat != NULL && lion_mat_spills(mat))
		buf = (LionContainer *) palloc(MAXALIGN(LION_CONTAINER_MAX_SIZE));

	/*
	 * A cursor for every value with an entry: column 0's keep their pins, the
	 * others let go of each page once it is copied.  Nothing below the copy's
	 * first key can be counted: every cursor begins there.
	 */
	for (c = 0; c < ncol; c++)
	{
		ccx[c] = base;
		ccx[c].vmbuf = InvalidBuffer;
		ccx[c].droppins = (c > 0);
		for (v = 0; v < radix[c]; v++)
		{
			LionPostingSet *ps = &cols[c].sets[v];

			if (c == 0)
				trust0[v] = ps->found && !ps->nopin && ps->mat == NULL;
			if (!ps->found)
				continue;
			lion_cursor_init_at(&cur[ncur], ps, &ccx[c], c > 0, firstkey,
								cols[c].images != NULL ?
								&cols[c].images[v] : NULL);
			curcol[ncur] = c;
			curval[ncur] = v;
			if (cur[ncur].valid)
			{
				if (c == 0)
					lion_group_heap_push(heap0, &nheap0, cur[ncur].cur->ckey,
										 ncur);
				else
					lion_group_heap_push(heapr, &nheapr, cur[ncur].cur->ckey,
										 ncur);
			}
			ncur++;
		}
	}

	while (nheap0 > 0)
	{
		uint32		key = heap0[0].ckey;
		BlockNumber firstblk = lion_ckey_first_block(key);
		const uint64 *wbits = NULL;
		int			nhot0;
		int			nhot = 0;
		uint64		wanted = 0;
		uint64		allvis;
		uint64		counted = 0;
		int			b;

		/* the copy at or past the smallest key column 0 stands at */
		if (mat != NULL)
		{
			base.stats.copy_seeks++;
			idx = lion_mat_seek(mat, idx, key);
			if (idx >= mat->ncontainers)
				break;
			if (lion_mat_key(mat, idx) != key)
			{
				uint32		target = lion_mat_key(mat, idx);

				/* nothing below target is counted: column 0 is sought there */
				while (nheap0 > 0 && heap0[0].ckey < target)
				{
					LionGroupEnt e = lion_group_heap_pop(heap0, &nheap0);

					lion_cursor_seek(&cur[e.g], target);
					if (cur[e.g].valid)
						lion_group_heap_push(heap0, &nheap0,
											 cur[e.g].cur->ckey, e.g);
				}
				CHECK_FOR_INTERRUPTS();
				continue;
			}
		}

		/* column 0's cursors standing at the key; the others sought to it */
		while (nheap0 > 0 && heap0[0].ckey == key)
			hot[nhot++] = lion_group_heap_pop(heap0, &nheap0).g;
		nhot0 = nhot;
		while (nheapr > 0 && heapr[0].ckey < key)
		{
			LionGroupEnt e = lion_group_heap_pop(heapr, &nheapr);

			lion_cursor_seek(&cur[e.g], key);
			if (cur[e.g].valid)
				lion_group_heap_push(heapr, &nheapr, cur[e.g].cur->ckey, e.g);
		}
		while (nheapr > 0 && heapr[0].ckey == key)
			hot[nhot++] = lion_group_heap_pop(heapr, &nheapr).g;

		/* a new generation of the arrays: a store stamped with an old one is
		 * no value at this key */
		if (++curgen == 0)
		{
			for (c = 1; c < ncol; c++)
				memset(gen[c], 0, sizeof(uint16) * LION_CONTAINER_RANGE);
			curgen = 1;
		}

		/* column 0: the value of each of its rows, and the rows */
		memset(present, 0, LION_BITSET_BYTES);
		for (i = 0; i < nhot0; i++)
		{
			int			k = hot[i];
			uint32		n = lion_container_to_array(cur[k].cur, members);
			uint32		j;

			for (j = 0; j < n; j++)
			{
				uint16		lo = members[j];

				val[0][lo] = (uint32) curval[k];
				present[lo >> 6] |= UINT64CONST(1) << (lo & 63);
			}
		}

		/* ... of them, the ones the WHERE keeps */
		if (mat != NULL)
		{
			/* through the position filters, once a key (lion_count.c) */
			const LionContainer *w =
				lion_count_posfilter(&base, lion_mat_container(&base, mat, idx, buf));

			if (w->type == LION_CT_BITSET)
				wbits = LION_BITSET_DATA(w);
			else
			{
				memset(wimg, 0, LION_BITSET_BYTES);
				lion_container_or_into_bitset(w, wimg);
				wbits = wimg;
			}
			for (i = 0; i < LION_BITSET_WORDS; i++)
				rows[i] = present[i] & wbits[i];
		}
		else
			memcpy(rows, present, LION_BITSET_BYTES);

		/* the other columns: the value of each of their rows */
		for (i = nhot0; i < nhot; i++)
		{
			int			k = hot[i];
			int			cc = curcol[k];
			uint32		n = lion_container_to_array(cur[k].cur, members);
			uint32	   *vc = val[cc];
			uint16	   *gc = gen[cc];
			uint32		j;

			for (j = 0; j < n; j++)
			{
				uint16		lo = members[j];

				vc[lo] = (uint32) curval[k];
				gc[lo] = curgen;
			}
		}

		/* the heap blocks the rows are on */
		for (b = 0; b < LION_BLOCKS_PER_CONTAINER; b++)
		{
			const int	wpb = (1 << LION_OFFSET_BITS) / 64;
			uint64		any = 0;
			int			w;

			for (w = b * wpb; w < (b + 1) * wpb; w++)
				any |= rows[w];
			if (any != 0)
				wanted |= UINT64CONST(1) << b;
		}

		if (wanted != 0)
		{
			/*
			 * The map, once for the key, every column-0 container here copied
			 * under its pin and none moved past it (the hook of
			 * lion_count_container_vm()).
			 */
			LION_INJECTION_POINT("lion-count-containers-pinned");
			if (base.in_recovery || lion_row_filter_heap(base.filter))
				allvis = 0;
			else
			{
				allvis = lion_vm_allvisible_mask(heap, firstblk, wanted,
												 &base.vmbuf,
												 &base.stats.vm_pins);
				base.stats.vm_checks++;
#ifdef LION_VM_MASK_CHECK
				lion_vm_mask_check(heap, firstblk, wanted, allvis,
								   &base.vmbuf);
#endif
			}

			/* each row's combination, counted or sent to the heap */
			for (i = 0; i < LION_BITSET_WORDS; i++)
			{
				uint64		bits = rows[i];

				while (bits != 0)
				{
					uint32		lo = (uint32) (i * 64 +
											   pg_rightmost_one_pos64(bits));
					uint32		v0 = val[0][lo];
					uint64		code = v0;
					uint64		blkbit;

					bits &= bits - 1;
					for (c = 1; c < ncol; c++)
					{
						if (gen[c][lo] != curgen)
							break;
						code = code * (uint64) radix[c] + val[c][lo];
					}
					if (c < ncol)
						continue;	/* no value of column c in the pass */

					blkbit = UINT64CONST(1) << (lo >> LION_OFFSET_BITS);
					if ((allvis & blkbit) != 0 && trust0[v0])
					{
						lion_decode_tally_add(tally, code, 1);
						counted |= blkbit;
						dstats->rows++;
					}
					else
						lion_decode_recheck_add(&rc, key, lo, code);
				}
			}

			/*
			 * We are not visiting the heap for the blocks counted from the
			 * map, so lock them as an index-only scan would.
			 */
			if (base.serializable)
			{
				uint64		m = counted;

				while (m != 0)
				{
					int			bb = pg_rightmost_one_pos64(m);

					m &= m - 1;
					PredicateLockPage(heap, firstblk + (BlockNumber) bb,
									  snapshot);
				}
			}
			base.stats.blocks_skipped_via_vm += pg_popcount64(counted);
		}
		dstats->keys++;

		/*
		 * Only now may the cursors here let go of their pages (§9), and each
		 * goes to the next key a count can be made at: the copy's next one -
		 * a step where that is the next key, a seek past the keys between
		 * otherwise (DESIGN.md §22) - or, with no WHERE, the next it has.
		 */
		if (mat != NULL && idx + 1 >= mat->ncontainers)
			break;
		for (i = 0; i < nhot; i++)
		{
			int			k = hot[i];

			if (mat != NULL && lion_mat_key(mat, idx + 1) != key + 1)
				lion_cursor_seek(&cur[k], lion_mat_key(mat, idx + 1));
			else
				lion_cursor_next(&cur[k]);
			if (cur[k].valid)
			{
				if (curcol[k] == 0)
					lion_group_heap_push(heap0, &nheap0, cur[k].cur->ckey, k);
				else
					lion_group_heap_push(heapr, &nheapr, cur[k].cur->ckey, k);
			}
		}
		CHECK_FOR_INTERRUPTS();
	}

	for (i = 0; i < ncur; i++)
		lion_cursor_close(&cur[i]);

	/* no index page is pinned any more: the heap answers the rest */
	lion_decode_recheck_flush(&rc);

	if (stats != NULL)
	{
		lion_count_stats_add(stats, &base.stats);
		for (c = 0; c < ncol; c++)
			lion_count_stats_add(stats, &ccx[c].stats);
	}
	for (c = 0; c < ncol; c++)
	{
		if (BufferIsValid(ccx[c].vmbuf))
			ReleaseBuffer(ccx[c].vmbuf);
	}
	if (BufferIsValid(base.vmbuf))
	{
		if (cache != NULL && !BufferIsValid(cache->vmbuf))
		{
			cache->vmbuf = base.vmbuf;
			cache->vmrelid = RelationGetRelid(heap);
		}
		else
			ReleaseBuffer(base.vmbuf);
	}
	dstats->spills += tally->nspills;
	tally->nspills = 0;

	MemoryContextSwitchTo(oldcxt);
	MemoryContextDelete(cxt);
}

/* ---------------------------------------------------------------------
 * Counts by cells
 * --------------------------------------------------------------------- */

/* Do the keys whose bits mask holds satisfy tree n, an AND and OR of keys? */
static bool
lion_cells_tree(const LionKeyNode *n, uint64 mask)
{
	int			i;

	switch (n->kind)
	{
		case LION_KN_KEY:
			Assert(n->keyno >= 0 && n->keyno < 63);
			return (mask & (UINT64CONST(1) << n->keyno)) != 0;
		case LION_KN_AND:
			for (i = 0; i < n->nargs; i++)
			{
				if (!lion_cells_tree(n->args[i], mask))
					return false;
			}
			return true;
		case LION_KN_OR:
			for (i = 0; i < n->nargs; i++)
			{
				if (lion_cells_tree(n->args[i], mask))
					return true;
			}
			return false;
		default:
			elog(ERROR, "lion index count: a key tree of kind %d in a count by cells",
				 (int) n->kind);
	}
	return false;					/* keep the compiler quiet */
}

/*
 * The outcome of a row of multi-key dimension dim whose labels' bits are
 * mask: NULL's, for a row of its NULL entry; otherwise the bit of each leaf
 * whose key tree the row's keys satisfy - none for a row of no key, the
 * EMPTY entry's among them.
 */
int
lion_cells_outcome(const LionCellDim *dim, uint64 mask)
{
	int			out = 0;
	int			b;

	Assert(dim->multi);
	if ((mask & LION_CELL_NULLBIT) != 0)
		return dim->nullout;
	if (dim->map != NULL)
		return dim->map[mask];
	for (b = 0; b < dim->nbit; b++)
	{
		if (lion_cells_tree(dim->trees[b], mask))
			out |= 1 << b;
	}
	return out;
}

/*
 * COUNTS BY CELLS (DESIGN.md §10, "Counts by cells"): the driver's sets in
 * one pass of its decoded walk, every row of them the WHERE keeps labelled
 * by the dimensions and counted under its cell's code.
 *
 * THE WALK is lion_count_groups_decode()'s with one column: the driver's
 * cursors keep their pins and drive, the copy of the WHERE says which of
 * their rows count, and the map is asked at a key once every driver cursor
 * standing there has copied its container.  The labels take the place of the
 * other columns: pinless cursors, each sought to the key and decoded there.
 * A scalar dimension's labels are entries of one index, disjoint, so a row
 * is in one of them at most and its outcome is added to its code as the
 * label is decoded; a row in none has outcome 0, which adds nothing.  A
 * multi-key dimension's row may be in several of its keys: their bits are
 * gathered first, a generation stamping each row's, and the outcome they
 * make is added after the dimension's last label.
 *
 * WHY IT IS EXACT is the decoded walk's argument (lion_count_groups_decode()),
 * with labels for columns.  Every row counted is in exactly one pinned set of
 * the driver - its entry of the driver's column, located for the pass under
 * the entry's pin; and a row is labelled only by sets read after the snapshot
 * was taken, which hold every row the snapshot sees that the label's value or
 * key is that of: the row was in every index before its transaction
 * committed, and it is not removed from any while the snapshot sees it.
 * What a label lists that the snapshot does not see is a dead row, on a heap
 * page VACUUM has not set all-visible while the driver's container still
 * holds it (the decoded walk's argument again), so it goes to the heap and
 * is not counted.  The rows of a NOPIN set of the driver, of one read from a
 * copy, and every row in recovery or under a row filter go to the heap as
 * well; so does a row whose code is past the tally's, which only a dead row
 * in two entries of one column can have, and which the recheck refuses to
 * find visible.
 */
void
lion_count_cells(Relation heap, Snapshot snapshot,
				 const LionDecodeCol *driver, const uint64 *drvadd,
				 int ndim, const LionCellDim *dims,
				 int nlabel, const LionCellLabel *labels,
				 uint64 ncells, uint32 lokey, uint64 hikey,
				 const LionPostingSet *where, LionDecodeTally *tally,
				 LionCountStats *stats, LionDecodeStats *dstats,
				 LionVisCache *cache, bool rel_read_only)
{
	const LionMatSet *mat = NULL;
	MemoryContext cxt;
	MemoryContext oldcxt;
	LionCountCtx base;
	LionCountCtx dcx;
	LionCountCtx lcx;
	LionDecodeRecheck rc;
	int			nd = driver->nsets;
	int			ntotal = nd + nlabel;
	LionSetCursor *cur;
	int		   *curlab;
	int		   *curval;
	int			ncur = 0;
	LionGroupEnt *heap0;
	LionGroupEnt *heapr;
	int			nheap0 = 0;
	int			nheapr = 0;
	int		   *hot;
	bool	   *trust0;
	uint64	   *present;
	uint64	   *rows;
	uint64	   *wimg;
	uint16	   *members;
	uint32	   *val0;
	uint64	   *code;
	uint64	   *acc;
	uint32	   *gen;
	uint16	   *touched;
	uint32		curgen = 0;
	LionContainer *buf = NULL;
	uint32		firstkey = lokey;
	int			idx = 0;
	int			v;
	int			i;

	Assert(ndim >= 1);
	Assert(tally->ncells == ncells);
	if (where != NULL)
	{
		if (!where->found || where->mat == NULL ||
			where->mat->ncontainers == 0)
			return;				/* the WHERE selects nothing */
		mat = where->mat;
		Assert(mat->collected);
		firstkey = Max(firstkey, lion_mat_key(mat, 0));
	}
	if (nd <= 0 || (uint64) firstkey >= hikey)
		return;

	cxt = AllocSetContextCreate(CurrentMemoryContext,
								"lion index count by cells",
								ALLOCSET_DEFAULT_SIZES);
	oldcxt = MemoryContextSwitchTo(cxt);

	if (cache != NULL && cache->windowed)
		elog(ERROR, "lion index count: a count by cells confined to a window of keys");

	/* lion_count_groups_decode()'s set-up, for one column and the labels */
	memset(&base, 0, sizeof(base));
	base.cxt = cxt;
	base.heap = heap;
	base.snapshot = snapshot;
	base.vmbuf = InvalidBuffer;
	if (cache != NULL && BufferIsValid(cache->vmbuf))
	{
		if (cache->vmrelid == RelationGetRelid(heap))
		{
			base.vmbuf = cache->vmbuf;
			cache->vmbuf = InvalidBuffer;
			cache->vmrelid = InvalidOid;
		}
		else
			lion_vis_cache_release_vm(cache);
	}
	base.serializable = IsolationIsSerializable();
	base.in_recovery = RecoveryInProgress();
	base.rel_read_only = rel_read_only;
	base.raw = true;
	base.tids_sorted = true;
	lion_vis_cache_begin(cache, heap, snapshot);
	base.cache = cache;
	base.filter = (cache != NULL) ? cache->filter : NULL;
	if (base.filter != NULL && base.filter->heap != heap)
		elog(ERROR, "lion index count: a row filter for another relation");
	base.batchmax = lion_recheck_budget_for(LION_DECODE_RECHECK_ENTRY);

	memset(&rc, 0, sizeof(rc));
	rc.cx = &base;
	rc.tally = tally;
	rc.dstats = dstats;

	cur = (LionSetCursor *) palloc(sizeof(LionSetCursor) * ntotal);
	curlab = (int *) palloc(sizeof(int) * ntotal);
	curval = (int *) palloc(sizeof(int) * ntotal);
	heap0 = (LionGroupEnt *) palloc(sizeof(LionGroupEnt) * ntotal);
	heapr = (LionGroupEnt *) palloc(sizeof(LionGroupEnt) * ntotal);
	hot = (int *) palloc(sizeof(int) * ntotal);
	trust0 = (bool *) palloc(sizeof(bool) * nd);
	present = (uint64 *) palloc(LION_BITSET_BYTES);
	rows = (uint64 *) palloc(LION_BITSET_BYTES);
	wimg = (uint64 *) palloc(LION_BITSET_BYTES);
	members = (uint16 *) palloc(sizeof(uint16) * LION_CONTAINER_RANGE);
	val0 = (uint32 *) palloc(sizeof(uint32) * LION_CONTAINER_RANGE);
	code = (uint64 *) palloc(sizeof(uint64) * LION_CONTAINER_RANGE);
	acc = (uint64 *) palloc(sizeof(uint64) * LION_CONTAINER_RANGE);
	gen = (uint32 *) palloc0(sizeof(uint32) * LION_CONTAINER_RANGE);
	touched = (uint16 *) palloc(sizeof(uint16) * LION_CONTAINER_RANGE);
	if (mat != NULL && lion_mat_spills(mat))
		buf = (LionContainer *) palloc(MAXALIGN(LION_CONTAINER_MAX_SIZE));

	/* the driver's cursors keep their pins; the labels' let go of each page */
	dcx = base;
	dcx.vmbuf = InvalidBuffer;
	dcx.droppins = false;
	lcx = base;
	lcx.vmbuf = InvalidBuffer;
	lcx.droppins = true;
	for (v = 0; v < nd; v++)
	{
		LionPostingSet *ps = &driver->sets[v];

		trust0[v] = ps->found && !ps->nopin && ps->mat == NULL;
		if (!ps->found)
			continue;
		lion_cursor_init_at(&cur[ncur], ps, &dcx, false, firstkey,
							driver->images != NULL ? &driver->images[v] : NULL);
		curlab[ncur] = -1;
		curval[ncur] = v;
		if (cur[ncur].valid)
			lion_group_heap_push(heap0, &nheap0, cur[ncur].cur->ckey, ncur);
		ncur++;
	}
	for (v = 0; v < nlabel; v++)
	{
		if (!labels[v].set->found)
			continue;
		Assert(v == 0 || labels[v].dim >= labels[v - 1].dim);
		lion_cursor_init_at(&cur[ncur], labels[v].set, &lcx, true, firstkey,
							labels[v].image);
		curlab[ncur] = v;
		curval[ncur] = -1;
		if (cur[ncur].valid)
			lion_group_heap_push(heapr, &nheapr, cur[ncur].cur->ckey, ncur);
		ncur++;
	}

	while (nheap0 > 0)
	{
		uint32		key = heap0[0].ckey;
		BlockNumber firstblk = lion_ckey_first_block(key);
		const uint64 *wbits = NULL;
		int			nhot0;
		int			nhot = 0;
		uint64		wanted = 0;
		uint64		allvis;
		uint64		counted = 0;
		int			b;

		if ((uint64) key >= hikey)
			break;

		/* the copy at or past the smallest key the driver stands at */
		if (mat != NULL)
		{
			base.stats.copy_seeks++;
			idx = lion_mat_seek(mat, idx, key);
			if (idx >= mat->ncontainers)
				break;
			if (lion_mat_key(mat, idx) != key)
			{
				uint32		target = lion_mat_key(mat, idx);

				while (nheap0 > 0 && heap0[0].ckey < target)
				{
					LionGroupEnt e = lion_group_heap_pop(heap0, &nheap0);

					lion_cursor_seek(&cur[e.g], target);
					if (cur[e.g].valid)
						lion_group_heap_push(heap0, &nheap0,
											 cur[e.g].cur->ckey, e.g);
				}
				CHECK_FOR_INTERRUPTS();
				continue;
			}
		}

		/* the driver's cursors at the key, and the labels sought to it */
		while (nheap0 > 0 && heap0[0].ckey == key)
			hot[nhot++] = lion_group_heap_pop(heap0, &nheap0).g;
		nhot0 = nhot;
		while (nheapr > 0 && heapr[0].ckey < key)
		{
			LionGroupEnt e = lion_group_heap_pop(heapr, &nheapr);

			lion_cursor_seek(&cur[e.g], key);
			if (cur[e.g].valid)
				lion_group_heap_push(heapr, &nheapr, cur[e.g].cur->ckey, e.g);
		}
		while (nheapr > 0 && heapr[0].ckey == key)
			hot[nhot++] = lion_group_heap_pop(heapr, &nheapr).g;

		/* ... the labels in their order, so a dimension's come together */
		for (i = nhot0 + 1; i < nhot; i++)
		{
			int			h = hot[i];
			int			j;

			for (j = i; j > nhot0 && curlab[hot[j - 1]] > curlab[h]; j--)
				hot[j] = hot[j - 1];
			hot[j] = h;
		}

		/* the driver: each row's value, its part of the code, and the rows */
		memset(present, 0, LION_BITSET_BYTES);
		for (i = 0; i < nhot0; i++)
		{
			int			k = hot[i];
			uint32		n = lion_container_to_array(cur[k].cur, members);
			uint64		add = drvadd[curval[k]];
			uint32		j;

			for (j = 0; j < n; j++)
			{
				uint16		lo = members[j];

				val0[lo] = (uint32) curval[k];
				code[lo] = add;
				present[lo >> 6] |= UINT64CONST(1) << (lo & 63);
			}
		}

		/* ... of them, the ones the WHERE keeps */
		if (mat != NULL)
		{
			const LionContainer *w =
				lion_count_posfilter(&base, lion_mat_container(&base, mat, idx, buf));

			if (w->type == LION_CT_BITSET)
				wbits = LION_BITSET_DATA(w);
			else
			{
				memset(wimg, 0, LION_BITSET_BYTES);
				lion_container_or_into_bitset(w, wimg);
				wbits = wimg;
			}
			for (i = 0; i < LION_BITSET_WORDS; i++)
				rows[i] = present[i] & wbits[i];
		}
		else
			memcpy(rows, present, LION_BITSET_BYTES);

		/* the labels, a dimension at a time */
		i = nhot0;
		while (i < nhot)
		{
			int			d = labels[curlab[hot[i]]].dim;
			const LionCellDim *dim = &dims[d];
			int			end = i;

			while (end < nhot && labels[curlab[hot[end]]].dim == d)
				end++;
			if (!dim->multi)
			{
				for (; i < end; i++)
				{
					int			k = hot[i];
					uint64		add = labels[curlab[k]].bits * dim->weight;
					uint32		n = lion_container_to_array(cur[k].cur, members);
					uint32		j;

					for (j = 0; j < n; j++)
						code[members[j]] += add;
				}
			}
			else
			{
				int			nt = 0;
				int			t;

				if (++curgen == 0)
				{
					memset(gen, 0, sizeof(uint32) * LION_CONTAINER_RANGE);
					curgen = 1;
				}
				for (; i < end; i++)
				{
					int			k = hot[i];
					uint64		bits = labels[curlab[k]].bits;
					uint32		n = lion_container_to_array(cur[k].cur, members);
					uint32		j;

					for (j = 0; j < n; j++)
					{
						uint16		lo = members[j];

						if (gen[lo] != curgen)
						{
							gen[lo] = curgen;
							acc[lo] = 0;
							touched[nt++] = lo;
						}
						acc[lo] |= bits;
					}
				}
				for (t = 0; t < nt; t++)
				{
					uint16		lo = touched[t];

					if ((rows[lo >> 6] & (UINT64CONST(1) << (lo & 63))) != 0)
						code[lo] += (uint64) lion_cells_outcome(dim, acc[lo]) *
							dim->weight;
				}
			}
		}

		/* the heap blocks the rows are on */
		for (b = 0; b < LION_BLOCKS_PER_CONTAINER; b++)
		{
			const int	wpb = (1 << LION_OFFSET_BITS) / 64;
			uint64		any = 0;
			int			w;

			for (w = b * wpb; w < (b + 1) * wpb; w++)
				any |= rows[w];
			if (any != 0)
				wanted |= UINT64CONST(1) << b;
		}

		if (wanted != 0)
		{
			/* the map, once for the key, every driver container here copied */
			LION_INJECTION_POINT("lion-count-containers-pinned");
			if (base.in_recovery || lion_row_filter_heap(base.filter))
				allvis = 0;
			else
			{
				allvis = lion_vm_allvisible_mask(heap, firstblk, wanted,
												 &base.vmbuf,
												 &base.stats.vm_pins);
				base.stats.vm_checks++;
#ifdef LION_VM_MASK_CHECK
				lion_vm_mask_check(heap, firstblk, wanted, allvis,
								   &base.vmbuf);
#endif
			}

			/* each row's cell, counted or sent to the heap */
			for (i = 0; i < LION_BITSET_WORDS; i++)
			{
				uint64		bits = rows[i];

				while (bits != 0)
				{
					uint32		lo = (uint32) (i * 64 +
											   pg_rightmost_one_pos64(bits));
					uint64		c = code[lo];
					uint64		blkbit = UINT64CONST(1) << (lo >> LION_OFFSET_BITS);

					bits &= bits - 1;
					if (c >= ncells)
						lion_decode_recheck_add(&rc, key, lo, ncells);
					else if ((allvis & blkbit) != 0 && trust0[val0[lo]])
					{
						lion_decode_tally_add(tally, c, 1);
						counted |= blkbit;
						dstats->rows++;
					}
					else
						lion_decode_recheck_add(&rc, key, lo, c);
				}
			}

			/* the blocks counted from the map, locked as an index-only scan's */
			if (base.serializable)
			{
				uint64		m = counted;

				while (m != 0)
				{
					int			bb = pg_rightmost_one_pos64(m);

					m &= m - 1;
					PredicateLockPage(heap, firstblk + (BlockNumber) bb,
									  snapshot);
				}
			}
			base.stats.blocks_skipped_via_vm += pg_popcount64(counted);
		}
		dstats->keys++;

		/* only now do the cursors here move on (§9), as the decoded walk's */
		if (mat != NULL && idx + 1 >= mat->ncontainers)
			break;
		for (i = 0; i < nhot; i++)
		{
			int			k = hot[i];

			if (mat != NULL && lion_mat_key(mat, idx + 1) != key + 1)
				lion_cursor_seek(&cur[k], lion_mat_key(mat, idx + 1));
			else
				lion_cursor_next(&cur[k]);
			if (cur[k].valid)
			{
				if (curlab[k] < 0)
					lion_group_heap_push(heap0, &nheap0, cur[k].cur->ckey, k);
				else
					lion_group_heap_push(heapr, &nheapr, cur[k].cur->ckey, k);
			}
		}
		CHECK_FOR_INTERRUPTS();
	}

	for (i = 0; i < ncur; i++)
		lion_cursor_close(&cur[i]);

	/* no index page is pinned any more: the heap answers the rest */
	lion_decode_recheck_flush(&rc);

	if (stats != NULL)
	{
		lion_count_stats_add(stats, &base.stats);
		lion_count_stats_add(stats, &dcx.stats);
		lion_count_stats_add(stats, &lcx.stats);
	}
	if (BufferIsValid(dcx.vmbuf))
		ReleaseBuffer(dcx.vmbuf);
	if (BufferIsValid(lcx.vmbuf))
		ReleaseBuffer(lcx.vmbuf);
	if (BufferIsValid(base.vmbuf))
	{
		if (cache != NULL && !BufferIsValid(cache->vmbuf))
		{
			cache->vmbuf = base.vmbuf;
			cache->vmrelid = RelationGetRelid(heap);
		}
		else
			ReleaseBuffer(base.vmbuf);
	}
	dstats->spills += tally->nspills;
	tally->nspills = 0;

	MemoryContextSwitchTo(oldcxt);
	MemoryContextDelete(cxt);
}
