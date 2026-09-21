/*-------------------------------------------------------------------------
 *
 * rbi_build.c
 *		ambuild for the roaring index (DESIGN.md section 5, BUILD).
 *
 * The heap is scanned once into a tuplesort of (hash int4, key, code int8)
 * sorted by (hash, code).  A first pass over the sorted data counts distinct
 * keys so that the bucket count can be chosen; a second pass groups the codes
 * of each key into containers and writes the posting sets out, either inline
 * in the entry tuple or as a chain of container pages.
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
	bool		isnull;			/* the reserved NULL key (DESIGN.md §14) */
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
	BlockNumber curblk;			/* block reserved for pageimg */
	PGAlignedBlock *pageimg;	/* container page under construction */
	bool		haspage;

	uint32		nitems;			/* containers and segments (entry.ncontainers) */
	uint64		ntids;
} RBIBuilder;

typedef struct RBIBuildState
{
	Relation	index;
	RBIState	state;
	uint32		nbuckets;
	uint32		inline_limit;

	Tuplesortstate *sortstate;
	TupleDesc	sorttupdesc;
	TupleTableSlot *inslot;
	TupleTableSlot *outslot;

	MemoryContext buildctx;		/* lives for the whole build */
	MemoryContext tmpctx;		/* reset per heap tuple / per key group */

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
 * Raw page helpers
 *
 * During a build nobody else can see the index, so pages are prepared in
 * backend-local memory and written out in one full-page WAL record.  A block
 * is reserved (the relation is extended) before its contents are final so
 * that the previous page of a chain can store the right rightlink.
 * --------------------------------------------------------------------- */

static BlockNumber
rbi_build_reserve_page(Relation index)
{
	Buffer		buf;
	BlockNumber blk;

	buf = ExtendBufferedRel(BMR_REL(index), MAIN_FORKNUM, NULL,
							EB_LOCK_FIRST | EB_SKIP_EXTENSION_LOCK);
	blk = BufferGetBlockNumber(buf);
	UnlockReleaseBuffer(buf);

	return blk;
}

static void
rbi_build_write_page(Relation index, BlockNumber blk, const char *image)
{
	Buffer		buf;
	GenericXLogState *xstate;
	Page		page;

	buf = ReadBuffer(index, blk);
	LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);

	xstate = GenericXLogStart(index);
	page = GenericXLogRegisterBuffer(xstate, buf, GENERIC_XLOG_FULL_IMAGE);
	memcpy(page, image, BLCKSZ);
	GenericXLogFinish(xstate);

	UnlockReleaseBuffer(buf);
}

/*
 * Create the meta page and all bucket head pages.
 */
static void
rbi_build_init_pages(Relation index, uint32 nbuckets, uint32 inline_limit)
{
	PGAlignedBlock img;
	BlockNumber blk;
	uint32		b;

	rbi_init_metapage((Page) img.data, nbuckets, inline_limit);
	blk = rbi_build_reserve_page(index);
	if (blk != RBI_METAPAGE_BLKNO)
		elog(ERROR, "roaring index: meta page landed on block %u", blk);
	rbi_build_write_page(index, blk, img.data);

	rbi_init_page((Page) img.data, RBI_PAGE_BUCKET);
	for (b = 0; b < nbuckets; b++)
	{
		blk = rbi_build_reserve_page(index);
		if (blk != RBI_BUCKET_BLKNO(b))
			elog(ERROR, "roaring index: bucket page %u landed on block %u", b, blk);
		rbi_build_write_page(index, blk, img.data);

		CHECK_FOR_INTERRUPTS();
	}
}

/* ---------------------------------------------------------------------
 * Posting set builders
 * --------------------------------------------------------------------- */

static RBIBuilder *
rbi_builder_create(RBIBuildState *bs, Datum key, bool isnull, uint32 hash)
{
	RBIBuilder *b = (RBIBuilder *) palloc0(sizeof(RBIBuilder));

	b->isnull = isnull;
	b->key = isnull ? (Datum) 0 :
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
	b->pageimg = NULL;

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
		b->pageimg = (PGAlignedBlock *) palloc(sizeof(PGAlignedBlock));
		b->curblk = rbi_build_reserve_page(bs->index);
		b->head = b->curblk;
		rbi_init_page((Page) b->pageimg->data, RBI_PAGE_CONTAINER);
		b->haspage = true;
	}

	img = (Page) b->pageimg->data;

	if (PageGetFreeSpace(img) < MAXALIGN(csize))
	{
		BlockNumber next = rbi_build_reserve_page(bs->index);

		rbi_page_update_minmax(img);
		RBIPageGetOpaque(img)->rightlink = next;
		rbi_build_write_page(bs->index, b->curblk, b->pageimg->data);

		b->curblk = next;
		rbi_init_page(img, RBI_PAGE_CONTAINER);
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
	Buffer		headbuf;

	rbi_builder_finish_group(bs, b);
	rbi_builder_close_segment(bs, b);

	if (b->spilled)
	{
		Page		img = (Page) b->pageimg->data;

		Assert(b->haspage);
		rbi_page_update_minmax(img);
		RBIPageGetOpaque(img)->rightlink = InvalidBlockNumber;
		rbi_build_write_page(bs->index, b->curblk, b->pageimg->data);

		entry = b->isnull ?
			rbi_make_null_entry(RBI_ENTRY_CHAIN, NULL, 0, &size) :
			rbi_make_entry(&bs->state, b->key, b->hash, RBI_ENTRY_CHAIN,
						   NULL, 0, &size);
		entry->head = b->head;
		entry->tail = b->curblk;
	}
	else
	{
		entry = b->isnull ?
			rbi_make_null_entry(RBI_ENTRY_INLINE, b->inlinebuf, b->inlineused,
								&size) :
			rbi_make_entry(&bs->state, b->key, b->hash, RBI_ENTRY_INLINE,
						   b->inlinebuf, b->inlineused, &size);
	}

	entry->ncontainers = b->nitems;
	entry->ntids = b->ntids;

	headbuf = ReadBuffer(bs->index,
						 RBI_BUCKET_BLKNO(rbi_bucket_of(b->hash, bs->nbuckets)));
	LockBuffer(headbuf, BUFFER_LOCK_EXCLUSIVE);
	rbi_add_entry(bs->index, headbuf, entry, size);
	UnlockReleaseBuffer(headbuf);

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

static void
rbi_build_callback(Relation index, ItemPointer tid, Datum *values,
				   bool *isnull, bool tupleIsAlive, void *arg)
{
	RBIBuildState *bs = (RBIBuildState *) arg;
	MemoryContext oldctx;
	Datum		key;
	uint32		hash;
	uint64		code;

	rbi_check_key_offset(tid);
	code = rbi_tid_to_code(tid);

	oldctx = MemoryContextSwitchTo(bs->tmpctx);

	/*
	 * A NULL key goes into the sort like any other, with hash 0: it ends up
	 * in the reserved NULL entry of bucket 0 (DESIGN.md §14), and the two
	 * passes below tell it from a real key by the isnull flag of the key
	 * column, never by comparing.
	 */
	if (isnull[0])
	{
		key = (Datum) 0;
		hash = RBI_NULLKEY_HASH;
	}
	else
	{
		key = values[0];
		if (!bs->state.typbyval && bs->state.typlen == -1)
			key = PointerGetDatum(PG_DETOAST_DATUM(key));

		hash = rbi_hash_key(&bs->state, key);
	}

	ExecClearTuple(bs->inslot);
	bs->inslot->tts_values[0] = Int32GetDatum((int32) hash);
	bs->inslot->tts_isnull[0] = false;
	bs->inslot->tts_values[1] = key;
	bs->inslot->tts_isnull[1] = isnull[0];
	bs->inslot->tts_values[2] = Int64GetDatum((int64) code);
	bs->inslot->tts_isnull[2] = false;
	ExecStoreVirtualTuple(bs->inslot);

	tuplesort_puttupleslot(bs->sortstate, bs->inslot);

	MemoryContextSwitchTo(oldctx);
	MemoryContextReset(bs->tmpctx);

	bs->indtuples += 1;
}

/*
 * One distinct key of the hash value pass 1 is currently looking at.
 */
typedef struct RBIKeyStat
{
	Datum		key;
	bool		isnull;			/* the reserved NULL key */
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
		RBIKeyStat *stat = NULL;

		hash = DatumGetInt32(slot_getattr(bs->outslot, 1, &isnull));
		key = slot_getattr(bs->outslot, 2, &isnull);

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
			if (keys[i].isnull != isnull)
				continue;
			if (isnull || rbi_keys_equal(&bs->state, keys[i].key, key))
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
			stat->isnull = isnull;
			stat->nmembers = 0;
			if (isnull)
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
		RBIBuilder *b = NULL;
		int			i;

		bool		keyisnull;

		hash = DatumGetInt32(slot_getattr(bs->outslot, 1, &isnull));
		key = slot_getattr(bs->outslot, 2, &keyisnull);
		code = (uint64) DatumGetInt64(slot_getattr(bs->outslot, 3, &isnull));

		if (!havehash || hash != curhash)
		{
			rbi_flush_builders(bs);
			MemoryContextReset(bs->tmpctx);
			curhash = hash;
			havehash = true;
		}

		/* Group by the isnull flag first, then by key (DESIGN.md §14). */
		for (i = 0; i < bs->nbuilders; i++)
		{
			if (bs->builders[i]->isnull != keyisnull)
				continue;
			if (keyisnull ||
				rbi_keys_equal(&bs->state, bs->builders[i]->key, key))
			{
				b = bs->builders[i];
				break;
			}
		}
		if (b == NULL)
			b = rbi_builder_create(bs, key, keyisnull, (uint32) hash);

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

	bs.maxbuilders = 8;
	bs.builders = (RBIBuilder **) MemoryContextAlloc(bs.buildctx,
													 sizeof(RBIBuilder *) * bs.maxbuilders);
	bs.nbuilders = 0;

	/* Sort tuple: (hash int4, key, code int8), sorted by hash then code. */
	keyatt = TupleDescAttr(RelationGetDescr(index), 0);
	bs.sorttupdesc = CreateTemplateTupleDesc(3);
	TupleDescInitEntry(bs.sorttupdesc, 1, "hash", INT4OID, -1, 0);
	TupleDescInitEntry(bs.sorttupdesc, 2, "key", keyatt->atttypid,
					   keyatt->atttypmod, 0);
	TupleDescInitEntry(bs.sorttupdesc, 3, "code", INT8OID, -1, 0);
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
		int64		want = (int64) ((entrybytes + RBI_BUCKET_FILL_BYTES - 1) /
									RBI_BUCKET_FILL_BYTES);

		bs.nbuckets = rbi_clamp_buckets(Max(want, (int64) RBI_DEFAULT_BUCKETS));
	}
	bs.state.meta.nbuckets = bs.nbuckets;

	elog(DEBUG1, "roaring index \"%s\": " INT64_FORMAT " distinct keys, %zu entry bytes, %u buckets",
		 RelationGetRelationName(index), ndistinct, entrybytes, bs.nbuckets);

	rbi_build_init_pages(index, bs.nbuckets, bs.inline_limit);

	rbi_build_write_entries(&bs);

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
