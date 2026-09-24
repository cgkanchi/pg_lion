/*-------------------------------------------------------------------------
 *
 * lion_build.c
 *		ambuild for the lion index (DESIGN.md section 5, BUILD).
 *
 * The heap is scanned once into a tuplesort of (kind int4, hash int8,
 * code int8, key) sorted into the DIRECTORY order of DESIGN.md
 * §21 - (kind, key, hash) for an ordered opclass, (kind, hash, stored bytes)
 * for one whose key type has no btree opclass - with the code last.  One pass
 * over the sorted data groups the codes of each key into containers and
 * writes the posting sets out, either inline in the entry tuple or as a chain
 * of container pages, and the entries come out in exactly the order the
 * directory wants them in.
 *
 * The directory itself is built bottom-up in that same pass, nbtree's
 * _bt_buildadd shape: one open page per level, filled to `fillfactor` percent
 * and written out when the next item does not fit, at which point its high
 * key is that item's key and its downlink goes to the level above.  The level
 * whose last page is also its first is the root.
 *
 * A multi-key opclass (DESIGN.md §17) turns one heap row into one sort tuple
 * per distinct key it extracts, all carrying the same code; nothing else in
 * the build changes, because the sort orders the codes of each key
 * ascending either way.
 *
 * A MULTICOLUMN index (DESIGN.md §24) is ONE heap scan feeding ONE TUPLESORT
 * PER KEY COLUMN, and the directory is then built from the sorts in column
 * order: the directory order leads with the column number, so the entries of
 * column 1 followed by the entries of column 2 are already the order the
 * leaves want, and the bottom-up level builder never has to know that more
 * than one column exists.
 *
 * *(Deviation from the first draft of §24, which said "one tuplesort of
 * (attno, kind, key, hash, code) rows".  One tuplesort needs one tuple
 * descriptor and one sort operator per sort key, and the columns of a
 * multicolumn index have DIFFERENT key types - int4, text, an array's element
 * type - so there is no `key` column to describe.  n sorts fed by one scan is
 * what §24's own rationale asks for ("n tuplesort inputs from one scan"), it
 * compares nothing across columns, and it keeps each column's sort keys
 * exactly what §21 chose for it.  maintenance_work_mem is split between
 * them.)*
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
 * written.  A directory page is final as soon as the item that does not fit
 * on it arrives, because that item's key is its high key, so at most one page
 * per level is open at a time - which is what the sorted directory buys over
 * the hash one, whose pages could not be finished until the very last entry
 * had been hashed and therefore all lived in memory at once.  Container pages
 * are final as soon as the next container does not fit, so only one of those
 * exists per open key at a time.
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
#include "storage/bufpage.h"
#include "utils/datum.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/tuplesort.h"
#include "varatt.h"

#include "lion.h"

#if PG_VERSION_NUM < 170000
#include "access/xloginsert.h"
#include "storage/smgr.h"

/*
 * The bulk-write API of PostgreSQL 17 (storage/bulk_write.h), reduced to what
 * this file uses, for 16 (lion_compat.h).  It follows 17's bulk_write.c: the
 * page buffers belong to the memory context that was current when the writer
 * started, up to LION_BULK_PENDING pages are queued and then written in block
 * order after one log_newpages() record for the batch, a block beyond the
 * current end is reached by zero-extending (the zero pages are not logged:
 * the real page's image overwrites them), and the fork is fsynced at the end
 * unless the relation is temporary, because none of these writes went through
 * shared buffers and a checkpoint during the build cannot know about them.
 */
#define LION_BULK_PENDING	64

typedef struct LionPendingWrite
{
	BulkWriteBuffer buf;
	BlockNumber blkno;
	bool		page_std;
} LionPendingWrite;

struct BulkWriteState
{
	SMgrRelation smgr;
	ForkNumber	forknum;
	RelFileLocator locator;
	bool		use_wal;
	bool		need_sync;
	int			npending;
	LionPendingWrite pending[LION_BULK_PENDING];
	BlockNumber pages_written;
	PGIOAlignedBlock *zeropage;
	MemoryContext memcxt;
};

#define ST_SORT sort_lion_pending_writes
#define ST_ELEMENT_TYPE LionPendingWrite
#define ST_COMPARE(a, b) \
	((int) ((a)->blkno > (b)->blkno) - (int) ((a)->blkno < (b)->blkno))
#define ST_SCOPE static
#define ST_DEFINE
#include "lib/sort_template.h"

BulkWriteState *
smgr_bulk_start_rel(Relation rel, ForkNumber forknum)
{
	BulkWriteState *bw = palloc0(sizeof(BulkWriteState));

	bw->smgr = RelationGetSmgr(rel);
	bw->forknum = forknum;
	bw->locator = rel->rd_locator;
	bw->use_wal = RelationNeedsWAL(rel) || forknum == INIT_FORKNUM;
	bw->need_sync = !RelationUsesLocalBuffers(rel);
	bw->pages_written = smgrnblocks(bw->smgr, forknum);
	bw->memcxt = CurrentMemoryContext;

	return bw;
}

BulkWriteBuffer
smgr_bulk_get_buf(BulkWriteState *bw)
{
	return MemoryContextAllocAligned(bw->memcxt, BLCKSZ, PG_IO_ALIGN_SIZE, 0);
}

static void
lion_bulk_flush(BulkWriteState *bw)
{
	int			n = bw->npending;

	if (n == 0)
		return;

	sort_lion_pending_writes(bw->pending, n);

	if (bw->use_wal)
	{
		BlockNumber blknos[LION_BULK_PENDING];
		Page		pages[LION_BULK_PENDING];
		bool		page_std = true;

		for (int i = 0; i < n; i++)
		{
			blknos[i] = bw->pending[i].blkno;
			pages[i] = (Page) bw->pending[i].buf->data;
			/* one non-standard page makes the whole batch non-standard */
			if (!bw->pending[i].page_std)
				page_std = false;
		}
		log_newpages(&bw->locator, bw->forknum, n, blknos, pages, page_std);
	}

	for (int i = 0; i < n; i++)
	{
		BlockNumber blkno = bw->pending[i].blkno;
		Page		page = (Page) bw->pending[i].buf->data;

		while (blkno > bw->pages_written)
		{
			if (bw->zeropage == NULL)
				bw->zeropage = MemoryContextAllocAligned(bw->memcxt, BLCKSZ,
														 PG_IO_ALIGN_SIZE,
														 MCXT_ALLOC_ZERO);
			smgrextend(bw->smgr, bw->forknum, bw->pages_written++,
					   bw->zeropage->data, true);
		}

		PageSetChecksumInplace(page, blkno);

		if (blkno == bw->pages_written)
		{
			smgrextend(bw->smgr, bw->forknum, blkno, page, true);
			bw->pages_written++;
		}
		else
			smgrwrite(bw->smgr, bw->forknum, blkno, page, true);

		pfree(page);
	}

	bw->npending = 0;
}

void
smgr_bulk_write(BulkWriteState *bw, BlockNumber blkno, BulkWriteBuffer buf,
				bool page_std)
{
	LionPendingWrite *w = &bw->pending[bw->npending++];

	w->buf = buf;
	w->blkno = blkno;
	w->page_std = page_std;

	if (bw->npending == LION_BULK_PENDING)
		lion_bulk_flush(bw);
}

void
smgr_bulk_finish(BulkWriteState *bw)
{
	lion_bulk_flush(bw);
	if (bw->need_sync)
		smgrimmedsync(bw->smgr, bw->forknum);
	if (bw->zeropage != NULL)
		pfree(bw->zeropage);
	pfree(bw);
}
#endif							/* PG_VERSION_NUM < 170000 */

/*
 * Which entry a sorted tuple belongs to.  A row contributes one tuple per
 * distinct extracted key, or exactly one NULL / EMPTY tuple when it has no
 * key at all; the two reserved kinds are told apart by this column rather
 * than by the (absent) key, and they share hash 0 with any real key that
 * happens to hash there.
 *
 * The values are the directory kinds of DESIGN.md §21 (NULL < EMPTY < VALUE),
 * so sorting the column ascending is already the order the directory wants.
 */
#define LION_KEY_NULL	LION_KIND_NULL
#define LION_KEY_EMPTY	LION_KIND_EMPTY
#define LION_KEY_REAL	LION_KIND_VALUE

static inline uint16
lion_reserved_flag(int kind)
{
	Assert(kind == LION_KEY_NULL || kind == LION_KEY_EMPTY);
	return (kind == LION_KEY_NULL) ? LION_ENTRY_NULLKEY : LION_ENTRY_EMPTYKEY;
}

/*
 * One posting set under construction.  Items are produced in ascending ckey
 * order and kept in an inline buffer until they no longer fit in `inlinemax`
 * bytes - inline_limit, or less for a key too long to leave inline_limit of
 * LION_MAX_ENTRY_SIZE - after that the entry spills to a chain of container
 * pages that is written out page by page.
 *
 * Sparse segments (DESIGN.md §13) make the grouping two-stage.  The codes of
 * one key arrive sorted, so they arrive grouped by ckey; a group is only
 * known to be dense enough for a container of its own once its
 * LION_SPARSE_THRESHOLD'th member shows up.  Until then its members wait in
 * pend[] -- deliberately *not* in the open segment, because a group that
 * turns out to be dense has to become a container placed after the segment,
 * and pairs already in the segment could not be taken back out without
 * breaking the ordering of the items.  Emitting a container therefore closes
 * the open segment first, which is what keeps item ranges from interleaving.
 */
/*
 * One INTERNAL level of a posting tree under construction (DESIGN.md §22).
 *
 * Built bottom-up in the same pass as the leaves, nbtree's _bt_buildadd shape
 * and the same shape §21 uses for the directory: one open page per level,
 * written out when the downlink that does not fit arrives - that downlink's
 * separator is the page's high key, and the page's own first separator is the
 * downlink it hands to the level above.  The level whose last page is also its
 * first is the root, and it is written at the block the set reserved for its
 * root, because the root block is the set's identity (DESIGN.md §18).
 */
typedef struct LionPostLevel
{
	uint16		level;			/* 1 = the level just above the leaves */
	LionPostingPivot *items;	/* the open page's downlinks */
	int			nitems;
	int			maxitems;
	Size		used;			/* what they cost on a page, line pointers in */
	BlockNumber blkno;			/* block reserved for the open page */
	int64		npages;			/* pages of this level written so far */
	struct LionPostLevel *parent;
} LionPostLevel;

typedef struct LionBuilder
{
	Datum		key;			/* private copy of the key */
	int			keykind;		/* LION_KEY_* */
	uint32		hash;
	char	   *rawkey;			/* the key as the entry tuple stores it */
	Size		rawlen;			/* ... which is the tail of the directory order */

	bool		hasgroup;		/* curckey is a ckey group being collected */
	uint32		curckey;
	bool		hascur;			/* cur holds an unfinished container */
	LionContainer *cur;			/* LION_CONTAINER_MAX_SIZE work buffer */
	LionContainer *cbuf;			/* scratch for re-reading packed items */

	uint16		pend[LION_SPARSE_THRESHOLD];	/* members of a still-sparse ckey */
	int			npend;
	LionContainer *seg;			/* open sparse segment, empty when none */

	char	   *inlinebuf;		/* buffered items, packed without padding */
	Size		inlineused;
	Size		inlinemax;		/* lion_inline_max() for this key */

	bool		spilled;		/* posting set lives on container pages */
	BlockNumber head;			/* the set's ROOT block (DESIGN.md §22) */
	BlockNumber curblk;			/* block pagebuf will be written to */
	BulkWriteBuffer pagebuf;	/* container page under construction */
	bool		haspage;

	/*
	 * The posting tree above the leaves (DESIGN.md §22).  A set that fits one
	 * leaf has none at all and `head` is that leaf; the moment a second leaf
	 * is needed, a block is reserved for the ROOT, the first leaf is re-stamped
	 * with it, and the internal levels are filled bottom-up.
	 */
	bool		hastree;
	BlockNumber rootblk;
	LionPostLevel *plevel;		/* the level just above the leaves */
	uint32		height;

	uint32		nitems;			/* containers and segments (entry.ncontainers) */
	uint64		ntids;
} LionBuilder;

/*
 * One level of the directory under construction (DESIGN.md §21).  Items are
 * buffered until the page is final - which it is as soon as the item that
 * does not fit arrives, because that item's key becomes the high key - and
 * the page is then written and its downlink handed to the level above.
 */
typedef struct LionBuildLevel
{
	uint16		level;			/* 0 = leaves */
	char	  **items;			/* private copies of the open page's items */
	Size	   *sizes;
	int			nitems;
	int			maxitems;
	Size		used;			/* what they cost on a page, line pointers in */
	BlockNumber blkno;			/* block reserved for the open page */
	BlockNumber leftblk;		/* the page written before it, if any */
	int64		npages;			/* pages of this level written so far */
	struct LionBuildLevel *parent;
} LionBuildLevel;

/*
 * One key column's input to the build (DESIGN.md §24): its own tuplesort, fed
 * by the one heap scan and drained into the shared directory when the columns
 * before it are done.
 */
typedef struct LionBuildCol
{
	LionState  *state;			/* the column's state, from bs->ix */
	bool		multikey;		/* the opclass extracts keys (DESIGN.md §17) */
	Tuplesortstate *sortstate;
	TupleDesc	sorttupdesc;
	TupleTableSlot *inslot;
	TupleTableSlot *outslot;
} LionBuildCol;

typedef struct LionBuildState
{
	Relation	index;
	LionIndexState ix;			/* every key column's state (DESIGN.md §24) */
	uint32		inline_limit;
	int			fillfactor;
	Size		leafbudget;		/* bytes of a page ambuild fills */

	Size		dirbudget;		/* ... and of an internal one (§21) */

	int			max_entries;	/* cardinality guard, 0 = unlimited */

	LionBuildCol *cols;			/* [ix.ncolumns] */
	LionBuildCol *cur;			/* the column being drained, or NULL */

	MemoryContext buildctx;		/* lives for the whole build */
	MemoryContext tmpctx;		/* reset per heap tuple / per key group */

	/*
	 * Page writing (see the file header): one bulk writer for the whole
	 * build, blocks handed out by a counter instead of by extending the
	 * relation, and the bucket pages kept in memory until pass 2 is over.
	 */
	BulkWriteState *bulk;
	BlockNumber nblocks;		/* blocks handed out so far */
	LionBuildLevel *leaf;		/* the bottom of the directory */
	int64		ndirpages;
	int64		ndistinct;		/* entries written, for the §17 guard */
	BlockNumber root;
	uint32		height;

	LionBuilder **builders;		/* open builders of the current hash */
	int			nbuilders;
	int			maxbuilders;

	double		indtuples;		/* TIDs pushed into the index */
} LionBuildState;

static void lion_build_callback(Relation index, ItemPointer tid, Datum *values,
							   bool *isnull, bool tupleIsAlive, void *arg);
static void lion_builder_flush(LionBuildState *bs, LionBuilder *b);
static void lion_builder_close_segment(LionBuildState *bs, LionBuilder *b);

/* ---------------------------------------------------------------------
 * Page writing
 *
 * During a build nobody else can see the index, so pages are prepared in
 * backend-local memory and handed to the bulk writer, which writes each of
 * them exactly once and WAL-logs them in batches.  Block numbers come from a
 * counter: the meta page is block 0 and everything else takes the next free
 * block as it is needed, so the leaves come out at the front of the file in
 * key order and the internal pages follow.
 * --------------------------------------------------------------------- */

static BlockNumber
lion_build_alloc_block(LionBuildState *bs)
{
	return bs->nblocks++;
}

static void lion_build_level_add(LionBuildState *bs, LionBuildLevel *lv,
								const LionEntryTuple *item, Size size);

static BulkWriteBuffer
lion_build_get_page(LionBuildState *bs, uint16 flags)
{
	BulkWriteBuffer buf = smgr_bulk_get_buf(bs->bulk);

	lion_init_page((Page) buf->data, flags);

	return buf;
}

/*
 * Write the meta page.  Its root is filled in at the end of the build, so the
 * image is kept until then; the bulk writer is happy to take block 0 last.
 */
static void
lion_build_init_pages(LionBuildState *bs)
{
	BlockNumber blk PG_USED_FOR_ASSERTS_ONLY;

	blk = lion_build_alloc_block(bs);
	Assert(blk == LION_METAPAGE_BLKNO);
}

static LionBuildLevel *
lion_build_level(LionBuildState *bs, uint16 level)
{
	LionBuildLevel *lv = (LionBuildLevel *)
		MemoryContextAllocZero(bs->buildctx, sizeof(LionBuildLevel));

	lv->level = level;
	lv->maxitems = 64;
	lv->items = (char **) MemoryContextAlloc(bs->buildctx,
											 sizeof(char *) * lv->maxitems);
	lv->sizes = (Size *) MemoryContextAlloc(bs->buildctx,
											sizeof(Size) * lv->maxitems);
	lv->blkno = InvalidBlockNumber;
	lv->leftblk = InvalidBlockNumber;

	return lv;
}

static LionBuildLevel *
lion_build_parent(LionBuildState *bs, LionBuildLevel *lv)
{
	if (lv->parent == NULL)
		lv->parent = lion_build_level(bs, lv->level + 1);
	return lv->parent;
}

/* A pivot tuple carrying src's key: a high key, or a downlink to child. */
static LionEntryTuple *
lion_build_pivot(const LionEntryTuple *src, uint16 pivotflag, BlockNumber child,
				Size *size)
{
	Size		keylen = (src != NULL) ? src->keylen : 0;
	Size		total = MAXALIGN(LION_ENTRY_HDRSZ + keylen);
	LionEntryTuple *p = (LionEntryTuple *) palloc0(total);

	p->hash = (src != NULL) ? src->hash : 0;
	p->flags = pivotflag |
		(uint16) ((src != NULL) ?
				  (src->flags & (LION_ENTRY_RESERVED | LION_ENTRY_MINUSINF)) :
				  LION_ENTRY_MINUSINF);
	p->keylen = (uint16) keylen;
	p->head = child;
	p->tail = InvalidBlockNumber;
	/* A pivot routes to the column its source key belongs to (§24). */
	p->attno = (src != NULL) ? src->attno : 0;
	if (keylen > 0)
		memcpy(LionEntryGetKey(p), LionEntryGetKey(src), keylen);

	*size = total;
	return p;
}

/* What a pivot copy of this item's key costs on a page. */
static inline Size
lion_build_pivot_need(const LionEntryTuple *item)
{
	return MAXALIGN(LION_ENTRY_HDRSZ + item->keylen) + sizeof(ItemIdData);
}

/* What this item costs on a page, line pointer in. */
static inline Size
lion_build_item_need(Size size)
{
	return MAXALIGN(size) + sizeof(ItemIdData);
}

/*
 * Write the open page of one level and start the next.  hkey is the key of
 * the item that did not fit - the first key of the page to the right, and
 * therefore this page's high key - or NULL when this is the last page of the
 * level.  isroot marks it as the tree's root and suppresses the downlink.
 *
 * The high key may be much larger than anything already on the page (keys are
 * variable length, and one of LION_MAX_KEY_SIZE arriving behind a page full of
 * short ones is the case that used to overflow the page).  What makes the
 * reserve exact is nbtree's rule in _bt_buildadd(): the item that becomes the
 * FIRST of the next page is the one whose key becomes the high key, so when
 * hkey does not fit, trailing items are moved to the next page instead and the
 * first of THEM supplies the high key - a pivot copy of a key that was already
 * on this page, which by construction is never larger than the item it came
 * from.
 */
static void
lion_build_level_flush(LionBuildState *bs, LionBuildLevel *lv,
					  const LionEntryTuple *hkey, bool isroot)
{
	BulkWriteBuffer buf;
	Page		page;
	BlockNumber nextblk = InvalidBlockNumber;
	BlockNumber thisblk;
	LionEntryTuple *pivot;
	Size		pivotsz;
	const LionEntryTuple *hksrc = hkey;
	Size		hkneed = 0;
	int			nkeep = lv->nitems;
	Size		keepused = lv->used;
	int			i;

	if (lv->nitems == 0 && lv->npages > 0)
		return;					/* nothing left over to write */

	if (hksrc != NULL)
	{
		hkneed = lion_build_pivot_need(hksrc);
		while (nkeep > 1 && keepused + hkneed > LION_PAGE_CAPACITY)
		{
			nkeep--;
			keepused -= lion_build_item_need(lv->sizes[nkeep]);
			hksrc = (const LionEntryTuple *) lv->items[nkeep];
			hkneed = lion_build_pivot_need(hksrc);
		}
		if (keepused + hkneed > LION_PAGE_CAPACITY)
			elog(ERROR, "lion index: a directory page cannot hold one item of %zu bytes and its high key",
				 lv->sizes[0]);
	}
	Assert(keepused + hkneed <= LION_PAGE_CAPACITY);
	/*
	 * Every internal page but the last of its level carries at least two
	 * downlinks, which is what makes the level strictly smaller than the one
	 * below it and lion_build_finish_dir() terminate.
	 */
	Assert(lv->level == 0 || hksrc == NULL || nkeep >= LION_ABS_MIN_DOWNLINKS);

	if (!BlockNumberIsValid(lv->blkno))
		lv->blkno = lion_build_alloc_block(bs);
	thisblk = lv->blkno;
	if (hksrc != NULL)
		nextblk = lion_build_alloc_block(bs);

	buf = smgr_bulk_get_buf(bs->bulk);
	page = (Page) buf->data;
	lion_init_page(page, (uint16) ((lv->level == 0 ? LION_PAGE_BUCKET :
									LION_PAGE_DIR) |
								   (isroot ? LION_PAGE_ROOT : 0)));
	LionPageGetOpaque(page)->level = lv->level;
	LionPageGetOpaque(page)->leftlink = lv->leftblk;
	LionPageGetOpaque(page)->rightlink = nextblk;

	if (hksrc != NULL)
	{
		pivot = lion_build_pivot(hksrc, LION_ENTRY_HIGHKEY, InvalidBlockNumber,
								 &pivotsz);
		if (PageAddItemExtended(page, (char *) pivot, pivotsz,
								FirstOffsetNumber, 0) == InvalidOffsetNumber)
			elog(ERROR, "lion index: failed to place a high key");
		pfree(pivot);
	}

	for (i = 0; i < nkeep; i++)
	{
		if (PageAddItemExtended(page, lv->items[i], lv->sizes[i],
								InvalidOffsetNumber, 0) == InvalidOffsetNumber)
			elog(ERROR, "lion index: failed to place a directory item");
	}

	smgr_bulk_write(bs->bulk, thisblk, buf, true);
	bs->ndirpages++;

	/*
	 * Hand the downlink up, unless this page is the root.  The first page of
	 * every level gets the minus-infinity separator, which is what makes the
	 * leftmost path of the tree reachable for any key at all.
	 */
	if (!isroot)
	{
		pivot = lion_build_pivot(lv->npages == 0 ? NULL :
								 (const LionEntryTuple *) lv->items[0],
								 LION_ENTRY_DOWNLINK, thisblk, &pivotsz);
		lion_build_level_add(bs, lion_build_parent(bs, lv), pivot, pivotsz);
		pfree(pivot);
	}
	else
	{
		bs->root = thisblk;
		bs->height = lv->level;
	}

	/* The items that did not stay open the next page. */
	for (i = 0; i < nkeep; i++)
		pfree(lv->items[i]);
	for (i = nkeep; i < lv->nitems; i++)
	{
		lv->items[i - nkeep] = lv->items[i];
		lv->sizes[i - nkeep] = lv->sizes[i];
	}
	lv->nitems -= nkeep;
	lv->used -= keepused;
	lv->npages++;
	lv->leftblk = thisblk;
	lv->blkno = nextblk;
}

/*
 * Add one item to a level, flushing the open page first when it no longer
 * fits.
 *
 * The leaves are filled to `fillfactor`; the internal levels are NOT
 * (DESIGN.md §21 and LION_NONLEAF_FILLFACTOR): a low fillfactor applied to an
 * internal page can leave it with a single downlink, which makes every level
 * as large as the one below and the bottom-up build never terminates.  An
 * internal page therefore always takes at least LION_ABS_MIN_DOWNLINKS
 * downlinks - it aims for LION_MIN_DOWNLINKS and settles for two when the
 * keys are too large for three - whatever the fill target says.
 */
static void
lion_build_level_add(LionBuildState *bs, LionBuildLevel *lv,
					const LionEntryTuple *item, Size size)
{
	Size		need = lion_build_item_need(size);
	Size		hkneed = lion_build_pivot_need(item);
	Size		budget = (lv->level == 0) ? bs->leafbudget : bs->dirbudget;
	int			minitems = (lv->level == 0) ? 1 : LION_MIN_DOWNLINKS;
	MemoryContext oldctx;

	/*
	 * A flush may CARRY trailing items onto the next page (see there), so the
	 * page this item lands on can still be too full for it; each flush writes
	 * at least one item out, so this settles.
	 */
	while (lv->nitems > 0 && lv->used + need + hkneed > budget &&
		   (lv->nitems >= minitems ||
			lv->used + need + hkneed > LION_PAGE_CAPACITY))
		lion_build_level_flush(bs, lv, item, false);

	Assert(lv->nitems == 0 || lv->used + need <= LION_PAGE_CAPACITY);

	if (lv->nitems >= lv->maxitems)
	{
		oldctx = MemoryContextSwitchTo(bs->buildctx);
		lv->maxitems *= 2;
		lv->items = (char **) repalloc(lv->items, sizeof(char *) * lv->maxitems);
		lv->sizes = (Size *) repalloc(lv->sizes, sizeof(Size) * lv->maxitems);
		MemoryContextSwitchTo(oldctx);
	}

	if (!BlockNumberIsValid(lv->blkno))
		lv->blkno = lion_build_alloc_block(bs);

	lv->items[lv->nitems] = (char *) MemoryContextAlloc(bs->buildctx, size);
	memcpy(lv->items[lv->nitems], item, size);
	lv->sizes[lv->nitems] = size;
	lv->nitems++;
	lv->used += need;
}

/* One entry tuple, in directory order. */
static void
lion_build_add_entry(LionBuildState *bs, LionEntryTuple *entry, Size size)
{
	lion_build_level_add(bs, bs->leaf, entry, size);
	bs->ndistinct++;
}

/*
 * Close the directory: flush the last page of every level from the bottom up,
 * and stop at the level whose last page is also its first - that page is the
 * root.  An index with no entries at all gets one empty leaf, which is the
 * root (DESIGN.md §21).
 */
static void
lion_build_finish_dir(LionBuildState *bs)
{
	LionBuildLevel *lv = bs->leaf;

	for (;;)
	{
		bool		isroot = (lv->npages == 0);
		int64		below;

		lion_build_level_flush(bs, lv, NULL, isroot);
		if (isroot)
			return;

		below = lv->npages;
		lv = lion_build_parent(bs, lv);

		/*
		 * Every level is strictly smaller than the one below it, because an
		 * internal page carries at least two downlinks (lion_build_level_add()).
		 * That is what makes this loop terminate, so it is checked rather than
		 * assumed: a level as large as the one below would climb for ever.
		 */
		if (lv->npages + (lv->nitems > 0 ? 1 : 0) >= below)
			elog(ERROR, "lion index: directory level %u has " INT64_FORMAT " pages, not fewer than the " INT64_FORMAT " below it",
				 lv->level, lv->npages + (lv->nitems > 0 ? 1 : 0), below);
	}
}

/* ---------------------------------------------------------------------
 * Posting set builders
 * --------------------------------------------------------------------- */

static LionBuilder *
lion_builder_create(LionBuildState *bs, Datum key, int keykind, uint32 hash)
{
	LionBuilder *b = (LionBuilder *) palloc0(sizeof(LionBuilder));
	LionState  *cs = bs->cur->state;

	b->keykind = keykind;
	b->key = (keykind != LION_KEY_REAL) ? (Datum) 0 :
		datumCopy(key, cs->typbyval, cs->typlen);
	b->hash = hash;
	/*
	 * The stored bytes, which are the tail of the directory order
	 * (DESIGN.md §21): two keys whose prefixes tie - which for an unordered
	 * opclass means two keys of one HASH - are ordered by them, and the
	 * flush below has to put them out that way.
	 */
	if (keykind == LION_KEY_REAL)
	{
		b->rawlen = lion_key_datum_size(cs, b->key);
		b->rawkey = (char *) palloc(b->rawlen);
		lion_store_key(cs, b->key, b->rawkey);
	}
	b->cur = (LionContainer *) palloc(LION_CONTAINER_MAX_SIZE);
	b->cbuf = (LionContainer *) palloc(LION_CONTAINER_MAX_SIZE);
	b->seg = (LionContainer *) palloc(LION_CONTAINER_MAX_SIZE);
	lion_sparse_init(b->seg, 0);
	b->npend = 0;
	b->hasgroup = false;
	b->inlinebuf = (char *) palloc0(bs->inline_limit);
	b->inlineused = 0;
	/* the bound lion_insert.c spills at, so that a rebuild can write it too */
	b->inlinemax = lion_inline_max((keykind == LION_KEY_REAL) ?
								   MAXALIGN(LION_ENTRY_HDRSZ + b->rawlen) :
								   MAXALIGN(LION_ENTRY_HDRSZ),
								   (Size) bs->inline_limit);
	b->spilled = false;
	b->head = InvalidBlockNumber;
	b->curblk = InvalidBlockNumber;
	b->haspage = false;
	b->pagebuf = NULL;

	if (bs->nbuilders >= bs->maxbuilders)
	{
		MemoryContext old = MemoryContextSwitchTo(bs->buildctx);

		bs->maxbuilders *= 2;
		bs->builders = (LionBuilder **) repalloc(bs->builders,
												sizeof(LionBuilder *) * bs->maxbuilders);
		MemoryContextSwitchTo(old);
	}
	bs->builders[bs->nbuilders++] = b;

	return b;
}

/* ---------------------------------------------------------------------
 * The posting tree above a key's leaves (DESIGN.md §22)
 * --------------------------------------------------------------------- */

static void lion_post_level_add(LionBuildState *bs, LionBuilder *b,
								LionPostLevel *lv, uint32 ckey,
								BlockNumber child);

static LionPostLevel *
lion_post_level(LionBuildState *bs, uint16 level)
{
	LionPostLevel *lv = (LionPostLevel *) palloc0(sizeof(LionPostLevel));

	lv->level = level;
	lv->maxitems = 64;
	lv->items = (LionPostingPivot *)
		palloc(sizeof(LionPostingPivot) * lv->maxitems);
	lv->blkno = InvalidBlockNumber;

	return lv;
}

static LionPostLevel *
lion_post_parent(LionBuildState *bs, LionPostLevel *lv)
{
	if (lv->parent == NULL)
	{
		if (lv->level >= LION_POSTING_MAX_HEIGHT)
			elog(ERROR, "lion index: posting tree is deeper than %d levels",
				 LION_POSTING_MAX_HEIGHT);
		lv->parent = lion_post_level(bs, lv->level + 1);
	}
	return lv->parent;
}

/*
 * Write the open page of one internal level and start the next.  hk is the
 * separator of the downlink that did not fit - the first key of the page to
 * the right, and therefore this page's high key - or NULL when this is the
 * last page of the level.  isroot writes it at the block the set reserved for
 * its root and suppresses the downlink.
 */
static void
lion_post_level_flush(LionBuildState *bs, LionBuilder *b, LionPostLevel *lv,
					  const uint32 *hk, bool isroot)
{
	BulkWriteBuffer buf;
	Page		page;
	BlockNumber thisblk;
	BlockNumber nextblk = InvalidBlockNumber;
	LionPostingPivot pivot;
	int			i;

	if (lv->nitems == 0 && lv->npages > 0)
		return;					/* nothing left over to write */

	if (BlockNumberIsValid(lv->blkno))
		thisblk = lv->blkno;
	else if (isroot)
		thisblk = b->rootblk;
	else
		thisblk = lion_build_alloc_block(bs);
	if (hk != NULL)
		nextblk = lion_build_alloc_block(bs);

	buf = lion_build_get_page(bs, LION_PAGE_CONTAINER);
	page = (Page) buf->data;
	LionPageGetOpaque(page)->level = lv->level;
	LionPageGetOpaque(page)->rightlink = nextblk;
	lion_page_set_owner(page, b->hash, b->head);

	if (hk != NULL)
	{
		pivot.ckey = *hk;
		pivot.child = InvalidBlockNumber;
		if (PageAddItemExtended(page, (char *) &pivot, LION_POSTING_PIVOT_SIZE,
								FirstOffsetNumber, 0) == InvalidOffsetNumber)
			elog(ERROR, "lion index: failed to place a posting high key");
	}

	for (i = 0; i < lv->nitems; i++)
	{
		if (PageAddItemExtended(page, (char *) &lv->items[i],
								LION_POSTING_PIVOT_SIZE, InvalidOffsetNumber,
								0) == InvalidOffsetNumber)
			elog(ERROR, "lion index: failed to place a posting downlink");
	}

	smgr_bulk_write(bs->bulk, thisblk, buf, true);

	if (isroot)
		b->height = lv->level;
	else
		lion_post_level_add(bs, b, lion_post_parent(bs, lv),
							lv->items[0].ckey, thisblk);

	lv->nitems = 0;
	lv->used = 0;
	lv->npages++;
	lv->blkno = nextblk;
}

static void
lion_post_level_add(LionBuildState *bs, LionBuilder *b, LionPostLevel *lv,
					uint32 ckey, BlockNumber child)
{
	Size		need = MAXALIGN(LION_POSTING_PIVOT_SIZE) + sizeof(ItemIdData);

	/* Room for the downlink AND for the high key the page may still need. */
	if (lv->nitems >= LION_ABS_MIN_DOWNLINKS &&
		lv->used + 2 * need > (Size) LION_PAGE_CAPACITY)
		lion_post_level_flush(bs, b, lv, &ckey, false);

	if (lv->nitems >= lv->maxitems)
	{
		lv->maxitems *= 2;
		lv->items = (LionPostingPivot *)
			repalloc(lv->items, sizeof(LionPostingPivot) * lv->maxitems);
	}

	/* The first downlink of every level's first page is minus infinity. */
	lv->items[lv->nitems].ckey =
		(lv->npages == 0 && lv->nitems == 0) ? 0 : ckey;
	lv->items[lv->nitems].child = child;
	lv->nitems++;
	lv->used += need;
}

/*
 * The set needs a second leaf, so it needs a ROOT above them.  Reserve its
 * block and re-stamp the first leaf, whose image has not been written yet.
 */
static void
lion_builder_start_tree(LionBuildState *bs, LionBuilder *b, Page firstleaf)
{
	Assert(!b->hastree);

	b->rootblk = lion_build_alloc_block(bs);
	b->head = b->rootblk;
	b->hastree = true;
	b->plevel = lion_post_level(bs, 1);
	lion_page_set_owner(firstleaf, b->hash, b->head);
}

/*
 * Close the posting tree: flush the last page of every internal level from
 * the bottom up, and stop at the level whose last page is also its first -
 * that page is the root, and it goes to the reserved block.
 */
static void
lion_builder_finish_tree(LionBuildState *bs, LionBuilder *b)
{
	LionPostLevel *lv = b->plevel;

	for (;;)
	{
		bool		isroot = (lv->npages == 0);
		int64		below;

		lion_post_level_flush(bs, b, lv, NULL, isroot);
		if (isroot)
			return;

		below = lv->npages;
		lv = lion_post_parent(bs, lv);

		/*
		 * Every level is strictly smaller than the one below it, because an
		 * internal page carries at least LION_ABS_MIN_DOWNLINKS downlinks.
		 * That is what makes this loop terminate, so it is checked.
		 */
		if (lv->npages + (lv->nitems > 0 ? 1 : 0) >= below)
			elog(ERROR, "lion index: posting level %u has " INT64_FORMAT " pages, not fewer than the " INT64_FORMAT " below it",
				 lv->level, lv->npages + (lv->nitems > 0 ? 1 : 0), below);
	}
}

/*
 * Append one finished container to the posting set under construction.
 */
static void
lion_builder_spill(LionBuildState *bs, LionBuilder *b, LionContainer *c)
{
	Size		csize = lion_item_size(c);
	Page		img;

	if (!b->haspage)
	{
		b->pagebuf = lion_build_get_page(bs, LION_PAGE_CONTAINER);
		b->curblk = lion_build_alloc_block(bs);
		b->head = b->curblk;
		b->haspage = true;
		/* DESIGN.md §18: every container page names the set it belongs to. */
		lion_page_set_owner((Page) b->pagebuf->data, b->hash, b->head);
	}

	img = (Page) b->pagebuf->data;

	if (PageGetFreeSpace(img) < MAXALIGN(csize))
	{
		BlockNumber next;

		/*
		 * The page is final, and a second leaf means the set is a TREE: the
		 * root block is reserved now and this first leaf is re-stamped with
		 * it before it goes out, because every page of a set carries its root
		 * block as the owner stamp (DESIGN.md §18, §22).
		 */
		if (!b->hastree)
			lion_builder_start_tree(bs, b, img);

		next = lion_build_alloc_block(bs);
		lion_page_update_minmax(img);
		LionPageGetOpaque(img)->rightlink = next;
		lion_post_level_add(bs, b, b->plevel,
							LionPageGetOpaque(img)->minckey, b->curblk);
		/* the page is final: hand the image itself to the bulk writer */
		smgr_bulk_write(bs->bulk, b->curblk, b->pagebuf, true);

		b->curblk = next;
		b->pagebuf = lion_build_get_page(bs, LION_PAGE_CONTAINER);
		img = (Page) b->pagebuf->data;
		lion_page_set_owner(img, b->hash, b->head);
	}

	if (PageAddItemExtended(img, c, csize, InvalidOffsetNumber, 0) ==
		InvalidOffsetNumber)
		elog(ERROR, "lion index: failed to add container to build page");
}

static void
lion_builder_emit(LionBuildState *bs, LionBuilder *b, LionContainer *c)
{
	Size		csize = lion_item_size(c);

	if (!b->spilled)
	{
		if (b->inlineused + csize <= b->inlinemax)
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
			while (lion_inline_fetch(b->inlinebuf, used, &off, b->cbuf) > 0)
				lion_builder_spill(bs, b, b->cbuf);
		}
	}

	lion_builder_spill(bs, b, c);
}

/*
 * Emit the open sparse segment, if there is one, and start a new one.
 */
static void
lion_builder_close_segment(LionBuildState *bs, LionBuilder *b)
{
	if (b->seg->cardinality == 0)
		return;

	lion_builder_emit(bs, b, b->seg);
	b->nitems++;
	lion_sparse_init(b->seg, 0);
}

/*
 * Close the ckey group that is being collected: a dense one has already
 * become a container in b->cur and is emitted here, a sparse one appends its
 * few members to the open segment.
 */
static void
lion_builder_finish_group(LionBuildState *bs, LionBuilder *b)
{
	if (!b->hasgroup)
		return;

	if (b->hascur)
	{
		lion_container_optimize(b->cur);
		lion_builder_emit(bs, b, b->cur);
		b->nitems++;
		b->hascur = false;
	}
	else if (b->npend > 0)
	{
		int			i;

		/* A ckey lives in one item only, so never split a group in two. */
		if ((uint32) b->seg->cardinality + (uint32) b->npend >
			LION_SPARSE_MAX_PAIRS)
			lion_builder_close_segment(bs, b);

		for (i = 0; i < b->npend; i++)
		{
			if (!lion_sparse_insert(b->seg, b->curckey, b->pend[i], NULL))
				elog(ERROR, "lion index: sparse segment overflowed during build");
		}
	}

	b->npend = 0;
	b->hasgroup = false;
}

static void
lion_builder_add(LionBuildState *bs, LionBuilder *b, uint64 code)
{
	uint32		ckey = lion_code_ckey(code);
	uint16		lo = lion_code_lo(code);

	if (!b->hasgroup || b->curckey != ckey)
	{
		lion_builder_finish_group(bs, b);
		b->curckey = ckey;
		b->hasgroup = true;
	}

	if (b->hascur)
		lion_container_append_sorted(b->cur, lo);
	else
	{
		Assert(b->npend < LION_SPARSE_THRESHOLD);
		b->pend[b->npend++] = lo;

		if (b->npend >= LION_SPARSE_THRESHOLD)
		{
			int			i;

			/*
			 * This ckey is dense enough for a container.  The container's
			 * range sits after every pair collected so far, so the open
			 * segment has to be emitted before it (DESIGN.md §13: item ranges
			 * must not interleave).
			 */
			lion_builder_close_segment(bs, b);

			lion_container_init(b->cur, ckey);
			for (i = 0; i < b->npend; i++)
				lion_container_append_sorted(b->cur, b->pend[i]);
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
lion_builder_flush(LionBuildState *bs, LionBuilder *b)
{
	LionEntryTuple *entry;
	Size		size;

	lion_builder_finish_group(bs, b);
	lion_builder_close_segment(bs, b);

	if (b->spilled)
	{
		Page		img = (Page) b->pagebuf->data;

		Assert(b->haspage);
		lion_page_update_minmax(img);
		LionPageGetOpaque(img)->rightlink = InvalidBlockNumber;
		if (b->hastree)
			lion_post_level_add(bs, b, b->plevel,
								LionPageGetOpaque(img)->minckey, b->curblk);
		smgr_bulk_write(bs->bulk, b->curblk, b->pagebuf, true);
		b->pagebuf = NULL;
		b->haspage = false;

		/* The internal levels, bottom-up; the root lands on b->head. */
		if (b->hastree)
			lion_builder_finish_tree(bs, b);

		entry = (b->keykind != LION_KEY_REAL) ?
			lion_make_reserved_entry((AttrNumber) bs->cur->state->attno,
									lion_reserved_flag(b->keykind),
									LION_ENTRY_CHAIN, NULL, 0, &size) :
			lion_make_entry(bs->cur->state, b->key, b->hash, LION_ENTRY_CHAIN,
						   NULL, 0, &size);
		entry->head = b->head;
		entry->tail = b->curblk;
	}
	else
	{
		entry = (b->keykind != LION_KEY_REAL) ?
			lion_make_reserved_entry((AttrNumber) bs->cur->state->attno,
									lion_reserved_flag(b->keykind),
									LION_ENTRY_INLINE, b->inlinebuf,
									b->inlineused, &size) :
			lion_make_entry(bs->cur->state, b->key, b->hash, LION_ENTRY_INLINE,
						   b->inlinebuf, b->inlineused, &size);
	}

	entry->ncontainers = b->nitems;
	entry->ntids = b->ntids;

	lion_build_add_entry(bs, entry, size);

	pfree(entry);
}

/*
 * Two open builders, in the DIRECTORY order of DESIGN.md §21 - the same
 * (kind, comparison, hash, stored bytes) that lion_cmp_entry() applies to two
 * entries on a leaf.
 */
static int
lion_builder_cmp(const void *a, const void *b, void *arg)
{
	LionBuildState *bs = (LionBuildState *) arg;
	LionState  *cs = bs->cur->state;
	const LionBuilder *x = *(LionBuilder *const *) a;
	const LionBuilder *y = *(LionBuilder *const *) b;
	Size		n;
	int			c;

	if (x->keykind != y->keykind)
		return x->keykind < y->keykind ? -1 : 1;
	if (x->keykind != LION_KEY_REAL)
		return 0;				/* one NULL and one EMPTY entry per column */

	if (cs->ordered)
	{
		c = DatumGetInt32(FunctionCall2Coll(&cs->cmpproc, cs->collation,
											x->key, y->key));
		if (c != 0)
			return c < 0 ? -1 : 1;
	}

	if (x->hash != y->hash)
		return x->hash < y->hash ? -1 : 1;

	n = Min(x->rawlen, y->rawlen);
	if (n > 0)
	{
		c = memcmp(x->rawkey, y->rawkey, n);
		if (c != 0)
			return c < 0 ? -1 : 1;
	}
	if (x->rawlen != y->rawlen)
		return x->rawlen < y->rawlen ? -1 : 1;
	return 0;
}

/*
 * Close every open builder, in directory order.
 *
 * The sort is what a hash collision needs (DESIGN.md §21).  The tuplesort
 * brings the tuples of one HASH together but says nothing about the order of
 * the several distinct keys inside it, so the builders were created in the
 * order the first TID of each key happened to arrive; writing them out that
 * way would put the leaf items out of order, which breaks the binary search
 * and the key-based resume of lion_entry_scan_next().  There are as many
 * builders as there are distinct keys of one hash, which is one unless the
 * hash function collides.
 */
static void
lion_flush_builders(LionBuildState *bs)
{
	int			i;

	if (bs->nbuilders > 1)
		qsort_arg(bs->builders, bs->nbuilders, sizeof(LionBuilder *),
				  lion_builder_cmp, bs);

	for (i = 0; i < bs->nbuilders; i++)
		lion_builder_flush(bs, bs->builders[i]);
	bs->nbuilders = 0;
}

/* ---------------------------------------------------------------------
 * The build itself
 * --------------------------------------------------------------------- */

/*
 * Push one (kind, key, code) tuple into one column's sort.  A reserved kind
 * carries no key at all and hashes to 0.
 */
static void
lion_build_put(LionBuildState *bs, LionBuildCol *col, int keykind, Datum key,
			   uint64 code)
{
	uint32		hash = (keykind == LION_KEY_REAL) ?
		lion_hash_key(col->state, key) : LION_NULLKEY_HASH;

	ExecClearTuple(col->inslot);
	col->inslot->tts_values[0] = Int32GetDatum((int32) keykind);
	col->inslot->tts_isnull[0] = false;
	col->inslot->tts_values[1] = Int64GetDatum((int64) hash);
	col->inslot->tts_isnull[1] = false;
	col->inslot->tts_values[2] = Int64GetDatum((int64) code);
	col->inslot->tts_isnull[2] = false;
	col->inslot->tts_values[3] = key;
	col->inslot->tts_isnull[3] = (keykind != LION_KEY_REAL);
	ExecStoreVirtualTuple(col->inslot);

	tuplesort_puttupleslot(col->sortstate, col->inslot);

	bs->indtuples += 1;
}

static void
lion_build_callback(Relation index, ItemPointer tid, Datum *values,
				   bool *isnull, bool tupleIsAlive, void *arg)
{
	LionBuildState *bs = (LionBuildState *) arg;
	MemoryContext oldctx;
	uint64		code;
	int			c;

	lion_check_key_offset(tid);
	code = lion_tid_to_code(tid);

	oldctx = MemoryContextSwitchTo(bs->tmpctx);

	/* One row contributes to every key column (DESIGN.md §24). */
	for (c = 0; c < bs->ix.ncolumns; c++)
	{
		LionBuildCol *col = &bs->cols[c];
		Datum		key;

		/*
		 * A NULL value goes into the sort like any other row, with hash 0: it
		 * ends up in that column's reserved NULL entry (DESIGN.md §14), and
		 * the pass below tells it from a real key by the kind column, never
		 * by comparing.
		 */
		if (isnull[c])
			lion_build_put(bs, col, LION_KEY_NULL, (Datum) 0, code);
		else if (col->multikey)
		{
			/*
			 * DESIGN.md §17: one row, many keys.  A row the opclass extracts
			 * nothing from - an empty array, a tsvector with no lexemes -
			 * goes into the reserved EMPTY entry, so that a scan that has to
			 * look at every indexed row (`tags @> '{}'`) can still find it.
			 */
			Datum	   *keys;
			int			nkeys = lion_extract_value(col->state, values[c],
												   &keys);
			int			i;

			if (nkeys == 0)
				lion_build_put(bs, col, LION_KEY_EMPTY, (Datum) 0, code);
			for (i = 0; i < nkeys; i++)
				lion_build_put(bs, col, LION_KEY_REAL, keys[i], code);
		}
		else
		{
			key = values[c];
			if (!col->state->typbyval && col->state->typlen == -1)
				key = PointerGetDatum(PG_DETOAST_DATUM(key));

			lion_build_put(bs, col, LION_KEY_REAL, key, code);
		}
	}

	MemoryContextSwitchTo(oldctx);
	MemoryContextReset(bs->tmpctx);
}

/*
 * The one pass over the sorted data: group by (kind, key) and write out the
 * posting sets, which come out in exactly the directory order of DESIGN.md
 * §21 because that is what the sort keys are.
 *
 * With an ordering the group boundary is "the key changed", and since equal
 * keys sort together there is at most one open builder.  Without one the sort
 * can only bring equal HASHES together, so a hash run may interleave several
 * distinct keys and each gets a builder of its own, flushed in the order they
 * were created - which is the order of their stored bytes, and therefore the
 * directory order again.
 */
static void
lion_build_write_entries(LionBuildState *bs, LionBuildCol *col)
{
	LionState  *cs = col->state;
	uint32		curhash = 0;
	int			curkind = -1;
	bool		havegroup = false;
	MemoryContext oldctx;

	bs->cur = col;
	oldctx = MemoryContextSwitchTo(bs->tmpctx);

	while (tuplesort_gettupleslot(col->sortstate, true, false, col->outslot,
								  NULL))
	{
		bool		isnull;
		uint32		hash;
		Datum		key;
		uint64		code;
		int			keykind;
		LionBuilder *b = NULL;
		bool		boundary;
		int			i;

		keykind = (int) DatumGetInt32(slot_getattr(col->outslot, 1, &isnull));
		hash = (uint32) DatumGetInt64(slot_getattr(col->outslot, 2, &isnull));
		code = (uint64) DatumGetInt64(slot_getattr(col->outslot, 3, &isnull));
		key = slot_getattr(col->outslot, 4, &isnull);

		if (!havegroup)
			boundary = false;
		else if (keykind != curkind)
			boundary = true;
		else if (cs->ordered && keykind == LION_KEY_REAL)
			boundary = !lion_keys_equal(cs, bs->builders[0]->key, key);
		else
			boundary = (hash != curhash);

		if (boundary)
		{
			lion_flush_builders(bs);
			MemoryContextReset(bs->tmpctx);
		}
		curhash = hash;
		curkind = keykind;
		havegroup = true;

		/* Group by the kind first, then by key (DESIGN.md §14 and §17). */
		for (i = 0; i < bs->nbuilders; i++)
		{
			if (bs->builders[i]->keykind != keykind)
				continue;
			if (keykind != LION_KEY_REAL ||
				lion_keys_equal(cs, bs->builders[i]->key, key))
			{
				b = bs->builders[i];
				break;
			}
		}
		if (b == NULL)
			b = lion_builder_create(bs, key, keykind, hash);

		lion_builder_add(bs, b, code);

		CHECK_FOR_INTERRUPTS();
	}

	lion_flush_builders(bs);
	MemoryContextSwitchTo(oldctx);
	MemoryContextReset(bs->tmpctx);
	bs->cur = NULL;
}

IndexBuildResult *
lionbuild(Relation heap, Relation index, IndexInfo *indexInfo)
{
	IndexBuildResult *result;
	LionBuildState bs;
	LionOptions *opts = (LionOptions *) index->rd_options;
	LionMetaPageData meta;
	double		reltuples;
	BulkWriteBuffer metabuf;
	int			ncols;
	int			sortmem;
	int			c;

	if (RelationGetNumberOfBlocks(index) != 0)
		elog(ERROR, "index \"%s\" already contains data",
			 RelationGetRelationName(index));

	memset(&bs, 0, sizeof(bs));
	bs.index = index;
	bs.indtuples = 0;
	bs.inline_limit = opts ? (uint32) opts->inline_limit : LION_DEFAULT_INLINE_LIMIT;
	bs.fillfactor = opts ? opts->fillfactor : LION_DEFAULT_FILLFACTOR;
	bs.max_entries = lion_max_entries(index);
	bs.root = InvalidBlockNumber;
	bs.height = 0;

	/*
	 * How full a directory page is packed (DESIGN.md §21).  A LEAF needs no
	 * floor: lion_build_level_add() accepts the FIRST item of a page whatever
	 * the budget says, and one entry plus the high key a split would give it
	 * always fits a page by construction (LION_MAX_ENTRY_SIZE).  An INTERNAL
	 * page keeps its own fill - `fillfactor` is about leaving room for later
	 * inserts into the leaves - and a floor of two downlinks, without which a
	 * low fillfactor makes every level as large as the one below and the build
	 * never reaches a root.
	 */
	bs.leafbudget = Max(LION_PAGE_CAPACITY * (Size) bs.fillfactor / 100, 1);
	bs.dirbudget = LION_PAGE_CAPACITY * (Size) LION_NONLEAF_FILLFACTOR / 100;

	bs.buildctx = AllocSetContextCreate(CurrentMemoryContext,
										"lion index build",
										ALLOCSET_DEFAULT_SIZES);
	bs.tmpctx = AllocSetContextCreate(bs.buildctx,
									  "lion index build temporary",
									  ALLOCSET_DEFAULT_SIZES);

	/*
	 * The meta page does not exist yet, so build the relation state from the
	 * options directly.  The root is filled in once the tree has been built.
	 */
	memset(&meta, 0, sizeof(meta));
	meta.magic = LION_MAGIC;
	meta.version = LION_VERSION;
	meta.offset_bits = LION_OFFSET_BITS;
	meta.container_bits = LION_CONTAINER_BITS;
	meta.inline_limit = bs.inline_limit;
	meta.root = InvalidBlockNumber;
	lion_fill_index_state(index, &bs.ix, &meta, bs.buildctx);
	ncols = bs.ix.ncolumns;

	bs.maxbuilders = 8;
	bs.builders = (LionBuilder **) MemoryContextAlloc(bs.buildctx,
													 sizeof(LionBuilder *) * bs.maxbuilders);
	bs.nbuilders = 0;

	/*
	 * Sort tuple: (kind int4, hash int8, code int8, key), sorted into the
	 * DIRECTORY order of DESIGN.md §21 so that the entries come out ready to
	 * be written left to right.
	 *
	 * The order the directory wants is (kind, key, hash) with NULL < EMPTY <
	 * value, and the sort keys below say that WITHOUT putting `kind` first:
	 *
	 *	ordered	  (key ASC NULLS FIRST, code).  A reserved entry has no key at
	 *			  all, so its NULL sorts before every value, and a real key is
	 *			  never NULL.  The HASH is not a sort key here: the directory
	 *			  order only consults it when the comparison TIES, and two keys
	 *			  the comparison calls equal are one entry, so there is nothing
	 *			  left to order.
	 *	otherwise (hash, code) - all a type with no btree opclass offers.  The
	 *			  reserved entries hash to 0, which is the minimum, so leading
	 *			  with the hash gives the same sequence as leading with the
	 *			  kind.
	 *	either	  plus `kind`, but ONLY for a multi-key opclass, which is the
	 *			  only kind that has an EMPTY entry to tell from the NULL one
	 *			  (both have no key and hash 0).
	 *
	 * Why it matters which one leads: tuplesort compares the LEADING key from
	 * a datum it precomputed at put time and every further key by fetching
	 * the attribute out of the tuple.  A leading `kind` is the same value for
	 * every real row, so every comparison would fall through to a fetch -
	 * measured at 1.43 s of sort against 0.15 s for one million keys.  The
	 * three fetched columns are therefore also the fixed-width ones, first in
	 * the descriptor, so that their offsets are cached.
	 *
	 * The hash goes in as int8, zero-extended.  It is a uint32 and the
	 * directory compares it as one (lion_cmp_prefix()), so sorting it as int4
	 * would put everything above 2^31 first and the build would lay the
	 * entries out in an order the search does not agree with - which is
	 * exactly what an index whose key type has no btree opclass, and whose
	 * order is therefore (kind, hash, bytes), is made of.
	 *
	 * The key column's type is the index's own, which core resolved from the
	 * opclass: the element type for a multi-key class (DESIGN.md §17), so one
	 * heap row's several keys sort as the values they are.
	 *
	 * All of that is PER KEY COLUMN (DESIGN.md §24): every column has its own
	 * key type, its own opclass and therefore its own answer to "ordered?", so
	 * it gets a tuplesort of its own, and they share maintenance_work_mem.
	 */
	bs.cols = (LionBuildCol *) MemoryContextAllocZero(bs.buildctx,
													  sizeof(LionBuildCol) * ncols);
	bs.cur = NULL;
	sortmem = Max(maintenance_work_mem / ncols, 64);

	for (c = 0; c < ncols; c++)
	{
		LionBuildCol *col = &bs.cols[c];
		LionState  *cs = &bs.ix.cols[c];
		Form_pg_attribute keyatt = TupleDescAttr(RelationGetDescr(index), c);
		AttrNumber	attNums[4];
		Oid			sortOperators[4];
		Oid			sortCollations[4];
		bool		nullsFirstFlags[4];
		int			nsortkeys = 0;

		col->state = cs;
		col->multikey = cs->multikey;

		col->sorttupdesc = CreateTemplateTupleDesc(4);
		TupleDescInitEntry(col->sorttupdesc, 1, "kind", INT4OID, -1, 0);
		TupleDescInitEntry(col->sorttupdesc, 2, "hash", INT8OID, -1, 0);
		TupleDescInitEntry(col->sorttupdesc, 3, "code", INT8OID, -1, 0);
		TupleDescInitEntry(col->sorttupdesc, 4, "key", keyatt->atttypid,
						   keyatt->atttypmod, 0);
		TupleDescInitEntryCollation(col->sorttupdesc, 4, cs->collation);
		TupleDescFinalize(col->sorttupdesc);

		if (cs->ordered)
		{
			attNums[nsortkeys] = 4; /* key, NULLS FIRST: the reserved kinds */
			sortOperators[nsortkeys] = cs->ltopr;
			sortCollations[nsortkeys] = cs->collation;
			nullsFirstFlags[nsortkeys++] = true;
		}
		else
		{
			attNums[nsortkeys] = 2; /* hash: the reserved kinds hash to 0 */
			sortOperators[nsortkeys] = Int8LessOperator;
			sortCollations[nsortkeys] = InvalidOid;
			nullsFirstFlags[nsortkeys++] = false;
		}
		if (cs->multikey || !cs->ordered)
		{
			/*
			 * `kind` separates the reserved entries from each other and from
			 * the real keys that share their hash.  A multi-key class needs
			 * it because it has an EMPTY entry as well as a NULL one and
			 * neither has a key.  An UNORDERED class needs it because its
			 * leading sort key is the hash: the reserved entries hash to
			 * LION_NULLKEY_HASH, and a real key that hashes there too would
			 * otherwise interleave with them, code by code, and the grouping
			 * pass - which starts a new entry whenever the kind changes -
			 * would write several entries for one reserved kind.  It goes
			 * AFTER the hash, which is the same sequence: the reserved
			 * entries' hash is the minimum, so they still come first.
			 */
			attNums[nsortkeys] = 1;
			sortOperators[nsortkeys] = Int4LessOperator;
			sortCollations[nsortkeys] = InvalidOid;
			nullsFirstFlags[nsortkeys++] = false;
		}
		attNums[nsortkeys] = 3;		/* code */
		sortOperators[nsortkeys] = Int8LessOperator;
		sortCollations[nsortkeys] = InvalidOid;
		nullsFirstFlags[nsortkeys++] = false;

		col->sortstate = tuplesort_begin_heap(col->sorttupdesc, nsortkeys,
											  attNums, sortOperators,
											  sortCollations, nullsFirstFlags,
											  sortmem, NULL, TUPLESORT_NONE);

		col->inslot = MakeSingleTupleTableSlot(col->sorttupdesc,
											   &TTSOpsVirtual);
		col->outslot = MakeSingleTupleTableSlot(col->sorttupdesc,
												&TTSOpsMinimalTuple);
	}

	reltuples = table_index_build_scan(heap, index, indexInfo, true, true,
									   lion_build_callback, (void *) &bs, NULL);

	for (c = 0; c < ncols; c++)
		tuplesort_performsort(bs.cols[c].sortstate);

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
	lion_build_init_pages(&bs);
	bs.leaf = lion_build_level(&bs, 0);

	/*
	 * Column by column, in attno order: the directory order leads with the
	 * key column (DESIGN.md §24), so the columns' entry runs laid end to end
	 * are already sorted and the level builder needs no notion of columns.
	 */
	for (c = 0; c < ncols; c++)
		lion_build_write_entries(&bs, &bs.cols[c]);

	lion_build_finish_dir(&bs);

	Assert(BlockNumberIsValid(bs.root));
	metabuf = smgr_bulk_get_buf(bs.bulk);
	lion_init_metapage((Page) metabuf->data, bs.inline_limit, bs.root,
					  bs.height, (uint32) bs.ndirpages,
					  lion_wal_mode_for_build(index));
	smgr_bulk_write(bs.bulk, LION_METAPAGE_BLKNO, metabuf, true);

	smgr_bulk_finish(bs.bulk);

	elog(DEBUG1, "lion index \"%s\": %d key columns, " INT64_FORMAT " entries, "
		 "%u blocks, " INT64_FORMAT " directory pages, height %u, fillfactor %d",
		 RelationGetRelationName(index), ncols, bs.ndistinct, bs.nblocks,
		 bs.ndirpages, bs.height, bs.fillfactor);

	/*
	 * Cardinality guard (DESIGN.md §17).  At build time the count is exact -
	 * the pass above grouped the keys - so the warning is too.  It is only a
	 * warning: the index is built either way.
	 */
	if (bs.max_entries > 0 && bs.ndistinct > (int64) bs.max_entries)
		lion_warn_max_entries(index, bs.ndistinct);

	for (c = 0; c < ncols; c++)
	{
		tuplesort_end(bs.cols[c].sortstate);
		ExecDropSingleTupleTableSlot(bs.cols[c].inslot);
		ExecDropSingleTupleTableSlot(bs.cols[c].outslot);
		FreeTupleDesc(bs.cols[c].sorttupdesc);
	}
	MemoryContextDelete(bs.buildctx);

	result = (IndexBuildResult *) palloc0(sizeof(IndexBuildResult));
	result->heap_tuples = reltuples;
	result->index_tuples = bs.indtuples;

	return result;
}
