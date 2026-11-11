/*-------------------------------------------------------------------------
 *
 * lion_tidbatch.c
 *		A bitmap scan's TIDs of many keys, put in heap order before they go
 *		into the TIDBitmap (DESIGN.md §28, "Many keys into one bitmap").
 *
 * A TIDBitmap finds a heap page's entry by hashing its block number, and
 * tbm_add_tuples() looks the entry up once for each run of TIDs on the same
 * page.  One key's posting set comes in TID order, so its TIDs reach the
 * bitmap a page at a time.  A walk of many keys - a range, a list, every key
 * of a column - emits them key after key instead, and the keys of a column in
 * no heap order spread their few rows over every page: each TID is then a run
 * of its own, a lookup in a table of every page the scan has touched, and a
 * cache miss.
 *
 * So the walk hands its items here.  Their members are kept by container key
 * (the 64 heap blocks of one container at 8K, DESIGN.md §2), appended as the
 * uint16 lo values of an array, and go into the bitmap a heap block at a time
 * when the walk is over - or before, whenever what is kept outgrows work_mem,
 * the TIDBitmap's own budget.  That bound is the arena blocks handed out, so
 * it is passed by at most one item's slots and array; a sparse item whose new
 * directory chunks would pass it flushes what is kept first, and one that
 * could not fit at all goes at once.  A container key's slot is found
 * through a two-level directory of slots, not a hash: container keys are
 * small and dense, and so are the slots, a 5M-row table's in 21 kB.  All of
 * it is in an arena of anonymous memory, outside malloc's heap
 * (lion_tid_batch_map() says why).
 *
 * An array that would outgrow LION_ARRAY_MAX_CARD members, the size of a 4 KB
 * bitset of the container's TIDs, is FOLDED into one, and from then on the
 * array is a buffer of LION_TID_BATCH_PEND members folded in whenever it
 * fills.  Setting each member's bit as it came would be a cache miss a TID
 * again once the bitsets outgrow the cache - 3.5 MB of them for a 5M-row
 * table - where an append writes to the one cache line at the array's end,
 * and a fold touches the bitset's 64 lines once for hundreds of members.
 *
 * What goes to the bitmap at once:
 *
 *	- an item of LION_TID_BATCH_DIRECT members or more, already a few TIDs a
 *	  page;
 *	- everything, while the bitmap has been given fewer TIDs than the heap has
 *	  blocks (lion_tid_batch_begin()'s nblocks).  Batching saves the lookups
 *	  of a page the bitmap has already been given, and until then most TIDs
 *	  are a page's first;
 *	- everything after that, when the smaller items of those first TIDs came
 *	  LION_TID_BATCH_IN_ORDER or more a run of one heap block: the walk is in
 *	  heap order already, as on a column the rows were appended in the order
 *	  of, and its TIDs reach the bitmap a page at a time without help;
 *	- everything after a flush that found barely more TIDs than pages: a heap
 *	  far larger than work_mem, whose rows the walk spreads thinner than one a
 *	  page between two flushes.
 *
 * Nothing here reads an index page: the items are the walk's copies.  A
 * damaged item yields what iterating it yields (lion_container.h, "DAMAGED
 * INPUT"), every lo below LION_CONTAINER_RANGE, so no index here leaves its
 * array; an offset the heap cannot have is tbm_add_tuples()'s to refuse, as
 * it is on the direct path.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "miscadmin.h"
#include "nodes/tidbitmap.h"
#include "port/pg_bitutils.h"
#include "utils/memutils.h"

#include "lion.h"
#include "lion_arena.h"
#include "lion_sparse.h"

/*
 * An item of at least this many members goes to the bitmap at once: spread
 * over the container's LION_BLOCKS_PER_CONTAINER blocks it is 8 TIDs a block
 * on average at 8K, where a lookup no longer costs much a TID.
 */
#define LION_TID_BATCH_DIRECT	(LION_BLOCKS_PER_CONTAINER * 8)

/* A flush must find this many TIDs a heap block for batching to go on. */
#define LION_TID_BATCH_MIN_GAIN	2

/*
 * A walk whose small items came this many TIDs or more a run of the same heap
 * block while the bitmap was warming up gives its TIDs in heap order already.
 */
#define LION_TID_BATCH_IN_ORDER	4

/* The directory: chunks of 1 << LION_TID_BATCH_DIR_BITS container keys. */
#define LION_TID_BATCH_DIR_BITS	10
#define LION_TID_BATCH_DIR_SIZE	(1 << LION_TID_BATCH_DIR_BITS)
#define LION_TID_BATCH_DIR_MASK	(LION_TID_BATCH_DIR_SIZE - 1)

/* The first array a container key gets, in members. */
#define LION_TID_BATCH_ARRAY_INIT	8

/* A folded container key's array, in members. */
#define LION_TID_BATCH_PEND		512

/* The heap offsets one block can have in a lo value. */
#define LION_TID_BATCH_BLOCK_TIDS	(1 << LION_OFFSET_BITS)

/*
 * What a batch keeps at most, whatever work_mem says, and the arena's
 * regions: doubling from a grain to LION_ARENA_REGION_MAX, they map 1.6 GB,
 * room for twice that, a block given back for each one kept.
 */
#define LION_TID_BATCH_MAXBYTES	((Size) 512 * 1024 * 1024)
#define LION_TID_BATCH_REGIONS	32

/* The members of one container key; all zeros while it has none. */
typedef struct LionTidSlot
{
	uint32		n;				/* members in lo[], repeats counted */
	uint32		cap;			/* room in lo[] */
	uint16	   *lo;				/* members not folded into bits[] yet */
	uint64	   *bits;			/* LION_BITSET_WORDS, or NULL: none folded */
} LionTidSlot;

/* What one directory chunk of slots takes from the arena. */
#define LION_TID_BATCH_CHUNK \
	lion_arena_class_size(lion_arena_class(sizeof(LionTidSlot) * \
										   LION_TID_BATCH_DIR_SIZE))

/* What a flush and an item's members are put through; in the arena. */
typedef struct LionTidScratch
{
	uint16		members[LION_TID_BATCH_DIRECT];	/* an item's */
	uint16		sorted[LION_ARRAY_MAX_CARD];	/* a slot's array, by block */
	ItemPointerData tids[LION_TID_BATCH_BLOCK_TIDS];	/* one block's */
} LionTidScratch;

/* A batch's memory, and what unmaps it however the walk ends. */
typedef struct LionTidArena
{
	MemoryContextCallback unmap;
	LionArena	arena;
	LionArenaRegion regions[LION_TID_BATCH_REGIONS];
} LionTidArena;

struct LionTidBatch
{
	LionTidBatchHead head;		/* first: lion_tid_batch_keeps() reads it */
	const char *name;			/* the index's, for the DEBUG1 summary */
	TIDBitmap  *tbm;
	bool		recheck;		/* passed on to tbm_add_tuples() */
	int64		warmup;			/* TIDs to give the bitmap before batching */
	BlockNumber lastblk;		/* while warming up: the last TID's block */
	int64		wtids;			/* ... the TIDs of items that would be kept */
	int64		wruns;			/* ... and their runs of the same block */
	Size		maxbytes;		/* work_mem, within LION_TID_BATCH_MAXBYTES */
	MemoryContext cxt;			/* holds ta, whose callback it runs when it
								 * goes; NULL until an item is first kept */
	LionTidArena *ta;			/* everything below, reset by a flush */
	LionTidScratch *scratch;	/* NULL again after a flush */
	Size		held;			/* the blocks the arena has handed out */
	LionTidSlot **dir;			/* dir[ckey >> BITS][ckey & MASK] */
	uint32		ndir;
	int64		pending;		/* TIDs kept since the last flush */
	int64		nkept;			/* TIDs kept */
	int			nflushes;
};

/*
 * A batch for a walk of index name into tbm, of a heap of nblocks blocks.  It
 * allocates nothing more until an item is kept.
 */
LionTidBatch *
lion_tid_batch_begin(const char *name, TIDBitmap *tbm, bool recheck,
					 BlockNumber nblocks)
{
	LionTidBatch *b = (LionTidBatch *) palloc0(sizeof(LionTidBatch));

	b->name = name;
	b->tbm = tbm;
	b->recheck = recheck;
	b->warmup = (int64) nblocks;
	b->lastblk = InvalidBlockNumber;
	b->maxbytes = Max(Min((Size) work_mem * 1024, LION_TID_BATCH_MAXBYTES),
					  LION_BITSET_BYTES * 16);
	return b;
}

static void
lion_tid_batch_unmap(void *arg)
{
	lion_arena_release(&((LionTidArena *) arg)->arena);
}

/*
 * The arena, for the first item kept: anonymous memory (lion_arena.h), in a
 * context of its own under b's, whose deletion - by lion_tid_batch_end(), or
 * with the query's memory if the walk fails - unmaps it.
 *
 * Not palloc.  glibc's malloc gives the top of its heap back to the kernel
 * once it is free and larger than twice the largest block malloc has
 * unmapped, and a TIDBitmap's page table, which doubles, each table made
 * before the last is freed, leaves a heap of about that size: the tables of
 * a bitmap as large as the backend's first add up to twice its largest.  A
 * few hundred kilobytes more on the heap tipped scans that had stayed just
 * under that line over it, and the next scan faulted every page of the heap
 * in again - 3,000 faults, 4 ms, on the 61,000-page heap of DESIGN.md §28,
 * more than a batch saves on a walk of one or two TIDs a page.  A mapping
 * of its own leaves malloc's heap as it was, and costs a fault for each page
 * the batch writes.
 */
static void
lion_tid_batch_map(LionTidBatch *b)
{
	MemoryContext cxt = AllocSetContextCreate(GetMemoryChunkContext(b),
											  "lion TID batch",
											  ALLOCSET_SMALL_SIZES);
	LionTidArena *ta = (LionTidArena *) MemoryContextAlloc(cxt,
														   sizeof(LionTidArena));

	lion_arena_init(&ta->arena, ta->regions, LION_TID_BATCH_REGIONS,
					NULL, NULL, NULL);
	/* registered before anything is mapped: it finds nothing to undo */
	ta->unmap.func = lion_tid_batch_unmap;
	ta->unmap.arg = ta;
	MemoryContextRegisterResetCallback(cxt, &ta->unmap);
	b->cxt = cxt;
	b->ta = ta;
}

/* size bytes from the arena, or the ERROR palloc() would raise */
static void *
lion_tid_alloc(LionTidBatch *b, Size size)
{
	void	   *p = lion_arena_alloc(&b->ta->arena, size);

	if (p == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("out of memory"),
				 errdetail("Failed to map memory for the TIDs of lion index \"%s\": %m.",
						   b->name)));
	b->held += lion_arena_class_size(lion_arena_class(size));
	return p;
}

/* the same size back */
static void
lion_tid_free(LionTidBatch *b, void *p, Size size)
{
	lion_arena_free(&b->ta->arena, p, size);
	b->held -= lion_arena_class_size(lion_arena_class(size));
}

/* The slot of ckey. */
static inline LionTidSlot *
lion_tid_slot(LionTidBatch *b, uint32 ckey)
{
	uint32		hi = ckey >> LION_TID_BATCH_DIR_BITS;

	if (hi >= b->ndir)
	{
		uint32		n = Max(hi + 1, b->ndir * 2);
		LionTidSlot **dir;

		dir = (LionTidSlot **) lion_tid_alloc(b, sizeof(LionTidSlot *) * n);
		memset(dir + b->ndir, 0, sizeof(LionTidSlot *) * (n - b->ndir));
		if (b->ndir > 0)
		{
			memcpy(dir, b->dir, sizeof(LionTidSlot *) * b->ndir);
			lion_tid_free(b, b->dir, sizeof(LionTidSlot *) * b->ndir);
		}
		b->dir = dir;
		b->ndir = n;
	}
	if (b->dir[hi] == NULL)
	{
		b->dir[hi] = (LionTidSlot *)
			lion_tid_alloc(b, sizeof(LionTidSlot) * LION_TID_BATCH_DIR_SIZE);
		memset(b->dir[hi], 0, sizeof(LionTidSlot) * LION_TID_BATCH_DIR_SIZE);
	}
	return &b->dir[hi][ckey & LION_TID_BATCH_DIR_MASK];
}

/*
 * The directory chunks sparse item c has pairs in: all of them in *all, and
 * as the result those b has no slots in yet.  Its pairs come sorted by code,
 * so a chunk's come together; a damaged item counts what iterating it yields.
 */
static uint32
lion_tid_sparse_chunks(const LionTidBatch *b, const LionContainer *c,
					   uint32 *all)
{
	LionSparseIter it;
	uint32		last = PG_UINT32_MAX;
	uint32		fresh = 0;

	*all = 0;
	lion_sparse_iter_init(&it, c);
	while (lion_sparse_iter_next(&it))
	{
		uint32		hi = lion_sparse_iter_ckey(&it) >> LION_TID_BATCH_DIR_BITS;

		if (hi == last)
			continue;
		last = hi;
		(*all)++;
		if (hi >= b->ndir || b->dir[hi] == NULL)
			fresh++;
	}
	return fresh;
}

/*
 * Fold s's array into its bitset, made if it has none - and the array, the
 * first time, made LION_TID_BATCH_PEND members long.
 */
static void
lion_tid_slot_fold(LionTidBatch *b, LionTidSlot *s)
{
	uint32		i;

	if (s->bits == NULL)
	{
		s->bits = (uint64 *) lion_tid_alloc(b, LION_BITSET_BYTES);
		memset(s->bits, 0, LION_BITSET_BYTES);
	}
	for (i = 0; i < s->n; i++)
		s->bits[s->lo[i] >> 6] |= UINT64CONST(1) << (s->lo[i] & 63);
	s->n = 0;
	if (s->cap != LION_TID_BATCH_PEND)
	{
		if (s->lo != NULL)
			lion_tid_free(b, s->lo, sizeof(uint16) * s->cap);
		s->lo = (uint16 *) lion_tid_alloc(b, sizeof(uint16) * LION_TID_BATCH_PEND);
		s->cap = LION_TID_BATCH_PEND;
	}
}

/*
 * Room in s's array for n more members: the array grown, while it has no
 * bitset and the members fit an ARRAY container; false when they do not.
 */
static bool
lion_tid_slot_grow(LionTidBatch *b, LionTidSlot *s, uint32 n)
{
	uint32		cap;
	uint16	   *lo;

	if (s->n + n <= s->cap)
		return true;
	if (s->bits != NULL || s->n + n > LION_ARRAY_MAX_CARD)
		return false;
	cap = Max(s->cap, LION_TID_BATCH_ARRAY_INIT);
	while (cap < s->n + n)
		cap *= 2;
	cap = Min(cap, LION_ARRAY_MAX_CARD);
	lo = (uint16 *) lion_tid_alloc(b, sizeof(uint16) * cap);
	if (s->lo != NULL)
	{
		memcpy(lo, s->lo, sizeof(uint16) * s->n);
		lion_tid_free(b, s->lo, sizeof(uint16) * s->cap);
	}
	s->lo = lo;
	s->cap = cap;
	return true;
}

static inline void
lion_tid_slot_put(LionTidBatch *b, LionTidSlot *s, uint16 lo)
{
	if (s->n == s->cap && !lion_tid_slot_grow(b, s, 1))
		lion_tid_slot_fold(b, s);
	s->lo[s->n++] = lo;
}

/* n members of one container, ascending: appended, or set in its bitset. */
static void
lion_tid_slot_put_many(LionTidBatch *b, LionTidSlot *s, const uint16 *lo,
					   uint32 n)
{
	uint32		i;

	if (lion_tid_slot_grow(b, s, n))
	{
		memcpy(s->lo + s->n, lo, sizeof(uint16) * n);
		s->n += n;
		return;
	}
	if (s->bits == NULL)
		lion_tid_slot_fold(b, s);
	for (i = 0; i < n; i++)
		s->bits[lo[i] >> 6] |= UINT64CONST(1) << (lo[i] & 63);
}

/*
 * Emit one slot into the bitmap, one tbm_add_tuples() call a heap block
 * (two for a block whose repeated members outnumber its offsets).  Returns
 * the number of blocks.
 */
static int64
lion_tid_slot_emit(LionTidBatch *b, LionTidSlot *s, uint32 ckey)
{
	ItemPointerData *tids = b->scratch->tids;
	BlockNumber firstblk = lion_ckey_first_block(ckey);
	int64		blocks = 0;
	uint32		blk;
	uint32		i;

	if (s->bits != NULL)
	{
		for (i = 0; i < s->n; i++)
			s->bits[s->lo[i] >> 6] |= UINT64CONST(1) << (s->lo[i] & 63);
		for (blk = 0; blk < LION_BLOCKS_PER_CONTAINER; blk++)
		{
			const uint64 *w = s->bits + blk * LION_BITSET_WIDTH;
			int			n = 0;
			int			j;

			for (j = 0; j < LION_BITSET_WIDTH; j++)
			{
				uint64		word = w[j];

				while (word != 0)
				{
					ItemPointerSet(&tids[n++], firstblk + blk,
								   (OffsetNumber) (j * 64 +
												   pg_rightmost_one_pos64(word)));
					word &= word - 1;
				}
			}
			if (n > 0)
			{
				tbm_add_tuples(b->tbm, tids, n, b->recheck);
				blocks++;
			}
		}
	}
	else
	{
		uint16	   *sorted = b->scratch->sorted;
		uint32		start[LION_BLOCKS_PER_CONTAINER + 1];
		uint32		pos[LION_BLOCKS_PER_CONTAINER];

		/* a counting sort by block: the order within a block is no matter */
		memset(start, 0, sizeof(start));
		for (i = 0; i < s->n; i++)
			start[(s->lo[i] >> LION_OFFSET_BITS) + 1]++;
		for (blk = 0; blk < LION_BLOCKS_PER_CONTAINER; blk++)
		{
			start[blk + 1] += start[blk];
			pos[blk] = start[blk];
		}
		for (i = 0; i < s->n; i++)
			sorted[pos[s->lo[i] >> LION_OFFSET_BITS]++] = s->lo[i];

		for (blk = 0; blk < LION_BLOCKS_PER_CONTAINER; blk++)
		{
			int			n = 0;

			if (start[blk + 1] == start[blk])
				continue;
			for (i = start[blk]; i < start[blk + 1]; i++)
			{
				ItemPointerSet(&tids[n++], firstblk + blk,
							   (OffsetNumber) (sorted[i] & LION_MAX_OFFSET));
				if (n == LION_TID_BATCH_BLOCK_TIDS)
				{
					tbm_add_tuples(b->tbm, tids, n, b->recheck);
					n = 0;
				}
			}
			if (n > 0)
				tbm_add_tuples(b->tbm, tids, n, b->recheck);
			blocks++;
		}
	}
	return blocks;
}

/*
 * Everything kept, into the bitmap, and the memory given back.  A flush
 * before the end that found barely more TIDs than heap blocks ends the
 * batching.
 */
static void
lion_tid_batch_flush(LionTidBatch *b, bool last)
{
	int64		blocks = 0;
	uint32		hi;

	if (b->ta == NULL)
		return;
	for (hi = 0; hi < b->ndir; hi++)
	{
		LionTidSlot *chunk = b->dir[hi];
		uint32		j;

		if (chunk == NULL)
			continue;
		for (j = 0; j < LION_TID_BATCH_DIR_SIZE; j++)
		{
			if (chunk[j].n > 0 || chunk[j].bits != NULL)
				blocks += lion_tid_slot_emit(b, &chunk[j],
											 (hi << LION_TID_BATCH_DIR_BITS) | j);
		}
		CHECK_FOR_INTERRUPTS();
	}
	b->nflushes++;
	if (!last && b->pending < LION_TID_BATCH_MIN_GAIN * blocks)
		b->head.direct = true;

	if (b->head.direct)
	{
		/* nothing more will be kept: unmapped now */
		MemoryContextDelete(b->cxt);
		b->cxt = NULL;
		b->ta = NULL;
	}
	else
		lion_arena_reset(&b->ta->arena);
	b->held = 0;
	b->dir = NULL;
	b->ndir = 0;
	b->scratch = NULL;
	b->pending = 0;
}

/*
 * Take one item, a container or a sparse segment, as lion_container_to_tbm()
 * would emit it.  Returns the number of members taken.
 */
int64
lion_tid_batch_add(LionTidBatch *b, const LionContainer *c)
{
	int64		n = 0;
	bool		big;

	if (b->head.direct)
		return lion_container_to_tbm(c, b->tbm, b->recheck);

	big = (c->type != LION_CT_SPARSE &&
		   lion_container_cardinality(c) >= LION_TID_BATCH_DIRECT);
	if (b->warmup > 0)
	{
		int64		runs = 0;

		n = lion_container_to_tbm_runs(c, b->tbm, b->recheck, &b->lastblk,
									   &runs);
		b->warmup -= n;
		if (!big)
		{
			b->wtids += n;
			b->wruns += runs;
		}
		/* the walk's TIDs already come a heap block at a time */
		if (b->warmup <= 0 && b->wtids > 0 &&
			b->wtids >= LION_TID_BATCH_IN_ORDER * b->wruns)
			b->head.direct = true;
		return n;
	}
	if (big)
		return lion_container_to_tbm(c, b->tbm, b->recheck);

	/*
	 * A sparse item can span directory chunks it has no slots in yet, each of
	 * them LION_TID_BATCH_CHUNK bytes.  When they would take what is kept past
	 * maxbytes, flush first; an item that could not fit even then goes at once.
	 */
	if (c->type == LION_CT_SPARSE)
	{
		uint32		all;
		uint32		fresh = lion_tid_sparse_chunks(b, c, &all);

		if (fresh > 0 && b->held + fresh * LION_TID_BATCH_CHUNK > b->maxbytes)
		{
			if (b->pending > 0)
				lion_tid_batch_flush(b, false);
			if (b->head.direct || all * LION_TID_BATCH_CHUNK > b->maxbytes)
				return lion_container_to_tbm(c, b->tbm, b->recheck);
		}
	}

	if (b->ta == NULL)
		lion_tid_batch_map(b);
	if (b->scratch == NULL)
		b->scratch = (LionTidScratch *) lion_tid_alloc(b, sizeof(LionTidScratch));

	if (c->type == LION_CT_SPARSE)
	{
		LionSparseIter it;

		lion_sparse_iter_init(&it, c);
		while (lion_sparse_iter_next(&it))
		{
			lion_tid_slot_put(b, lion_tid_slot(b, lion_sparse_iter_ckey(&it)),
							  lion_sparse_iter_lo(&it));
			n++;
		}
	}
	else
	{
		uint32		k;

		/* more members than the header says only a damaged one has */
		k = lion_container_extract_members(c, b->scratch->members,
										   LION_TID_BATCH_DIRECT);
		if (k > LION_TID_BATCH_DIRECT)
			return lion_container_to_tbm(c, b->tbm, b->recheck);
		if (k > 0)
			lion_tid_slot_put_many(b, lion_tid_slot(b, c->ckey),
								   b->scratch->members, k);
		n = k;
	}

	b->pending += n;
	b->nkept += n;
	if (b->held > b->maxbytes)
		lion_tid_batch_flush(b, false);
	return n;
}

/* Emit what is left and free the batch, of a walk that emitted ntids TIDs. */
void
lion_tid_batch_end(LionTidBatch *b, int64 ntids)
{
	lion_tid_batch_flush(b, true);
	if (b->nkept > 0)
		elog(DEBUG1, "lion index \"%s\": %lld of %lld TIDs into the bitmap by heap block, in %d flush%s%s",
			 b->name, (long long) b->nkept, (long long) ntids,
			 b->nflushes, b->nflushes == 1 ? "" : "es",
			 b->head.direct ? ", the rest at once" : "");
	if (b->cxt != NULL)
		MemoryContextDelete(b->cxt);	/* unmaps the arena */
	pfree(b);
}
