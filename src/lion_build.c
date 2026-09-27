/*-------------------------------------------------------------------------
 *
 * lion_build.c
 *		ambuild for the lion index (DESIGN.md section 5, BUILD).
 *
 * The heap is scanned once into a SPOOL (lion_spool.c), which appends each
 * row's code to its key's entry in memory - the scan delivers TIDs in
 * ascending order, so a posting set needs no sorting - and spills sorted runs
 * when maintenance_work_mem is used up.  The spool then hands the entries
 * back in the DIRECTORY order of DESIGN.md §21 - (kind, key, hash) for an
 * ordered opclass, (kind, hash, stored bytes) for one whose key type has no
 * btree opclass - each with its codes in ascending order, and one pass groups
 * the codes of each key into containers and writes the posting sets out,
 * either inline in the entry tuple or as a chain of container pages, in
 * exactly the order the directory wants them in.
 *
 * A PARALLEL build (amcanbuildparallel, PostgreSQL 17 and later) is nbtree's
 * shape: the workers and the leader share one parallel heap scan, each fills
 * a spool of its own and exports it as one tape, and the leader merges the
 * tapes and does the same single pass as a serial build.  The merge orders
 * every key's codes whichever participant read them, so the index is the one
 * a serial build writes, page for page.
 *
 * The directory itself is built bottom-up in that same pass, nbtree's
 * _bt_buildadd shape: one open page per level, filled to `fillfactor` percent
 * and written out when the next item does not fit, at which point its high
 * key is that item's key and its downlink goes to the level above.  The level
 * whose last page is also its first is the root.
 *
 * A multi-key opclass (DESIGN.md §17) turns one heap row into one code for
 * each distinct key it extracts; nothing else in the build changes.
 *
 * A MULTICOLUMN index (DESIGN.md §24) is ONE heap scan feeding one
 * accumulator PER KEY COLUMN, and the directory is then built from them in
 * column order: the directory order leads with the column number, so the
 * entries of column 1 followed by the entries of column 2 are already the
 * order the leaves want, and the bottom-up level builder never has to know
 * that more than one column exists.  The columns share maintenance_work_mem
 * as they need it (lion_spool.c).
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
#include "access/parallel.h"
#include "access/table.h"
#include "access/tableam.h"
#include "access/xact.h"
#include "catalog/index.h"
#include "catalog/pg_operator_d.h"
#include "catalog/pg_type_d.h"
#include "executor/instrument.h"
#include "miscadmin.h"
#include "nodes/execnodes.h"
#include "pgstat.h"
#include "storage/bufmgr.h"
#include "storage/bufpage.h"
#include "storage/buffile.h"
#include "storage/condition_variable.h"
#include "storage/spin.h"
#include "tcop/tcopprot.h"
#include "utils/datum.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "utils/tuplesort.h"
#include "utils/wait_event.h"
#include "varatt.h"

#include "lion.h"
#include "lion_spool.h"

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

	/*
	 * Summary posting sets (DESIGN.md §31).  sumflags are the kind flags the
	 * entry is written with (LION_ENTRY_SUMMARY, plus LION_ENTRY_SUMLAST for a
	 * column's last bucket).  tofile makes a COLLECTING builder: it groups
	 * codes into items exactly as every builder does, and writes each item to
	 * the file instead of to an entry or a page - the items of a summary wait
	 * there until the column's VALUE entries are all written, which is where
	 * the summaries go in the directory order.
	 */
	uint16		sumflags;
	BufFile    *tofile;
} LionBuilder;

/*
 * The summaries of the key column being written (DESIGN.md §31).  The build
 * sees a column's keys in directory order with each key's codes, so it cuts
 * them into BUCKETS as they pass: the codes of consecutive keys are collected
 * until they reach summary_tids, and the bucket is then closed at the key
 * boundary - sorted, grouped into items and put aside in `file`.  The last
 * bucket of the column, whatever its size, becomes the column's SUMLAST
 * entry.  Once the column's last key has been written the buckets are written
 * after it, as entries, which is where the directory order puts them.
 *
 * AUTO decides only then, from the column's exact counts, whether to keep
 * them; until then nothing has been written to the index, so dropping them
 * costs nothing but the file.
 */
typedef struct LionSumBuild
{
	int			mode;			/* LION_SUMOPT_ON or LION_SUMOPT_AUTO */
	uint32		bucket_tids;	/* the summary_tids reloption */
	MemoryContext cxt;			/* lives for the column */
	MemoryContext bucketcxt;	/* reset after each bucket */

	/* the bucket being collected */
	uint64	   *codes;
	int64		ncodes;
	int64		capcodes;
	int64		maxcodes;		/* past this the codes go to `sort` */
	Tuplesortstate *sort;
	bool		sorted;			/* codes[] is ascending as it stands */
	uint64		lastcode;
	char	   *lastraw;		/* stored bytes of its largest key so far */
	Size		lastrawlen;
	Size		lastrawcap;
	int64		bucketkeys;

	/* the column so far */
	int64		nkeys;
	double		ntids;
	int64		nbuckets;
	BufFile    *file;
} LionSumBuild;

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

struct LionBuildLeader;

typedef struct LionBuildState
{
	Relation	index;
	LionIndexState ix;			/* every key column's state (DESIGN.md §24) */
	uint32		inline_limit;
	int			fillfactor;
	Size		leafbudget;		/* bytes of a page ambuild fills */

	Size		dirbudget;		/* ... and of an internal one (§21) */

	int			max_entries;	/* cardinality guard, 0 = unlimited */

	LionState  *cur;			/* the column being written, or NULL */

	/*
	 * The input: a serial build's own spool, or in a parallel build the
	 * leader's reader over the participants' tapes (lion_spool.c).
	 */
	LionSpool  *spool;
	LionSpoolReader *reader;
	struct LionBuildLeader *leader; /* parallel builds only */

	MemoryContext buildctx;		/* lives for the whole build */
	MemoryContext tmpctx;		/* reset per key group */

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

	/* Summary posting sets (DESIGN.md §31). */
	int			sumopt;			/* the `summaries` reloption, LION_SUMOPT_* */
	uint32		sumtids;		/* the `summary_tids` reloption */
	LionSumBuild *sum;			/* the column being written's, or NULL */
	uint32		summary_cols;	/* the columns that got them */
	int64		nsummaries;		/* summary entries written */
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
				  (src->flags & (LION_ENTRY_KINDFLAGS | LION_ENTRY_MINUSINF)) :
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
	/* the keys, for the §17 guard; a summary (§31) is not one */
	if (!LionEntryIsSummary(entry))
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
	LionState  *cs = bs->cur;

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

	/* A collecting summary builder puts its items aside (DESIGN.md §31). */
	if (b->tofile != NULL)
	{
		uint32		len = (uint32) csize;

		BufFileWrite(b->tofile, &len, sizeof(len));
		BufFileWrite(b->tofile, c, csize);
		return;
	}

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
			lion_make_reserved_entry((AttrNumber) bs->cur->attno,
									lion_reserved_flag(b->keykind),
									LION_ENTRY_CHAIN, NULL, 0, &size) :
			lion_make_entry(bs->cur, b->key, b->hash, LION_ENTRY_CHAIN,
						   NULL, 0, &size);
		entry->head = b->head;
		entry->tail = b->curblk;
	}
	else
	{
		entry = (b->keykind != LION_KEY_REAL) ?
			lion_make_reserved_entry((AttrNumber) bs->cur->attno,
									lion_reserved_flag(b->keykind),
									LION_ENTRY_INLINE, b->inlinebuf,
									b->inlineused, &size) :
			lion_make_entry(bs->cur, b->key, b->hash, LION_ENTRY_INLINE,
						   b->inlinebuf, b->inlineused, &size);
	}

	entry->ncontainers = b->nitems;
	entry->ntids = b->ntids;
	entry->flags |= b->sumflags;

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
	LionState  *cs = bs->cur;
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
 * The sort is what a hash collision needs (DESIGN.md §21): writing the
 * builders of one hash out in any other order would put the leaf items out of
 * order, which breaks the binary search and the key-based resume of
 * lion_entry_scan_next().  The spool hands the distinct keys of a colliding
 * hash over in directory order already, so this changes nothing today; it is
 * kept because the page layout depends on it and it costs nothing.  There are
 * as many builders as there are distinct keys of one hash, which is one
 * unless the hash function collides.
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
 * Summary posting sets (DESIGN.md §31)
 * --------------------------------------------------------------------- */

/*
 * The mark that ends a bucket's items in the file, where an item's length
 * would be; no item is empty.
 */
#define LION_SUM_END_OF_ITEMS	0

/* The header of a bucket in the file: its key and what its entry needs. */
typedef struct LionSumBucketHdr
{
	uint16		sumflags;
	uint32		rawlen;
	uint64		ntids;
} LionSumBucketHdr;

/*
 * Does the key column being written get summaries?  Only an ordered scalar
 * column can: a summary is the union of a RUN of keys, which needs an order,
 * and a multi-key column's entries are extracted keys, not column values.
 */
static LionSumBuild *
lion_sum_begin(LionBuildState *bs, LionState *col)
{
	LionSumBuild *sum;

	if (bs->sumopt == LION_SUMOPT_OFF || !col->ordered || col->multikey)
		return NULL;

	sum = (LionSumBuild *) MemoryContextAllocZero(bs->buildctx,
												  sizeof(LionSumBuild));
	sum->mode = bs->sumopt;
	sum->bucket_tids = bs->sumtids;
	sum->cxt = AllocSetContextCreate(bs->buildctx, "lion summary build",
									 ALLOCSET_DEFAULT_SIZES);
	sum->bucketcxt = AllocSetContextCreate(sum->cxt, "lion summary bucket",
										   ALLOCSET_DEFAULT_SIZES);
	sum->capcodes = 1024;
	sum->codes = (uint64 *) MemoryContextAlloc(sum->cxt,
											   sizeof(uint64) * sum->capcodes);

	/*
	 * A bucket is normally summary_tids codes and a key's worth more, which
	 * is a few hundred kilobytes.  One key can be far larger than that - a
	 * column of few keys with summaries = on - and past a quarter of
	 * maintenance_work_mem the rest of such a bucket is sorted by a tuplesort,
	 * which spills.
	 */
	sum->maxcodes = Max((int64) maintenance_work_mem * 1024L / 4 /
						(int64) sizeof(uint64), (int64) 8192);
	sum->sorted = true;
	sum->lastrawcap = 64;
	sum->lastraw = (char *) MemoryContextAlloc(sum->cxt, sum->lastrawcap);
	sum->file = BufFileCreateTemp(false);

	return sum;
}

/* One code of the bucket being collected. */
static void
lion_sum_add(LionSumBuild *sum, uint64 code)
{
	if (sum->sort != NULL)
	{
		tuplesort_putdatum(sum->sort, Int64GetDatum((int64) code), false);
		sum->ncodes++;
		return;
	}

	if (sum->ncodes > 0 && code <= sum->lastcode)
		sum->sorted = false;
	sum->lastcode = code;

	if (sum->ncodes >= sum->capcodes)
	{
		if (sum->ncodes >= sum->maxcodes)
		{
			MemoryContext old = MemoryContextSwitchTo(sum->bucketcxt);
			int64		i;

			sum->sort = tuplesort_begin_datum(INT8OID, Int8LessOperator,
											  InvalidOid, false,
											  maintenance_work_mem / 4, NULL,
											  TUPLESORT_NONE);
			MemoryContextSwitchTo(old);
			for (i = 0; i < sum->ncodes; i++)
				tuplesort_putdatum(sum->sort,
								   Int64GetDatum((int64) sum->codes[i]), false);
			tuplesort_putdatum(sum->sort, Int64GetDatum((int64) code), false);
			sum->ncodes++;
			return;
		}
		sum->capcodes = Min(sum->capcodes * 2, sum->maxcodes);
		sum->codes = (uint64 *) repalloc_huge(sum->codes,
											  sizeof(uint64) * sum->capcodes);
	}
	sum->codes[sum->ncodes++] = code;
}

static int
lion_sum_code_cmp(const void *a, const void *b)
{
	uint64		x = *(const uint64 *) a;
	uint64		y = *(const uint64 *) b;

	return (x < y) ? -1 : (x > y) ? 1 : 0;
}

/*
 * Close the bucket being collected: its codes in ascending order, grouped into
 * items by the very builder every posting set is written with, and put aside
 * in the file after a header naming the bucket's key.  The codes of one bucket
 * are the disjoint codes of distinct keys of one scalar column, so none of
 * them repeats; a repeat is skipped all the same, as the builder takes each
 * member once.
 */
static void
lion_sum_close_bucket(LionBuildState *bs, LionSumBuild *sum, bool last)
{
	MemoryContext old;
	LionBuilder *b;
	LionSumBucketHdr hdr;
	uint32		end = LION_SUM_END_OF_ITEMS;
	uint64		prev = 0;
	bool		any = false;

	if (sum->ncodes == 0)
		return;

	old = MemoryContextSwitchTo(sum->bucketcxt);

	memset(&hdr, 0, sizeof(hdr));
	hdr.sumflags = LION_ENTRY_SUMMARY | (last ? LION_ENTRY_SUMLAST : 0);
	hdr.rawlen = (uint32) sum->lastrawlen;
	hdr.ntids = (uint64) sum->ncodes;
	BufFileWrite(sum->file, &hdr, sizeof(hdr));
	BufFileWrite(sum->file, sum->lastraw, sum->lastrawlen);

	/* A collecting builder: no key, no entry, its items go to the file. */
	b = (LionBuilder *) palloc0(sizeof(LionBuilder));
	b->keykind = LION_KEY_REAL;
	b->cur = (LionContainer *) palloc(LION_CONTAINER_MAX_SIZE);
	b->cbuf = (LionContainer *) palloc(LION_CONTAINER_MAX_SIZE);
	b->seg = (LionContainer *) palloc(LION_CONTAINER_MAX_SIZE);
	lion_sparse_init(b->seg, 0);
	b->tofile = sum->file;

	if (sum->sort != NULL)
	{
		Datum		val;
		bool		isnull;

		tuplesort_performsort(sum->sort);
		while (tuplesort_getdatum(sum->sort, true, false, &val, &isnull, NULL))
		{
			uint64		code = (uint64) DatumGetInt64(val);

			if (any && code == prev)
				continue;
			lion_builder_add(bs, b, code);
			prev = code;
			any = true;
		}
		tuplesort_end(sum->sort);
		sum->sort = NULL;
	}
	else
	{
		int64		i;

		/*
		 * A column stored in the order of its keys - a timestamp that rows
		 * arrive in - hands its buckets over already sorted.
		 */
		if (!sum->sorted)
			qsort(sum->codes, (size_t) sum->ncodes, sizeof(uint64),
				  lion_sum_code_cmp);
		for (i = 0; i < sum->ncodes; i++)
		{
			if (any && sum->codes[i] == prev)
				continue;
			lion_builder_add(bs, b, sum->codes[i]);
			prev = sum->codes[i];
			any = true;
		}
	}
	lion_builder_finish_group(bs, b);
	lion_builder_close_segment(bs, b);
	BufFileWrite(sum->file, &end, sizeof(end));

	sum->nbuckets++;
	sum->ncodes = 0;
	sum->sorted = true;
	sum->bucketkeys = 0;

	MemoryContextSwitchTo(old);
	MemoryContextReset(sum->bucketcxt);
}

/*
 * The key whose codes lion_sum_add() was just handed has been written: it is
 * now the largest key of the bucket, and the bucket closes once it holds
 * summary_tids codes - at a key boundary, since a key's rows are never split
 * between two buckets.
 */
static void
lion_sum_key_done(LionBuildState *bs, LionSumBuild *sum, const char *raw,
				  Size rawlen)
{
	if (rawlen > sum->lastrawcap)
	{
		sum->lastrawcap = Max(rawlen, sum->lastrawcap * 2);
		sum->lastraw = (char *) repalloc(sum->lastraw, sum->lastrawcap);
	}
	memcpy(sum->lastraw, raw, rawlen);
	sum->lastrawlen = rawlen;
	sum->bucketkeys++;
	sum->nkeys++;

	if (sum->ncodes >= (int64) sum->bucket_tids)
		lion_sum_close_bucket(bs, sum, false);
}

/*
 * The column's VALUE entries are all written.  The last bucket becomes the
 * column's SUMLAST entry, AUTO decides from the column's exact counts whether
 * to keep any of it, and the buckets are written after the column's values as
 * entries - INLINE or a posting tree of their own, by the same builders and
 * the same rules as every entry.
 */
static void
lion_sum_finish(LionBuildState *bs, LionSumBuild *sum)
{
	LionState  *cs = bs->cur;
	bool		keep;

	lion_sum_close_bucket(bs, sum, true);

	/*
	 * ON summarizes the column even with no rows yet: its first insert opens
	 * the first bucket.  AUTO needs rows to decide from, and a column it
	 * leaves without summaries has none until the next REINDEX.
	 */
	if (sum->mode == LION_SUMOPT_ON)
		keep = true;
	else
		keep = sum->ntids >= (double) LION_SUMMARY_AUTO_MIN_BUCKETS *
			sum->bucket_tids &&
			sum->ntids <= (double) sum->nkeys *
			((double) sum->bucket_tids / LION_SUMMARY_AUTO_MIN_KEYS);

	if (keep)
	{
		LionSumBucketHdr hdr;
		LionContainer *item = (LionContainer *)
			MemoryContextAlloc(sum->cxt, LION_CONTAINER_MAX_SIZE);
		char	   *raw = NULL;
		Size		rawcap = 0;
		int64		i;

		if (BufFileSeek(sum->file, 0, 0, SEEK_SET) != 0)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not rewind lion summary temporary file")));

		for (i = 0; i < sum->nbuckets; i++)
		{
			MemoryContext old;
			LionBuilder *b;
			Datum		key;
			uint32		len;

			BufFileReadExact(sum->file, &hdr, sizeof(hdr));
			if (hdr.rawlen > rawcap)
			{
				rawcap = Max((Size) hdr.rawlen, (Size) 64);
				raw = (char *) MemoryContextAlloc(sum->cxt, rawcap);
			}
			BufFileReadExact(sum->file, raw, hdr.rawlen);

			old = MemoryContextSwitchTo(bs->tmpctx);
			key = lion_fetch_key(cs, raw);

			Assert(bs->nbuilders == 0);
			b = lion_builder_create(bs, key, LION_KEY_REAL,
									LION_SUMMARY_HASH);
			bs->nbuilders = 0;	/* written here, not by lion_flush_builders() */
			b->sumflags = hdr.sumflags;

			for (;;)
			{
				BufFileReadExact(sum->file, &len, sizeof(len));
				if (len == LION_SUM_END_OF_ITEMS)
					break;
				if (len < LION_CONTAINER_HDRSZ || len > LION_CONTAINER_MAX_SIZE)
					elog(ERROR, "lion index build: a summary item of %u bytes",
						 len);
				BufFileReadExact(sum->file, item, len);
				lion_builder_emit(bs, b, item);
				b->nitems++;
			}
			b->ntids = hdr.ntids;
			lion_builder_flush(bs, b);
			bs->nsummaries++;

			MemoryContextSwitchTo(old);
			MemoryContextReset(bs->tmpctx);
		}

		bs->summary_cols |= ((uint32) 1) << (cs->attno - 1);
	}

	elog(DEBUG1, "lion index \"%s\": key column %d, " INT64_FORMAT " keys, %.0f TIDs, " INT64_FORMAT " summary buckets of %u TIDs, %s",
		 RelationGetRelationName(bs->index), cs->attno, sum->nkeys, sum->ntids,
		 sum->nbuckets, sum->bucket_tids, keep ? "kept" : "dropped");

	BufFileClose(sum->file);
	MemoryContextDelete(sum->cxt);
	pfree(sum);
}

/* ---------------------------------------------------------------------
 * The build itself
 * --------------------------------------------------------------------- */

/*
 * table_index_build_scan() callback, serial and parallel alike: arg is this
 * process's spool, which does everything (lion_spool.c).
 */
static void
lion_build_callback(Relation index, ItemPointer tid, Datum *values,
				   bool *isnull, bool tupleIsAlive, void *arg)
{
	lion_spool_add((LionSpool *) arg, tid, values, isnull);
}

/*
 * The one pass over the spool's output: each group it hands over is the
 * entries of one directory position - one key, or every key of a colliding
 * hash under an unordered opclass - with their codes in ascending order,
 * which is exactly what the builders take.  The groups come in the directory
 * order of DESIGN.md §21, so the entries are written left to right.
 */
static void
lion_build_emit(void *arg, LionSpoolGroup *group)
{
	LionBuildState *bs = (LionBuildState *) arg;
	int			n = lion_spool_group_size(group);
	MemoryContext oldctx = MemoryContextSwitchTo(bs->tmpctx);
	LionBuilder *one;
	LionBuilder **b = (n == 1) ? &one : palloc(sizeof(LionBuilder *) * n);
	uint64		ncodes = 0;
	uint64		code;
	int			which;
	int			i;
	bool		sumkey;

	Assert(bs->nbuilders == 0);
	for (i = 0; i < n; i++)
	{
		const LionSpoolEntry *e = lion_spool_group_entry(group, i);

		b[i] = lion_builder_create(bs, e->key, e->kind, e->hash);
	}

	/*
	 * A summarized column's values go into its current summary bucket as well
	 * (DESIGN.md §31); its reserved entries are in no bucket.
	 */
	sumkey = (bs->sum != NULL &&
			  lion_spool_group_entry(group, 0)->kind == LION_KIND_VALUE);

	while (lion_spool_group_next(group, &which, &code))
	{
		lion_builder_add(bs, b[which], code);
		if (sumkey)
		{
			lion_sum_add(bs->sum, code);
			bs->sum->ntids++;
		}
		if ((++ncodes & 0xFFFF) == 0)
			CHECK_FOR_INTERRUPTS();
	}

	lion_flush_builders(bs);

	if (sumkey)
	{
		/*
		 * The largest key of the group, which the directory order puts last;
		 * a group of an ordered column is one key unless its comparison ties
		 * keys its equality does not, and then they all sort as one.
		 */
		const LionSpoolEntry *e = lion_spool_group_entry(group, n - 1);

		lion_sum_key_done(bs, bs->sum, e->raw, e->rawlen);
	}
	MemoryContextSwitchTo(oldctx);
	MemoryContextReset(bs->tmpctx);
}

/*
 * Every key column's state, before the meta page exists: built from the
 * options directly, and deciding the order of each column from the catalog
 * (DESIGN.md §21), which every participant of a parallel build does the same
 * way.  Returns the inline limit.
 */
static uint32
lion_build_index_state(Relation index, LionIndexState *ix, MemoryContext cxt)
{
	LionOptions *opts = (LionOptions *) index->rd_options;
	LionMetaPageData meta;
	uint32		inline_limit;

	inline_limit = opts ? (uint32) opts->inline_limit : LION_DEFAULT_INLINE_LIMIT;

	/* The root is filled in once the tree has been built. */
	memset(&meta, 0, sizeof(meta));
	meta.magic = LION_MAGIC;
	meta.version = LION_VERSION;
	meta.offset_bits = LION_OFFSET_BITS;
	meta.container_bits = LION_CONTAINER_BITS;
	meta.inline_limit = inline_limit;
	meta.root = InvalidBlockNumber;
	lion_fill_index_state(index, ix, &meta, cxt);

	return inline_limit;
}

/* ---------------------------------------------------------------------
 * Parallel build (PostgreSQL 17 and later: amcanbuildparallel)
 *
 * nbtree's and GIN's shape.  The leader sets up a parallel heap scan, a
 * shared fileset and room for one TapeShare per participant, launches the
 * workers and takes part itself; each participant scans its share of the
 * heap into a spool of its own, with maintenance_work_mem divided between
 * them, and exports it as one tape (lion_spool_export()).  Once all of them
 * are done, the leader merges the tapes a column at a time and writes the
 * index exactly as a serial build does, with one bulk writer - the write pass
 * stays serial, as nbtree's does.
 *
 * The scan is told not to synchronize (see lion_spool.c): the heap AM's
 * parallel scan decides that when it is initialized, from the table's size,
 * and nothing reads the decision until the first participant starts.
 * --------------------------------------------------------------------- */

#if PG_VERSION_NUM >= 170000

#define PARALLEL_KEY_LION_SHARED	UINT64CONST(0xC100000000000001)
#define PARALLEL_KEY_LION_TAPES		UINT64CONST(0xC100000000000002)
#define PARALLEL_KEY_QUERY_TEXT		UINT64CONST(0xC100000000000003)
#define PARALLEL_KEY_WAL_USAGE		UINT64CONST(0xC100000000000004)
#define PARALLEL_KEY_BUFFER_USAGE	UINT64CONST(0xC100000000000005)

typedef struct LionBuildShared
{
	/* Set by the leader before any participant starts. */
	Oid			heaprelid;
	Oid			indexrelid;
	bool		isconcurrent;
	int			nrequested;		/* workers asked for, plus the leader */
	int			leaderfile;		/* the leader's file number */
	uint64		queryid;
	SharedFileSet fileset;

	/* Participants report here when their tape is ready. */
	ConditionVariable workersdonecv;
	slock_t		mutex;
	int			nparticipantsdone;
	double		reltuples;
	double		indtuples;
	bool		brokenhotchain;

	/*
	 * ParallelTableScanDescData follows, BUFFERALIGNed as shm_toc_allocate()
	 * aligns: a table AM's scan descriptor may need that.
	 */
} LionBuildShared;

#define ParallelTableScanFromLionBuildShared(shared) \
	((ParallelTableScanDesc) ((char *) (shared) + BUFFERALIGN(sizeof(LionBuildShared))))

typedef struct LionBuildLeader
{
	ParallelContext *pcxt;
	int			nparticipants;	/* workers launched, plus the leader */
	LionBuildShared *shared;
	TapeShare  *tapes;			/* by file number: workers', then leader's */
	Snapshot	snapshot;
	WalUsage   *walusage;
	BufferUsage *bufferusage;
} LionBuildLeader;

extern PGDLLEXPORT void lion_parallel_build_main(dsm_segment *seg,
												 shm_toc *toc);

/*
 * One participant's part, the leader's included: scan its share of the heap
 * into a spool, export the spool to the tape of its file number, and report.
 */
static void
lion_parallel_scan_and_spool(LionBuildShared *shared, TapeShare *tapes,
							 Relation heap, Relation index, int filenum,
							 int memkb, bool progress)
{
	MemoryContext cxt = AllocSetContextCreate(CurrentMemoryContext,
											  "lion parallel build",
											  ALLOCSET_DEFAULT_SIZES);
	MemoryContext oldctx = MemoryContextSwitchTo(cxt);
	LionIndexState ix;
	LionSpool  *spool;
	IndexInfo  *indexInfo;
	TableScanDesc scan;
	double		reltuples;

	(void) lion_build_index_state(index, &ix, cxt);
	spool = lion_spool_begin(&ix, (Size) memkb * 1024, &shared->fileset,
							 filenum);

	indexInfo = BuildIndexInfo(index);
	indexInfo->ii_Concurrent = shared->isconcurrent;
	scan = lion_table_beginscan_parallel(heap,
										 ParallelTableScanFromLionBuildShared(shared));
	reltuples = table_index_build_scan(heap, index, indexInfo, true, progress,
									   lion_build_callback, spool, scan);

	lion_spool_export(spool, &tapes[filenum]);

	SpinLockAcquire(&shared->mutex);
	shared->nparticipantsdone++;
	shared->reltuples += reltuples;
	shared->indtuples += lion_spool_ntids(spool);
	if (indexInfo->ii_BrokenHotChain)
		shared->brokenhotchain = true;
	SpinLockRelease(&shared->mutex);
	ConditionVariableSignal(&shared->workersdonecv);

	lion_spool_end(spool);
	MemoryContextSwitchTo(oldctx);
	MemoryContextDelete(cxt);
}

static void
lion_end_parallel(LionBuildLeader *leader)
{
	int			i;

	WaitForParallelWorkersToFinish(leader->pcxt);
	for (i = 0; i < leader->pcxt->nworkers_launched; i++)
		InstrAccumParallelQuery(&leader->bufferusage[i], &leader->walusage[i]);
	if (LION_IS_MVCC_LIKE(leader->snapshot))
		UnregisterSnapshot(leader->snapshot);
	DestroyParallelContext(leader->pcxt);
	ExitParallelMode();
}

/*
 * Launch the workers and take part in the scan.  bs->leader is set only when
 * at least one worker was launched; otherwise everything is undone and the
 * caller builds serially.
 */
static void
lion_begin_parallel(LionBuildState *bs, Relation heap, Relation index,
					bool isconcurrent, int request)
{
	LionBuildLeader *leader = palloc0(sizeof(LionBuildLeader));
	ParallelContext *pcxt;
	Snapshot	snapshot;
	Size		estshared;
	Size		esttapes;
	LionBuildShared *shared;
	TapeShare  *tapes;
	int			querylen = 0;

	EnterParallelMode();
	Assert(request > 0);
	pcxt = CreateParallelContext("pg_lion", "lion_parallel_build_main",
								 request);

	/*
	 * A normal build reads with SnapshotAny and decides what to index itself
	 * (RECENTLY_DEAD rows included); a concurrent one indexes what an MVCC
	 * snapshot sees.
	 */
	snapshot = isconcurrent ? RegisterSnapshot(GetTransactionSnapshot()) :
		SnapshotAny;

	estshared = add_size(BUFFERALIGN(sizeof(LionBuildShared)),
						 table_parallelscan_estimate(heap, snapshot));
	esttapes = mul_size(sizeof(TapeShare), request + 1);
	shm_toc_estimate_chunk(&pcxt->estimator, estshared);
	shm_toc_estimate_chunk(&pcxt->estimator, esttapes);
	shm_toc_estimate_keys(&pcxt->estimator, 2);
	shm_toc_estimate_chunk(&pcxt->estimator,
						   mul_size(sizeof(WalUsage), pcxt->nworkers));
	shm_toc_estimate_chunk(&pcxt->estimator,
						   mul_size(sizeof(BufferUsage), pcxt->nworkers));
	shm_toc_estimate_keys(&pcxt->estimator, 2);
	if (debug_query_string)
	{
		querylen = strlen(debug_query_string);
		shm_toc_estimate_chunk(&pcxt->estimator, querylen + 1);
		shm_toc_estimate_keys(&pcxt->estimator, 1);
	}

	InitializeParallelDSM(pcxt);
	if (pcxt->seg == NULL)
	{
		if (LION_IS_MVCC_LIKE(snapshot))
			UnregisterSnapshot(snapshot);
		DestroyParallelContext(pcxt);
		ExitParallelMode();
		return;
	}

	shared = (LionBuildShared *) shm_toc_allocate(pcxt->toc, estshared);
	shared->heaprelid = RelationGetRelid(heap);
	shared->indexrelid = RelationGetRelid(index);
	shared->isconcurrent = isconcurrent;
	shared->nrequested = request + 1;
	shared->leaderfile = request;
	shared->queryid = pgstat_get_my_query_id();
	SharedFileSetInit(&shared->fileset, pcxt->seg);
	ConditionVariableInit(&shared->workersdonecv);
	SpinLockInit(&shared->mutex);
	shared->nparticipantsdone = 0;
	shared->reltuples = 0.0;
	shared->indtuples = 0.0;
	shared->brokenhotchain = false;
	table_parallelscan_initialize(heap,
								  ParallelTableScanFromLionBuildShared(shared),
								  snapshot);
	ParallelTableScanFromLionBuildShared(shared)->phs_syncscan = false;

	tapes = (TapeShare *) shm_toc_allocate(pcxt->toc, esttapes);
	memset(tapes, 0, esttapes);

	shm_toc_insert(pcxt->toc, PARALLEL_KEY_LION_SHARED, shared);
	shm_toc_insert(pcxt->toc, PARALLEL_KEY_LION_TAPES, tapes);
	if (debug_query_string)
	{
		char	   *sharedquery = shm_toc_allocate(pcxt->toc, querylen + 1);

		memcpy(sharedquery, debug_query_string, querylen + 1);
		shm_toc_insert(pcxt->toc, PARALLEL_KEY_QUERY_TEXT, sharedquery);
	}
	leader->walusage = shm_toc_allocate(pcxt->toc,
										mul_size(sizeof(WalUsage), pcxt->nworkers));
	shm_toc_insert(pcxt->toc, PARALLEL_KEY_WAL_USAGE, leader->walusage);
	leader->bufferusage = shm_toc_allocate(pcxt->toc,
										   mul_size(sizeof(BufferUsage), pcxt->nworkers));
	shm_toc_insert(pcxt->toc, PARALLEL_KEY_BUFFER_USAGE, leader->bufferusage);

	LaunchParallelWorkers(pcxt);
	leader->pcxt = pcxt;
	leader->nparticipants = pcxt->nworkers_launched + 1;
	leader->shared = shared;
	leader->tapes = tapes;
	leader->snapshot = snapshot;

	if (pcxt->nworkers_launched == 0)
	{
		lion_end_parallel(leader);
		return;
	}
	bs->leader = leader;

	/* The leader takes part as a worker does, on the file after theirs. */
	lion_parallel_scan_and_spool(shared, tapes, heap, index,
								 shared->leaderfile,
								 maintenance_work_mem / leader->nparticipants,
								 true);

	/* Make sure a worker that failed to start cannot leave us waiting. */
	WaitForParallelWorkersToAttach(pcxt);
}

/*
 * Wait for every participant's tape, and open the leader's reader over them.
 * Returns the number of heap tuples the scan saw.
 */
static double
lion_parallel_heapscan(LionBuildState *bs, bool *brokenhotchain)
{
	LionBuildLeader *leader = bs->leader;
	LionBuildShared *shared = leader->shared;
	double		reltuples;
	int		   *filenums;
	TapeShare  *shares;
	int			i;

	for (;;)
	{
		SpinLockAcquire(&shared->mutex);
		if (shared->nparticipantsdone == leader->nparticipants)
		{
			reltuples = shared->reltuples;
			bs->indtuples = shared->indtuples;
			if (shared->brokenhotchain)
				*brokenhotchain = true;
			SpinLockRelease(&shared->mutex);
			break;
		}
		SpinLockRelease(&shared->mutex);
		ConditionVariableSleep(&shared->workersdonecv,
							   WAIT_EVENT_PARALLEL_CREATE_INDEX_SCAN);
	}
	ConditionVariableCancelSleep();

	/* the workers' files are numbered from 0, the leader's comes after */
	filenums = palloc(sizeof(int) * leader->nparticipants);
	shares = palloc(sizeof(TapeShare) * leader->nparticipants);
	for (i = 0; i < leader->nparticipants; i++)
	{
		filenums[i] = (i < leader->pcxt->nworkers_launched) ? i :
			shared->leaderfile;
		shares[i] = leader->tapes[filenums[i]];
	}
	bs->reader = lion_spool_reader_begin(&bs->ix, &shared->fileset,
										 leader->nparticipants, filenums,
										 shares,
										 (Size) maintenance_work_mem * 1024);
	return reltuples;
}

/*
 * A parallel worker (PostgreSQL's ParallelWorkerMain() calls it by name).
 */
void
lion_parallel_build_main(dsm_segment *seg, shm_toc *toc)
{
	LionBuildShared *shared;
	TapeShare  *tapes;
	Relation	heap;
	Relation	index;
	LOCKMODE	heaplock;
	LOCKMODE	indexlock;
	WalUsage   *walusage;
	BufferUsage *bufferusage;

	debug_query_string = shm_toc_lookup(toc, PARALLEL_KEY_QUERY_TEXT, true);
	pgstat_report_activity(STATE_RUNNING, debug_query_string);

	shared = shm_toc_lookup(toc, PARALLEL_KEY_LION_SHARED, false);
	tapes = shm_toc_lookup(toc, PARALLEL_KEY_LION_TAPES, false);

	/* the lock modes index.c took for the leader */
	if (!shared->isconcurrent)
	{
		heaplock = ShareLock;
		indexlock = AccessExclusiveLock;
	}
	else
	{
		heaplock = ShareUpdateExclusiveLock;
		indexlock = RowExclusiveLock;
	}

	pgstat_report_query_id(shared->queryid, false);
	heap = table_open(shared->heaprelid, heaplock);
	index = index_open(shared->indexrelid, indexlock);

	SharedFileSetAttach(&shared->fileset, seg);
	InstrStartParallelQuery();

	lion_parallel_scan_and_spool(shared, tapes, heap, index,
								 ParallelWorkerNumber,
								 maintenance_work_mem / shared->nrequested,
								 false);

	bufferusage = shm_toc_lookup(toc, PARALLEL_KEY_BUFFER_USAGE, false);
	walusage = shm_toc_lookup(toc, PARALLEL_KEY_WAL_USAGE, false);
	InstrEndParallelQuery(&bufferusage[ParallelWorkerNumber],
						  &walusage[ParallelWorkerNumber]);

	index_close(index, indexlock);
	table_close(heap, heaplock);
}

#endif							/* PG_VERSION_NUM >= 170000 */

IndexBuildResult *
lionbuild(Relation heap, Relation index, IndexInfo *indexInfo)
{
	IndexBuildResult *result;
	LionBuildState bs;
	LionOptions *opts = (LionOptions *) index->rd_options;
	double		reltuples;
	BulkWriteBuffer metabuf;
	int			ncols;
	int			c;

	/*
	 * Before anything else, and whether or not the table has rows: an index
	 * that happens to build on an empty table of another table AM would fail
	 * at the first insert instead, or count wrongly (lion_am.c).
	 */
	lion_check_table_am(heap);

	if (RelationGetNumberOfBlocks(index) != 0)
		elog(ERROR, "index \"%s\" already contains data",
			 RelationGetRelationName(index));

	memset(&bs, 0, sizeof(bs));
	bs.index = index;
	bs.indtuples = 0;
	bs.fillfactor = opts ? opts->fillfactor : LION_DEFAULT_FILLFACTOR;
	bs.max_entries = lion_max_entries(index);
	bs.root = InvalidBlockNumber;
	bs.height = 0;
	bs.sumopt = opts ? opts->summaries : LION_SUMOPT_OFF;
	bs.sumtids = opts ? (uint32) opts->summary_tids : LION_DEFAULT_SUMMARY_TIDS;
	bs.sum = NULL;
	bs.summary_cols = 0;
	bs.nsummaries = 0;

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
	/*
	 * A key's builders take four buffers of up to a container each; a first
	 * block that holds them all keeps the reset after every key from handing
	 * blocks back to malloc and asking for them again.
	 */
	bs.tmpctx = AllocSetContextCreate(bs.buildctx,
									  "lion index build temporary",
									  0, 64 * 1024, ALLOCSET_DEFAULT_MAXSIZE);

	bs.inline_limit = lion_build_index_state(index, &bs.ix, bs.buildctx);
	ncols = bs.ix.ncolumns;

	bs.maxbuilders = 8;
	bs.builders = (LionBuilder **) MemoryContextAlloc(bs.buildctx,
													 sizeof(LionBuilder *) * bs.maxbuilders);
	bs.nbuilders = 0;
	bs.cur = NULL;

	/*
	 * The scan.  Core asks for workers only where the AM can take them
	 * (amcanbuildparallel, 17 and later) and plan_create_index_workers()
	 * allows them: max_parallel_maintenance_workers, the table's
	 * parallel_workers, and 32MB of maintenance_work_mem per participant.
	 */
#if PG_VERSION_NUM >= 170000
	if (indexInfo->ii_ParallelWorkers > 0)
		lion_begin_parallel(&bs, heap, index, indexInfo->ii_Concurrent,
							indexInfo->ii_ParallelWorkers);
	if (bs.leader != NULL)
		reltuples = lion_parallel_heapscan(&bs, &indexInfo->ii_BrokenHotChain);
	else
#endif
	{
		bs.spool = lion_spool_begin(&bs.ix, (Size) maintenance_work_mem * 1024,
									NULL, -1);

		/*
		 * No synchronized scan: TIDs in ascending order is what lets the spool
		 * append instead of sort (lion_spool.c), which is also why GIN's
		 * serial build asks for none.
		 */
		reltuples = table_index_build_scan(heap, index, indexInfo, false, true,
										   lion_build_callback, bs.spool, NULL);
		bs.indtuples = lion_spool_ntids(bs.spool);
	}

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
	{
		bs.cur = &bs.ix.cols[c];

		/*
		 * A column's summaries (DESIGN.md §31) are collected while its values
		 * are written and written after them, which is where they sort.
		 */
		bs.sum = lion_sum_begin(&bs, bs.cur);
		if (bs.reader != NULL)
			lion_spool_reader_emit_column(bs.reader, c, lion_build_emit, &bs);
		else
			lion_spool_emit_column(bs.spool, c, lion_build_emit, &bs);
		if (bs.sum != NULL)
		{
			lion_sum_finish(&bs, bs.sum);
			bs.sum = NULL;
		}
	}
	bs.cur = NULL;

	lion_build_finish_dir(&bs);

	Assert(BlockNumberIsValid(bs.root));
	metabuf = smgr_bulk_get_buf(bs.bulk);
	lion_init_metapage((Page) metabuf->data, bs.inline_limit, bs.root,
					  bs.height, (uint32) bs.ndirpages,
					  lion_wal_mode_for_build(index));
	/* ... and the order the directory was just laid out in (§21). */
	lion_meta_record_order(LionPageGetMeta((Page) metabuf->data), &bs.ix);
	/* ... and which columns got summaries, which makes it version 7 (§31). */
	lion_meta_record_summaries(LionPageGetMeta((Page) metabuf->data),
							   bs.summary_cols, bs.sumtids);
	smgr_bulk_write(bs.bulk, LION_METAPAGE_BLKNO, metabuf, true);

	smgr_bulk_finish(bs.bulk);

	elog(DEBUG1, "lion index \"%s\": %d key columns, " INT64_FORMAT " entries, "
		 "%u blocks, " INT64_FORMAT " directory pages, height %u, fillfactor %d, %s",
		 RelationGetRelationName(index), ncols, bs.ndistinct, bs.nblocks,
		 bs.ndirpages, bs.height, bs.fillfactor,
		 bs.spool == NULL ? "built in parallel" :
		 lion_spool_nruns(bs.spool) == 0 ? "built in memory" :
		 "built with spilled runs");
	if (bs.spool != NULL)
		elog(DEBUG2, "lion index \"%s\": %d runs spilled",
			 RelationGetRelationName(index), lion_spool_nruns(bs.spool));

	/*
	 * Cardinality guard (DESIGN.md §17).  At build time the count is exact -
	 * the pass above grouped the keys - so the warning is too.  It is only a
	 * warning: the index is built either way.
	 */
	if (bs.max_entries > 0 && bs.ndistinct > (int64) bs.max_entries)
		lion_warn_max_entries(index, bs.ndistinct);

	if (bs.spool != NULL)
		lion_spool_end(bs.spool);
#if PG_VERSION_NUM >= 170000
	if (bs.reader != NULL)
		lion_spool_reader_end(bs.reader);
	if (bs.leader != NULL)
		lion_end_parallel(bs.leader);
#endif
	MemoryContextDelete(bs.buildctx);

	result = (IndexBuildResult *) palloc0(sizeof(IndexBuildResult));
	result->heap_tuples = reltuples;
	result->index_tuples = bs.indtuples;

	return result;
}
