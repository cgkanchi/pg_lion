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

	if (ncells <= t->maxbytes / (sizeof(int64) * LION_DECODE_DENSE_SHARE))
	{
		t->dense = (int64 *) MemoryContextAllocZero(t->cxt,
													sizeof(int64) * ncells);
		t->hash = NULL;
	}
	else
	{
		t->dense = NULL;
		t->hash = lion_tally_create(t->cxt, 256, NULL);
		t->maxents = (uint32) Max(t->maxbytes / (2 * sizeof(LionTallyEnt)),
								  (Size) 1024);
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
	lion_tally_start_iterate(t->hash, &it);
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
			lion_tally_start_iterate(t->hash, &t->it);
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
			lion_decode_tally_add(rc->tally, rc->codes[i], 1);
			rc->dstats->rows++;
		}
	}
	rc->dstats->rechecked += cx->ntids;
	cx->ntids = 0;
}

/*
 * One row for the heap to decide on, with its combination.  The rows come key
 * by key and in lo order inside a key, so the list is in TID order; it is
 * rechecked, and emptied, once it holds its budget - at a block boundary, as
 * lion_recheck_add() does, and for the reason given there, which holds here:
 * the map has been asked about every block of it already, under the pins.
 */
static void
lion_decode_recheck_add(LionDecodeRecheck *rc, uint32 ckey, uint32 lo,
						uint64 code)
{
	LionCountCtx *cx = rc->cx;
	ItemPointerData tid;

	lion_code_to_tid(((uint64) ckey << LION_CONTAINER_BITS) | lo, &tid);
	if (cx->ntids >= cx->batchmax &&
		ItemPointerGetBlockNumber(&cx->tids[cx->ntids - 1]) !=
		ItemPointerGetBlockNumber(&tid))
		lion_decode_recheck_flush(rc);

	if (cx->ntids >= rc->cap)
	{
		MemoryContext oldcxt = MemoryContextSwitchTo(cx->cxt);
		int			newcap = (rc->cap == 0) ? 1024 : rc->cap * 2;

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
	base.batchmax = lion_recheck_budget();

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
			const LionContainer *w = lion_mat_container(&base, mat, idx, buf);

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
			if (base.in_recovery || base.filter != NULL)
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
