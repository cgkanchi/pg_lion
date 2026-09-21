/*-------------------------------------------------------------------------
 *
 * rbi_build.c
 *		ambuild for the roaring index (DESIGN.md section 5, BUILD).
 *
 * The heap is scanned once into a tuplesort of (hash int4, key, code int8,
 * kind int2) sorted by (hash, code).  A first pass over the sorted data counts
 * distinct keys so that the bucket count can be chosen; a second pass groups
 * the codes of each key into containers and writes the posting sets out,
 * either inline in the entry tuple or as a chain of container pages.
 *
 * A multi-key opclass (DESIGN.md §17) turns one heap row into one sort tuple
 * per distinct key it extracts, all carrying the same code; nothing else in
 * the build changes, because the sort orders the codes of each key
 * ascending either way.
 *
 * Every page is written through the bulk-write API (storage/bulk_write.h),
 * which is what nbtree and GiST builds use: pages are prepared in
 * backend-local memory, written straight to the file without going through
 * shared buffers, WAL-logged in batches of up to 32 as full-page images (or
 * not at all, for an unlogged relation or wal_level = minimal, in which case
 * the relation is registered for the next sync instead), and each page is
 * written exactly ONCE.  That last property is what makes it fast: the old
 * route through the buffer manager logged a GenericXLog record per entry
 * tuple, so a million-key index paid a million page diffs and a million WAL
 * records to fill 15,000 pages.
 *
 * Writing each page once means every page has to be final before it is
 * written, and a bucket page is only final when the last entry that hashes to
 * it has been added.  Bucket pages therefore live in memory - one 8 KB image
 * per bucket page that actually holds entries - until pass 2 is over
 * (rbi_build_flush_buckets()).  That is the one cost of this route: the
 * bucket directory is sized at about three quarters of a page per bucket, so
 * the images are roughly 1.3 times the bytes the entry tuples need, bounded
 * by RBI_MAX_BUCKETS pages (512 MB) in the worst case and by nothing else.
 * Container pages are final as soon as the next container does not fit, so
 * only one of those exists per open key at a time.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/generic_xlog.h"
#include "access/tableam.h"
#include "catalog/pg_operator_d.h"
#include "catalog/pg_type.h"
#include "executor/executor.h"
#include "executor/tuptable.h"
#include "miscadmin.h"
#include "nodes/execnodes.h"
#include "storage/bufmgr.h"
#include "storage/bulk_write.h"
#include "utils/datum.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/tuplesort.h"
#include "varatt.h"

#include "rbi.h"

/*
 * Bytes of entry tuples one bucket is expected to hold when ambuild chooses
 * the bucket count itself (DESIGN.md §5).  Three quarters of a page.
 */
#define RBI_BUCKET_FILL_BYTES	((Size) (BLCKSZ / 4 * 3))

/*
 * Which entry a sorted tuple belongs to.  A row contributes one tuple per
 * distinct extracted key, or exactly one RBI_KEY_NULL / RBI_KEY_EMPTY tuple
 * when it has no key at all; the two reserved kinds are told apart by this
 * flag rather than by the (absent) key, and they share hash 0 with any real
 * key that happens to hash there.
 */
#define RBI_KEY_REAL	0
#define RBI_KEY_NULL	1		/* the indexed value is NULL (DESIGN.md §14) */
#define RBI_KEY_EMPTY	2		/* no keys were extracted (DESIGN.md §17) */

static inline uint16
rbi_reserved_flag(int kind)
{
	Assert(kind == RBI_KEY_NULL || kind == RBI_KEY_EMPTY);
	return (kind == RBI_KEY_NULL) ? RBI_ENTRY_NULLKEY : RBI_ENTRY_EMPTYKEY;
}

/*
 * One posting set under construction.  Items are produced in ascending ckey
 * order and kept in an inline buffer until they no longer fit in
 * inline_limit bytes; after that the entry spills to a chain of container
 * pages that is written out page by page.
 *
 * Sparse segments (DESIGN.md §13) make the grouping two-stage.  The codes of
 * one key arrive sorted, so they arrive grouped by ckey; a group is only
 * known to be dense enough for a container of its own once its
 * RBI_SPARSE_THRESHOLD'th member shows up.  Until then its members wait in
 * pend[] -- deliberately *not* in the open segment, because a group that
 * turns out to be dense has to become a container placed after the segment,
 * and pairs already in the segment could not be taken back out without
 * breaking the ordering of the items.  Emitting a container therefore closes
 * the open segment first, which is what keeps item ranges from interleaving.
 */
typedef struct RBIBuilder
{
	Datum		key;			/* private copy of the key */
	int			keykind;		/* RBI_KEY_* */
	uint32		hash;

	bool		hasgroup;		/* curckey is a ckey group being collected */
	uint32		curckey;
	bool		hascur;			/* cur holds an unfinished container */
	RBIContainer *cur;			/* RBI_CONTAINER_MAX_SIZE work buffer */
	RBIContainer *cbuf;			/* scratch for re-reading packed items */

	uint16		pend[RBI_SPARSE_THRESHOLD];	/* members of a still-sparse ckey */
	int			npend;
	RBIContainer *seg;			/* open sparse segment, empty when none */

	char	   *inlinebuf;		/* buffered items, packed without padding */
	Size		inlineused;

	bool		spilled;		/* posting set lives on container pages */
	BlockNumber head;
	BlockNumber curblk;			/* block pagebuf will be written to */
	BulkWriteBuffer pagebuf;	/* container page under construction */
	bool		haspage;

	uint32		nitems;			/* containers and segments (entry.ncontainers) */
	uint64		ntids;
} RBIBuilder;

/*
 * One bucket page under construction.  The image is a bulk-write buffer, so
 * that writing it out at the end of the build hands the very same memory to
 * the bulk writer instead of copying it.
 */
typedef struct RBIBuildPage
{
	struct RBIBuildPage *next;	/* next page of this bucket's chain */
	struct RBIBuildPage *next2; /* next overflow page in block order */
	BlockNumber blkno;
	BulkWriteBuffer buf;		/* the image, NULL once it has been written */
} RBIBuildPage;

typedef struct RBIBuildState
{
	Relation	index;
	RBIState	state;
	uint32		nbuckets;
	uint32		inline_limit;

	bool		multikey;		/* the opclass extracts keys (DESIGN.md §17) */
	int			max_entries;	/* cardinality guard, 0 = unlimited */

	Tuplesortstate *sortstate;
	TupleDesc	sorttupdesc;
	TupleTableSlot *inslot;
	TupleTableSlot *outslot;

	MemoryContext buildctx;		/* lives for the whole build */
	MemoryContext tmpctx;		/* reset per heap tuple / per key group */

	/*
	 * Page writing (see the file header): one bulk writer for the whole
	 * build, blocks handed out by a counter instead of by extending the
	 * relation, and the bucket pages kept in memory until pass 2 is over.
	 */
	BulkWriteState *bulk;
	BlockNumber nblocks;		/* blocks handed out so far */
	struct RBIBuildPage **bucketpages;	/* head page of each bucket, or NULL */
	struct RBIBuildPage *overflow;		/* bucket pages beyond the heads, in */
	struct RBIBuildPage *overflowlast;	/* block order */
	int64		nbucketpages;

	RBIBuilder **builders;		/* open builders of the current hash */
	int			nbuilders;
	int			maxbuilders;

	double		indtuples;		/* TIDs pushed into the index */
} RBIBuildState;

static void rbi_build_callback(Relation index, ItemPointer tid, Datum *values,
							   bool *isnull, bool tupleIsAlive, void *arg);
static void rbi_builder_flush(RBIBuildState *bs, RBIBuilder *b);
static void rbi_builder_close_segment(RBIBuildState *bs, RBIBuilder *b);

/* ---------------------------------------------------------------------
 * Page writing
 *
 * During a build nobody else can see the index, so pages are prepared in
 * backend-local memory and handed to the bulk writer, which writes each of
 * them exactly once and WAL-logs them in batches.  Block numbers come from a
 * counter: the meta page is block 0, the bucket directory takes blocks
 * 1 .. nbuckets, and container pages and further bucket pages are handed the
 * next free block as they are needed - the same order the buffer-manager
 * route extended the relation in, so an index built by either route has the
 * same page at the same block.
 * --------------------------------------------------------------------- */

static BlockNumber
rbi_build_alloc_block(RBIBuildState *bs)
{
	return bs->nblocks++;
}

static BulkWriteBuffer
rbi_build_get_page(RBIBuildState *bs, uint16 flags)
{
	BulkWriteBuffer buf = smgr_bulk_get_buf(bs->bulk);

	rbi_init_page((Page) buf->data, flags);

	return buf;
}

/*
 * Write the meta page and reserve the bucket directory.  The head pages
 * themselves are written by rbi_build_flush_buckets() once their entries are
 * all in; until then the directory is a hole in the file that the bulk writer
 * fills with zeroes if a container page is written past it, and overwrites
 * with the real pages afterwards.
 */
static void
rbi_build_init_pages(RBIBuildState *bs)
{
	BulkWriteBuffer meta = smgr_bulk_get_buf(bs->bulk);
	BlockNumber blk;

	rbi_init_metapage((Page) meta->data, bs->nbuckets, bs->inline_limit);
	blk = rbi_build_alloc_block(bs);
	Assert(blk == RBI_METAPAGE_BLKNO);
	smgr_bulk_write(bs->bulk, blk, meta, true);

	bs->bucketpages = (RBIBuildPage **)
		MemoryContextAllocZero(bs->buildctx,
							   sizeof(RBIBuildPage *) * bs->nbuckets);
	bs->nblocks = RBI_BUCKET_BLKNO(bs->nbuckets);
}

/*
 * A new page for a bucket's chain.  prev is the page it is linked after, or
 * NULL for the bucket's head page, which owns a block of the directory.
 */
static RBIBuildPage *
rbi_build_new_bucket_page(RBIBuildState *bs, uint32 bucket, RBIBuildPage *prev)
{
	RBIBuildPage *bp = (RBIBuildPage *)
		MemoryContextAllocZero(bs->buildctx, sizeof(RBIBuildPage));

	bp->buf = rbi_build_get_page(bs, RBI_PAGE_BUCKET);
	bs->nbucketpages++;

	if (prev == NULL)
	{
		bp->blkno = RBI_BUCKET_BLKNO(bucket);
		bs->bucketpages[bucket] = bp;
	}
	else
	{
		bp->blkno = rbi_build_alloc_block(bs);
		RBIPageGetOpaque((Page) prev->buf->data)->rightlink = bp->blkno;
		prev->next = bp;

		/* Overflow pages are written in the order they were allocated. */
		if (bs->overflowlast == NULL)
			bs->overflow = bp;
		else
			bs->overflowlast->next2 = bp;
		bs->overflowlast = bp;
	}

	return bp;
}

/*
 * Add one entry tuple to its bucket, appending a bucket page when no page of
 * the chain has room.  This is rbi_add_entry() (rbi_pages.c) without the
 * locking and the WAL record: same walk, same PageAddItemExtended(), so the
 * pages come out byte for byte the same.
 */
static void
rbi_build_add_entry(RBIBuildState *bs, uint32 bucket, RBIEntryTuple *entry,
					Size size)
{
	Size		need = MAXALIGN(size);
	RBIBuildPage *bp = bs->bucketpages[bucket];

	if (bp == NULL)
		bp = rbi_build_new_bucket_page(bs, bucket, NULL);

	for (;;)
	{
		Page		page = (Page) bp->buf->data;

		Assert(RBIPageIsBucket(page));

		if (PageGetFreeSpace(page) >= need)
		{
			if (PageAddItemExtended(page, entry, size, InvalidOffsetNumber,
									0) == InvalidOffsetNumber)
				elog(ERROR, "roaring index: failed to add entry to bucket page");
			return;
		}

		if (bp->next == NULL)
			(void) rbi_build_new_bucket_page(bs, bucket, bp);
		bp = bp->next;

		CHECK_FOR_INTERRUPTS();
	}
}

/*
 * Write every bucket page: the head pages in bucket order first (an empty
 * bucket still owns its head page), then the overflow pages in block order.
 */
static void
rbi_build_flush_buckets(RBIBuildState *bs)
{
	RBIBuildPage *bp;
	uint32		b;

	for (b = 0; b < bs->nbuckets; b++)
	{
		bp = bs->bucketpages[b];

		if (bp == NULL)
			smgr_bulk_write(bs->bulk, RBI_BUCKET_BLKNO(b),
							rbi_build_get_page(bs, RBI_PAGE_BUCKET), true);
		else
		{
			Assert(bp->blkno == RBI_BUCKET_BLKNO(b));
			smgr_bulk_write(bs->bulk, bp->blkno, bp->buf, true);
			bp->buf = NULL;
		}

		CHECK_FOR_INTERRUPTS();
	}

	for (bp = bs->overflow; bp != NULL; bp = bp->next2)
	{
		Assert(bp->buf != NULL);
		smgr_bulk_write(bs->bulk, bp->blkno, bp->buf, true);
		bp->buf = NULL;
		CHECK_FOR_INTERRUPTS();
	}
}

/* ---------------------------------------------------------------------
 * Posting set builders
 * --------------------------------------------------------------------- */

static RBIBuilder *
rbi_builder_create(RBIBuildState *bs, Datum key, int keykind, uint32 hash)
{
	RBIBuilder *b = (RBIBuilder *) palloc0(sizeof(RBIBuilder));

	b->keykind = keykind;
	b->key = (keykind != RBI_KEY_REAL) ? (Datum) 0 :
		datumCopy(key, bs->state.typbyval, bs->state.typlen);
	b->hash = hash;
	b->cur = (RBIContainer *) palloc(RBI_CONTAINER_MAX_SIZE);
	b->cbuf = (RBIContainer *) palloc(RBI_CONTAINER_MAX_SIZE);
	b->seg = (RBIContainer *) palloc(RBI_CONTAINER_MAX_SIZE);
	rbi_sparse_init(b->seg, 0);
	b->npend = 0;
	b->hasgroup = false;
	b->inlinebuf = (char *) palloc0(bs->inline_limit);
	b->inlineused = 0;
	b->spilled = false;
	b->head = InvalidBlockNumber;
	b->curblk = InvalidBlockNumber;
	b->haspage = false;
	b->pagebuf = NULL;

	if (bs->nbuilders >= bs->maxbuilders)
	{
		MemoryContext old = MemoryContextSwitchTo(bs->buildctx);

		bs->maxbuilders *= 2;
		bs->builders = (RBIBuilder **) repalloc(bs->builders,
												sizeof(RBIBuilder *) * bs->maxbuilders);
		MemoryContextSwitchTo(old);
	}
	bs->builders[bs->nbuilders++] = b;

	return b;
}

/*
 * Append one finished container to the posting set under construction.
 */
static void
rbi_builder_spill(RBIBuildState *bs, RBIBuilder *b, RBIContainer *c)
{
	Size		csize = rbi_item_size(c);
	Page		img;

	if (!b->haspage)
	{
		b->pagebuf = rbi_build_get_page(bs, RBI_PAGE_CONTAINER);
		b->curblk = rbi_build_alloc_block(bs);
		b->head = b->curblk;
		b->haspage = true;
	}

	img = (Page) b->pagebuf->data;

	if (PageGetFreeSpace(img) < MAXALIGN(csize))
	{
		BlockNumber next = rbi_build_alloc_block(bs);

		rbi_page_update_minmax(img);
		RBIPageGetOpaque(img)->rightlink = next;
		/* the page is final: hand the image itself to the bulk writer */
		smgr_bulk_write(bs->bulk, b->curblk, b->pagebuf, true);

		b->curblk = next;
		b->pagebuf = rbi_build_get_page(bs, RBI_PAGE_CONTAINER);
		img = (Page) b->pagebuf->data;
	}

	if (PageAddItemExtended(img, c, csize, InvalidOffsetNumber, 0) ==
		InvalidOffsetNumber)
		elog(ERROR, "roaring index: failed to add container to build page");
}

static void
rbi_builder_emit(RBIBuildState *bs, RBIBuilder *b, RBIContainer *c)
{
	Size		csize = rbi_item_size(c);

	if (!b->spilled)
	{
		if (b->inlineused + csize <= (Size) bs->inline_limit)
		{
			/* INLINE payloads are packed without padding, as on disk. */
			memcpy(b->inlinebuf + b->inlineused, c, csize);
			b->inlineused += csize;
			return;
		}

		/* The posting set has outgrown the entry tuple: move it to pages. */
		{
			Size		off = 0;
			Size		used = b->inlineused;

			b->spilled = true;
			b->inlineused = 0;
			while (rbi_inline_fetch(b->inlinebuf, used, &off, b->cbuf) > 0)
				rbi_builder_spill(bs, b, b->cbuf);
		}
	}

	rbi_builder_spill(bs, b, c);
}

/*
 * Emit the open sparse segment, if there is one, and start a new one.
 */
static void
rbi_builder_close_segment(RBIBuildState *bs, RBIBuilder *b)
{
	if (b->seg->cardinality == 0)
		return;

	rbi_builder_emit(bs, b, b->seg);
	b->nitems++;
	rbi_sparse_init(b->seg, 0);
}

/*
 * Close the ckey group that is being collected: a dense one has already
 * become a container in b->cur and is emitted here, a sparse one appends its
 * few members to the open segment.
 */
static void
rbi_builder_finish_group(RBIBuildState *bs, RBIBuilder *b)
{
	if (!b->hasgroup)
		return;

	if (b->hascur)
	{
		rbi_container_optimize(b->cur);
		rbi_builder_emit(bs, b, b->cur);
		b->nitems++;
		b->hascur = false;
	}
	else if (b->npend > 0)
	{
		int			i;

		/* A ckey lives in one item only, so never split a group in two. */
		if ((uint32) b->seg->cardinality + (uint32) b->npend >
			RBI_SPARSE_MAX_PAIRS)
			rbi_builder_close_segment(bs, b);

		for (i = 0; i < b->npend; i++)
		{
			if (!rbi_sparse_insert(b->seg, b->curckey, b->pend[i], NULL))
				elog(ERROR, "roaring index: sparse segment overflowed during build");
		}
	}

	b->npend = 0;
	b->hasgroup = false;
}

static void
rbi_builder_add(RBIBuildState *bs, RBIBuilder *b, uint64 code)
{
	uint32		ckey = rbi_code_ckey(code);
	uint16		lo = rbi_code_lo(code);

	if (!b->hasgroup || b->curckey != ckey)
	{
		rbi_builder_finish_group(bs, b);
		b->curckey = ckey;
		b->hasgroup = true;
	}

	if (b->hascur)
		rbi_container_append_sorted(b->cur, lo);
	else
	{
		Assert(b->npend < RBI_SPARSE_THRESHOLD);
		b->pend[b->npend++] = lo;

		if (b->npend >= RBI_SPARSE_THRESHOLD)
		{
			int			i;

			/*
			 * This ckey is dense enough for a container.  The container's
			 * range sits after every pair collected so far, so the open
			 * segment has to be emitted before it (DESIGN.md §13: item ranges
			 * must not interleave).
			 */
			rbi_builder_close_segment(bs, b);

			rbi_container_init(b->cur, ckey);
			for (i = 0; i < b->npend; i++)
				rbi_container_append_sorted(b->cur, b->pend[i]);
			b->npend = 0;
			b->hascur = true;
		}
	}

	b->ntids++;
}

/*
 * Close a posting set: write out any pending container page and add the entry
 * tuple to its bucket.
 */
static void
rbi_builder_flush(RBIBuildState *bs, RBIBuilder *b)
{
	RBIEntryTuple *entry;
	Size		size;

	rbi_builder_finish_group(bs, b);
	rbi_builder_close_segment(bs, b);

	if (b->spilled)
	{
		Page		img = (Page) b->pagebuf->data;

		Assert(b->haspage);
		rbi_page_update_minmax(img);
		RBIPageGetOpaque(img)->rightlink = InvalidBlockNumber;
		smgr_bulk_write(bs->bulk, b->curblk, b->pagebuf, true);
		b->pagebuf = NULL;
		b->haspage = false;

		entry = (b->keykind != RBI_KEY_REAL) ?
			rbi_make_reserved_entry(rbi_reserved_flag(b->keykind),
									RBI_ENTRY_CHAIN, NULL, 0, &size) :
			rbi_make_entry(&bs->state, b->key, b->hash, RBI_ENTRY_CHAIN,
						   NULL, 0, &size);
		entry->head = b->head;
		entry->tail = b->curblk;
	}
	else
	{
		entry = (b->keykind != RBI_KEY_REAL) ?
			rbi_make_reserved_entry(rbi_reserved_flag(b->keykind),
									RBI_ENTRY_INLINE, b->inlinebuf,
									b->inlineused, &size) :
			rbi_make_entry(&bs->state, b->key, b->hash, RBI_ENTRY_INLINE,
						   b->inlinebuf, b->inlineused, &size);
	}

	entry->ncontainers = b->nitems;
	entry->ntids = b->ntids;

	rbi_build_add_entry(bs, rbi_bucket_of(b->hash, bs->nbuckets), entry, size);

	pfree(entry);
}

static void
rbi_flush_builders(RBIBuildState *bs)
{
	int			i;

	for (i = 0; i < bs->nbuilders; i++)
		rbi_builder_flush(bs, bs->builders[i]);
	bs->nbuilders = 0;
}

/* ---------------------------------------------------------------------
 * The build itself
 * --------------------------------------------------------------------- */

/*
 * Push one (kind, key, code) tuple into the sort.  A reserved kind carries no
 * key at all and hashes to 0.
 */
static void
rbi_build_put(RBIBuildState *bs, int keykind, Datum key, uint64 code)
{
	uint32		hash = (keykind == RBI_KEY_REAL) ?
		rbi_hash_key(&bs->state, key) : RBI_NULLKEY_HASH;

	ExecClearTuple(bs->inslot);
	bs->inslot->tts_values[0] = Int32GetDatum((int32) hash);
	bs->inslot->tts_isnull[0] = false;
	bs->inslot->tts_values[1] = key;
	bs->inslot->tts_isnull[1] = (keykind != RBI_KEY_REAL);
	bs->inslot->tts_values[2] = Int64GetDatum((int64) code);
	bs->inslot->tts_isnull[2] = false;
	bs->inslot->tts_values[3] = Int16GetDatum((int16) keykind);
	bs->inslot->tts_isnull[3] = false;
	ExecStoreVirtualTuple(bs->inslot);

	tuplesort_puttupleslot(bs->sortstate, bs->inslot);

	bs->indtuples += 1;
}

static void
rbi_build_callback(Relation index, ItemPointer tid, Datum *values,
				   bool *isnull, bool tupleIsAlive, void *arg)
{
	RBIBuildState *bs = (RBIBuildState *) arg;
	MemoryContext oldctx;
	Datum		key;
	uint64		code;

	rbi_check_key_offset(tid);
	code = rbi_tid_to_code(tid);

	oldctx = MemoryContextSwitchTo(bs->tmpctx);

	/*
	 * A NULL value goes into the sort like any other row, with hash 0: it
	 * ends up in the reserved NULL entry of bucket 0 (DESIGN.md §14), and the
	 * two passes below tell it from a real key by the kind column, never by
	 * comparing.
	 */
	if (isnull[0])
		rbi_build_put(bs, RBI_KEY_NULL, (Datum) 0, code);
	else if (bs->multikey)
	{
		/*
		 * DESIGN.md §17: one row, many keys.  A row the opclass extracts
		 * nothing from - an empty array, a tsvector with no lexemes - goes
		 * into the reserved EMPTY entry, so that a scan that has to look at
		 * every indexed row (`tags @> '{}'`) can still find it.
		 */
		Datum	   *keys;
		int			nkeys = rbi_extract_value(&bs->state, values[0], &keys);
		int			i;

		if (nkeys == 0)
			rbi_build_put(bs, RBI_KEY_EMPTY, (Datum) 0, code);
		for (i = 0; i < nkeys; i++)
			rbi_build_put(bs, RBI_KEY_REAL, keys[i], code);
	}
	else
	{
		key = values[0];
		if (!bs->state.typbyval && bs->state.typlen == -1)
			key = PointerGetDatum(PG_DETOAST_DATUM(key));

		rbi_build_put(bs, RBI_KEY_REAL, key, code);
	}

	MemoryContextSwitchTo(oldctx);
	MemoryContextReset(bs->tmpctx);
}

/*
 * One distinct key of the hash value pass 1 is currently looking at.
 */
typedef struct RBIKeyStat
{
	Datum		key;
	int			keykind;		/* RBI_KEY_* */
	Size		keysize;		/* bytes rbi_store_key() would write */
	int64		nmembers;		/* TIDs seen for this key */
} RBIKeyStat;

/*
 * Bytes the entry tuple of a key with nmembers members is expected to take up
 * on its bucket page, its line pointer included.
 *
 * Pass 1 does not group the codes by container key, so the posting set is
 * estimated from the member count alone: a member costs RBI_SPARSE_PAIR_SIZE
 * bytes while its container key stays sparse (DESIGN.md §13), and from
 * RBI_SPARSE_THRESHOLD members on the key is assumed to gather them into
 * ARRAY containers, which cost two bytes per member plus one header.  Both
 * halves are rough - what a posting set really costs depends on how its TIDs
 * spread over the heap - but they have the right order of magnitude at both
 * extremes (one row per key, one key for the whole table), which is all the
 * bucket count needs.  A posting set that outgrows inline_limit spills onto
 * container pages and leaves only the entry header behind, so the estimate is
 * capped there.
 */
static Size
rbi_build_entry_bytes(Size keysize, int64 nmembers, uint32 inline_limit)
{
	Size		payload;

	Assert(nmembers >= 0);

	if (nmembers < RBI_SPARSE_THRESHOLD)
		payload = (Size) nmembers * RBI_SPARSE_PAIR_SIZE;
	else
		payload = RBI_CONTAINER_HDRSZ + (Size) nmembers * sizeof(uint16);

	payload = Min(payload, (Size) inline_limit);

	return MAXALIGN(MAXALIGN(RBI_ENTRY_HDRSZ + keysize) + payload) +
		sizeof(ItemIdData);
}

/*
 * Pass 1: count distinct keys in the sorted input and add up the bytes their
 * entry tuples are expected to need.  *totalbytes receives the sum; the
 * return value is the number of distinct keys (the NULL key counts as one).
 */
static int64
rbi_build_scan_keys(RBIBuildState *bs, Size *totalbytes)
{
	int64		ndistinct = 0;
	int32		curhash = 0;
	bool		havehash = false;
	RBIKeyStat *keys = NULL;
	int			nkeys = 0;
	int			maxkeys = 8;
	int			i;
	MemoryContext oldctx;

	*totalbytes = 0;
	keys = (RBIKeyStat *) MemoryContextAlloc(bs->buildctx,
											 sizeof(RBIKeyStat) * maxkeys);

	while (tuplesort_gettupleslot(bs->sortstate, true, false, bs->outslot, NULL))
	{
		bool		isnull;
		int32		hash;
		Datum		key;
		int			keykind;
		RBIKeyStat *stat = NULL;

		hash = DatumGetInt32(slot_getattr(bs->outslot, 1, &isnull));
		key = slot_getattr(bs->outslot, 2, &isnull);
		keykind = (int) DatumGetInt16(slot_getattr(bs->outslot, 4, &isnull));

		if (!havehash || hash != curhash)
		{
			/* The hash group is complete: charge for its keys. */
			for (i = 0; i < nkeys; i++)
				*totalbytes += rbi_build_entry_bytes(keys[i].keysize,
													 keys[i].nmembers,
													 bs->inline_limit);
			MemoryContextReset(bs->tmpctx);
			curhash = hash;
			havehash = true;
			nkeys = 0;
		}

		for (i = 0; i < nkeys; i++)
		{
			if (keys[i].keykind != keykind)
				continue;
			if (keykind != RBI_KEY_REAL ||
				rbi_keys_equal(&bs->state, keys[i].key, key))
			{
				stat = &keys[i];
				break;
			}
		}

		if (stat == NULL)
		{
			if (nkeys >= maxkeys)
			{
				oldctx = MemoryContextSwitchTo(bs->buildctx);
				maxkeys *= 2;
				keys = (RBIKeyStat *) repalloc(keys,
											   sizeof(RBIKeyStat) * maxkeys);
				MemoryContextSwitchTo(oldctx);
			}
			stat = &keys[nkeys++];
			stat->keykind = keykind;
			stat->nmembers = 0;
			if (keykind != RBI_KEY_REAL)
			{
				stat->key = (Datum) 0;
				stat->keysize = 0;
			}
			else
			{
				oldctx = MemoryContextSwitchTo(bs->tmpctx);
				stat->key = datumCopy(key, bs->state.typbyval,
									  bs->state.typlen);
				MemoryContextSwitchTo(oldctx);
				stat->keysize = rbi_key_datum_size(&bs->state, stat->key);
			}
			ndistinct++;
		}

		stat->nmembers++;

		CHECK_FOR_INTERRUPTS();
	}

	for (i = 0; i < nkeys; i++)
		*totalbytes += rbi_build_entry_bytes(keys[i].keysize,
											 keys[i].nmembers,
											 bs->inline_limit);

	MemoryContextReset(bs->tmpctx);
	pfree(keys);

	return ndistinct;
}

/*
 * Pass 2: group by key and write out the posting sets.
 */
static void
rbi_build_write_entries(RBIBuildState *bs)
{
	int32		curhash = 0;
	bool		havehash = false;
	MemoryContext oldctx;

	tuplesort_rescan(bs->sortstate);

	oldctx = MemoryContextSwitchTo(bs->tmpctx);

	while (tuplesort_gettupleslot(bs->sortstate, true, false, bs->outslot, NULL))
	{
		bool		isnull;
		int32		hash;
		Datum		key;
		uint64		code;
		int			keykind;
		RBIBuilder *b = NULL;
		int			i;

		hash = DatumGetInt32(slot_getattr(bs->outslot, 1, &isnull));
		key = slot_getattr(bs->outslot, 2, &isnull);
		code = (uint64) DatumGetInt64(slot_getattr(bs->outslot, 3, &isnull));
		keykind = (int) DatumGetInt16(slot_getattr(bs->outslot, 4, &isnull));

		if (!havehash || hash != curhash)
		{
			rbi_flush_builders(bs);
			MemoryContextReset(bs->tmpctx);
			curhash = hash;
			havehash = true;
		}

		/* Group by the kind first, then by key (DESIGN.md §14 and §17). */
		for (i = 0; i < bs->nbuilders; i++)
		{
			if (bs->builders[i]->keykind != keykind)
				continue;
			if (keykind != RBI_KEY_REAL ||
				rbi_keys_equal(&bs->state, bs->builders[i]->key, key))
			{
				b = bs->builders[i];
				break;
			}
		}
		if (b == NULL)
			b = rbi_builder_create(bs, key, keykind, (uint32) hash);

		rbi_builder_add(bs, b, code);

		CHECK_FOR_INTERRUPTS();
	}

	rbi_flush_builders(bs);
	MemoryContextSwitchTo(oldctx);
	MemoryContextReset(bs->tmpctx);
}

IndexBuildResult *
rbibuild(Relation heap, Relation index, IndexInfo *indexInfo)
{
	IndexBuildResult *result;
	RBIBuildState bs;
	RBIOptions *opts = (RBIOptions *) index->rd_options;
	RBIMetaPageData meta;
	AttrNumber	attNums[2];
	Oid			sortOperators[2];
	Oid			sortCollations[2];
	bool		nullsFirstFlags[2];
	int64		ndistinct;
	Size		entrybytes;
	double		reltuples;
	Form_pg_attribute keyatt;

	if (RelationGetNumberOfBlocks(index) != 0)
		elog(ERROR, "index \"%s\" already contains data",
			 RelationGetRelationName(index));

	memset(&bs, 0, sizeof(bs));
	bs.index = index;
	bs.indtuples = 0;
	bs.inline_limit = opts ? (uint32) opts->inline_limit : RBI_DEFAULT_INLINE_LIMIT;
	bs.max_entries = rbi_max_entries(index);

	bs.buildctx = AllocSetContextCreate(CurrentMemoryContext,
										"roaring index build",
										ALLOCSET_DEFAULT_SIZES);
	bs.tmpctx = AllocSetContextCreate(bs.buildctx,
									  "roaring index build temporary",
									  ALLOCSET_DEFAULT_SIZES);

	/*
	 * The meta page does not exist yet, so build the relation state from the
	 * options directly.  nbuckets is filled in once the data has been seen.
	 */
	memset(&meta, 0, sizeof(meta));
	meta.magic = RBI_MAGIC;
	meta.version = RBI_VERSION;
	meta.offset_bits = RBI_OFFSET_BITS;
	meta.container_bits = RBI_CONTAINER_BITS;
	meta.inline_limit = bs.inline_limit;
	rbi_fill_state(index, &bs.state, &meta, bs.buildctx);
	bs.multikey = bs.state.multikey;

	bs.maxbuilders = 8;
	bs.builders = (RBIBuilder **) MemoryContextAlloc(bs.buildctx,
													 sizeof(RBIBuilder *) * bs.maxbuilders);
	bs.nbuilders = 0;

	/*
	 * Sort tuple: (hash int4, key, code int8, kind int2), sorted by hash then
	 * code.  The key column's type is the index's own, which core resolved
	 * from the opclass: the element type for a multi-key class (DESIGN.md
	 * §17), so one heap row's several keys sort as the values they are.  The
	 * kind column is not a sort key - it is a function of the key being NULL
	 * - but the two reserved kinds share that state and have to be told apart
	 * when the passes group.
	 */
	keyatt = TupleDescAttr(RelationGetDescr(index), 0);
	bs.sorttupdesc = CreateTemplateTupleDesc(4);
	TupleDescInitEntry(bs.sorttupdesc, 1, "hash", INT4OID, -1, 0);
	TupleDescInitEntry(bs.sorttupdesc, 2, "key", keyatt->atttypid,
					   keyatt->atttypmod, 0);
	TupleDescInitEntry(bs.sorttupdesc, 3, "code", INT8OID, -1, 0);
	TupleDescInitEntry(bs.sorttupdesc, 4, "kind", INT2OID, -1, 0);
	TupleDescInitEntryCollation(bs.sorttupdesc, 2, bs.state.collation);
	TupleDescFinalize(bs.sorttupdesc);

	attNums[0] = 1;
	sortOperators[0] = Int4LessOperator;
	sortCollations[0] = InvalidOid;
	nullsFirstFlags[0] = false;
	attNums[1] = 3;
	sortOperators[1] = Int8LessOperator;
	sortCollations[1] = InvalidOid;
	nullsFirstFlags[1] = false;

	bs.sortstate = tuplesort_begin_heap(bs.sorttupdesc, 2, attNums,
										sortOperators, sortCollations,
										nullsFirstFlags,
										maintenance_work_mem, NULL,
										TUPLESORT_RANDOMACCESS);

	bs.inslot = MakeSingleTupleTableSlot(bs.sorttupdesc, &TTSOpsVirtual);
	bs.outslot = MakeSingleTupleTableSlot(bs.sorttupdesc, &TTSOpsMinimalTuple);

	reltuples = table_index_build_scan(heap, index, indexInfo, true, true,
									   rbi_build_callback, (void *) &bs, NULL);

	tuplesort_performsort(bs.sortstate);

	ndistinct = rbi_build_scan_keys(&bs, &entrybytes);

	if (opts && opts->buckets > 0)
		bs.nbuckets = rbi_clamp_buckets(opts->buckets);
	else
	{
		/*
		 * Auto-size by BYTES, not by key count (DESIGN.md §5).  Every bucket
		 * owns a head page whether or not it needs one, so what the count has
		 * to track is how many pages the entries want: aim at three quarters
		 * of a page per bucket, which leaves the fuller-than-average buckets
		 * room to grow inside their head page and does not spend a page on
		 * every handful of keys.  Never fewer than RBI_DEFAULT_BUCKETS,
		 * because an index is very often built on an empty table and filled
		 * afterwards, and a single bucket would make every later insert scan
		 * the whole entry list.
		 */
		double		bytes = (double) entrybytes;
		int64		want;

		/*
		 * The heap may already be known to hold more rows than this build put
		 * in the index, and the directory is sized once and never resized
		 * (DESIGN.md §5), so scale the estimate up by what pg_class.reltuples
		 * says.  Only ever up, and only for an index over the whole table: a
		 * partial index holds the rows its predicate selects, and sizing that
		 * for the whole heap would spend pages on buckets that stay empty.
		 * reltuples is -1 when nothing has analysed the heap yet, and an index
		 * built on an empty table therefore still gets the floor above - which
		 * is exactly the case the `buckets` reloption is for.
		 */
		if (bs.indtuples > 0 && indexInfo->ii_Predicate == NIL &&
			heap->rd_rel->reltuples > bs.indtuples)
		{
			bytes *= (double) heap->rd_rel->reltuples / bs.indtuples;
			elog(DEBUG1, "roaring index \"%s\": heap has %.0f rows against %.0f indexed; sizing for %.0f entry bytes",
				 RelationGetRelationName(index), (double) heap->rd_rel->reltuples,
				 bs.indtuples, bytes);
		}

		want = (int64) ((bytes + RBI_BUCKET_FILL_BYTES - 1) /
						RBI_BUCKET_FILL_BYTES);

		bs.nbuckets = rbi_clamp_buckets(Max(want, (int64) RBI_DEFAULT_BUCKETS));
	}
	bs.state.meta.nbuckets = bs.nbuckets;

	elog(DEBUG1, "roaring index \"%s\": " INT64_FORMAT " distinct keys, %zu entry bytes, %u buckets",
		 RelationGetRelationName(index), ndistinct, entrybytes, bs.nbuckets);

	/*
	 * Cardinality guard (DESIGN.md §17).  At build time the count is exact -
	 * pass 1 has just counted the distinct keys - so the warning is too.  It
	 * is only a warning: the index is built either way.
	 */
	if (bs.max_entries > 0 && ndistinct > (int64) bs.max_entries)
		rbi_warn_max_entries(index, ndistinct);

	/*
	 * Everything below writes pages, and all of it goes through one bulk
	 * writer: nothing may touch these blocks through the buffer manager until
	 * smgr_bulk_finish() has written and (if needed) synced them.
	 */
	{
		/*
		 * The bulk writer allocates its page buffers in the context that is
		 * current when it starts, and frees them as it writes them; keep them
		 * in the build context, which outlives smgr_bulk_finish().
		 */
		MemoryContext oldctx = MemoryContextSwitchTo(bs.buildctx);

		bs.bulk = smgr_bulk_start_rel(index, MAIN_FORKNUM);
		MemoryContextSwitchTo(oldctx);
	}
	rbi_build_init_pages(&bs);
	rbi_build_write_entries(&bs);
	rbi_build_flush_buckets(&bs);
	smgr_bulk_finish(bs.bulk);

	elog(DEBUG1, "roaring index \"%s\": %u blocks, " INT64_FORMAT " bucket pages",
		 RelationGetRelationName(index), bs.nblocks, bs.nbucketpages);

	tuplesort_end(bs.sortstate);
	ExecDropSingleTupleTableSlot(bs.inslot);
	ExecDropSingleTupleTableSlot(bs.outslot);
	FreeTupleDesc(bs.sorttupdesc);
	MemoryContextDelete(bs.buildctx);

	result = (IndexBuildResult *) palloc0(sizeof(IndexBuildResult));
	result->heap_tuples = reltuples;
	result->index_tuples = bs.indtuples;

	return result;
}
