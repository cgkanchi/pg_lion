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
 * A PARALLEL build (amcanbuildparallel, PostgreSQL 17 and later) scans as
 * nbtree's does: the workers and the leader share one parallel heap scan,
 * each fills a spool of its own and exports it as a tape per key column.  The
 * write is then split by column, which share no keys: the workers write the
 * columns after the first, each to a file of its own, while the leader
 * writes the first, and the leader then writes each further column in turn,
 * by replaying a worker's file or by itself.  The merge orders every key's
 * codes whichever participant read them, and the replay gives every block
 * the number a serial build gives it, so the index is the one a serial build
 * writes, page for page.
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
 *
 * The writer keeps the Relation, not its SMgrRelation, and asks
 * RelationGetSmgr() for the handle at every write, as 16's nbtsort.c does:
 * the handle does not survive a relcache flush, and an sinval reset during
 * the write phase - which any lock taken meanwhile can bring, a spilling
 * tuplesort's PrepareTempTablespaces() for one - closes every handle there is
 * (RelationCacheInvalidate() calls smgrcloseall()).  From 17 on a handle
 * stays valid to the end of the transaction, which is what lets 17's own bulk
 * writer keep one.
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
	Relation	rel;			/* RelationGetSmgr() at every use: see above */
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

	bw->rel = rel;
	bw->forknum = forknum;
	bw->locator = rel->rd_locator;
	bw->use_wal = RelationNeedsWAL(rel) || forknum == INIT_FORKNUM;
	bw->need_sync = !RelationUsesLocalBuffers(rel);
	bw->pages_written = smgrnblocks(RelationGetSmgr(rel), forknum);
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
			smgrextend(RelationGetSmgr(bw->rel), bw->forknum,
					   bw->pages_written++, bw->zeropage->data, true);
		}

		PageSetChecksumInplace(page, blkno);

		if (blkno == bw->pages_written)
		{
			smgrextend(RelationGetSmgr(bw->rel), bw->forknum, blkno, page,
					   true);
			bw->pages_written++;
		}
		else
			smgrwrite(RelationGetSmgr(bw->rel), bw->forknum, blkno, page,
					  true);

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
		smgrimmedsync(RelationGetSmgr(bw->rel), bw->forknum);
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
	uint32		hash;			/* the key's hash, for the order (§21) */
	uint32		owner;			/* the owner stamp of its pages (§18) */
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
	uint64		seglast;		/* the code of its last pair */

	char	   *inlinebuf;		/* buffered items, packed without padding */
	Size		inlineused;
	Size		inlinemax;		/* lion_inline_max() for this key */

	bool		spilled;		/* posting set lives on container pages */
	BlockNumber head;			/* the set's head block (DESIGN.md §22) */
	BlockNumber curblk;			/* block pagebuf will be written to */
	BulkWriteBuffer pagebuf;	/* container page under construction */
	bool		haspage;

	/*
	 * The posting tree above the leaves (DESIGN.md §22).  A set of at most
	 * LION_POSTING_FLAT_LEAVES leaves has none at all: `head` is its first
	 * leaf and the leaves are one chain.  Their finished images wait in
	 * `held` until the set is known to stay that small; the moment one leaf
	 * more is needed, a block is reserved for the ROOT, the held leaves are
	 * re-stamped with it and written, and the internal levels are filled
	 * bottom-up.
	 */
	BulkWriteBuffer held[LION_POSTING_FLAT_LEAVES];
	BlockNumber heldblk[LION_POSTING_FLAT_LEAVES];
	int			nheld;
	bool		hastree;
	BlockNumber rootblk;
	LionPostLevel *plevel;		/* the level just above the leaves */
	uint32		height;

	uint32		nitems;			/* containers and segments (entry.ncontainers) */
	uint64		ntids;

	/*
	 * Summary posting sets (DESIGN.md §32).  sumflags are the kind flags the
	 * entry is written with (LION_ENTRY_SUMMARY, plus LION_ENTRY_SUMLAST for a
	 * column's last bucket).  tofile makes a COLLECTING builder: it groups
	 * codes into items exactly as every builder does, and writes each item to
	 * the file instead of to an entry or a page - the items of a summary wait
	 * there until the column's VALUE entries are all written, which is where
	 * the summaries go in the directory order.
	 */
	uint16		sumflags;
	BufFile    *tofile;
	/* a key of a column that stores positions (DESIGN.md §17) */
	bool		haspos;
	LionContainer *poschunk;	/* its positions, when they fit one chunk */
	BlockNumber posroot;		/* else the position tree written for them */
	uint64		posmembers;		/* the rows they cover */
} LionBuilder;

/*
 * The summaries of the key column being written (DESIGN.md §32).  The build
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
	int			sharekb;		/* LION_SUM_SHARE_KB() of the writer's memory */
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
	int			posting_fillfactor;
	Size		postbudget;		/* ... of a posting-tree leaf (§22) */

	/*
	 * Write INLINE entries and posting items with an insert's growth slack
	 * (the `build_slack` reloption, DESIGN.md §4), and the buffer an item is
	 * zero-padded in for it.
	 */
	bool		slack;
	LionContainer *slackbuf;	/* LION_CONTAINER_MAX_SIZE bytes */

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
	int			memkb;			/* what writing a column may use, in kB */

	/*
	 * Page writing (see the file header): one bulk writer for the whole
	 * build, blocks handed out by a counter instead of by extending the
	 * relation, and the bucket pages kept in memory until pass 2 is over.
	 *
	 * A parallel worker that writes a column has no bulk writer and no
	 * directory: its pages and its entries go to colout instead, numbered by
	 * its own counter, for the leader to renumber and write (see "Columns
	 * written by the workers" below).
	 */
	BulkWriteState *bulk;
	BufFile    *colout;
	BlockNumber nblocks;		/* blocks handed out so far */
	LionBuildLevel *leaf;		/* the bottom of the directory */
	int64		ndirpages;
	int64		ndistinct;		/* entries written, for the §17 guard */
	uint64	   *nvalues;		/* VALUE entries of each key column, and */
	uint64		rows;			/* ... the TIDs of the first scalar one's
								 * entries: the key counts of §33 */
	AttrNumber	rowcol;			/* that column (lion_index_row_column()) */
	BlockNumber root;
	uint32		height;

	LionBuilder **builders;		/* open builders of the current hash */
	int			nbuilders;
	int			maxbuilders;

	double		indtuples;		/* rows pushed into the index */

	/* Summary posting sets (DESIGN.md §32). */
	int			sumopt;			/* the `summaries` reloption, LION_SUMOPT_* */
	uint32		sumtids;		/* the `summary_tids` reloption */
	LionSumBuild *sum;			/* the column being written's, or NULL */
	uint32		summary_cols;	/* the columns that got them */
	int64		nsummaries;		/* summary entries written */

	/* A NARROW item written, which lets VACUUM make NARROWs too (§38). */
	bool		wrote_narrow;
	/* the positions of the columns that store them (lion_posbuild.c) */
	LionPosBuild *posbuild;
	bool		curpos;			/* ... for the column being written */
	LionPageWriter pw;
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

/* What a worker writing a column writes (see "Columns written by ..."). */
#define LION_COLREC_PAGE	1
#define LION_COLREC_ENTRY	2
#define LION_COLREC_END		3

static void lion_build_colout_page(LionBuildState *bs, BlockNumber blk,
								   BulkWriteBuffer buf);

static BulkWriteBuffer
lion_build_get_page(LionBuildState *bs, uint16 flags)
{
	BulkWriteBuffer buf;

	if (bs->colout != NULL)
		buf = (BulkWriteBuffer) MemoryContextAllocAligned(bs->buildctx, BLCKSZ,
														  PG_IO_ALIGN_SIZE, 0);
	else
		buf = smgr_bulk_get_buf(bs->bulk);
	lion_init_page((Page) buf->data, flags);

	return buf;
}

/* A finished page of a posting or position tree, which buf now belongs to. */
static void
lion_build_put_page(LionBuildState *bs, BlockNumber blk, BulkWriteBuffer buf)
{
	if (bs->colout != NULL)
		lion_build_colout_page(bs, blk, buf);
	else
		smgr_bulk_write(bs->bulk, blk, buf, true);
}

/* The page writer lion_posbuild.c writes position trees through. */
static BlockNumber
lion_build_pw_alloc(void *arg)
{
	return lion_build_alloc_block((LionBuildState *) arg);
}

static void *
lion_build_pw_get(void *arg, Page *page, uint16 flags)
{
	BulkWriteBuffer buf = lion_build_get_page((LionBuildState *) arg, flags);

	*page = (Page) buf->data;
	return buf;
}

static void
lion_build_pw_put(void *arg, BlockNumber blk, void *handle)
{
	lion_build_put_page((LionBuildState *) arg, blk, (BulkWriteBuffer) handle);
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

/* What a pivot copy of this item's key costs on a page. */
static inline Size
lion_build_pivot_need(const LionEntryTuple *item)
{
	return MAXALIGN(lion_pivot_size(item)) + sizeof(ItemIdData);
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
		pivot = lion_make_pivot(hksrc, LION_ENTRY_HIGHKEY, InvalidBlockNumber,
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
		pivot = lion_make_pivot(lv->npages == 0 ? NULL :
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
 * Does a directory leaf take the item that crosses its fill budget?  The
 * leaf's fill lands on whichever side of the budget is nearer: the item is
 * added when the page would be over the budget by less than it is short of
 * it without the item - its high key counted either way, as the item's own
 * key stands for the next page's first - and the page still holds it.  An
 * item small against the budget crosses it by less than half its own size,
 * so the fill target keeps its meaning; a large INLINE entry, of which
 * a page holds a handful, used to leave most of an entry's room empty on
 * every leaf - four entries of 1.55 kB, 76% full, where a fifth makes 95% -
 * and now fills the page past the budget instead when that is nearer
 * (DESIGN.md §21).  The internal levels keep their own rule
 * (lion_build_level_add()).
 */
static inline bool
lion_build_leaf_crosses(const LionBuildLevel *lv, Size need, Size hkneed,
						Size budget)
{
	double		over;
	double		short_;

	if (lv->level != 0 || lv->used + need + hkneed > LION_PAGE_CAPACITY)
		return false;
	over = (double) (lv->used + need + hkneed) - (double) budget;
	short_ = (double) budget - (double) (lv->used + hkneed);
	return over < short_;
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
			lv->used + need + hkneed > LION_PAGE_CAPACITY) &&
		   !lion_build_leaf_crosses(lv, need, hkneed, budget))
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

static void lion_build_colout_record(LionBuildState *bs, uint32 kind,
									 uint32 arg, const void *data, Size len);

/*
 * One entry tuple, in directory order; a worker writing a column hands it to
 * the leader instead (see "Columns written by the workers").
 */
static void
lion_build_add_entry(LionBuildState *bs, LionEntryTuple *entry, Size size)
{
	if (bs->colout != NULL)
	{
		lion_build_colout_record(bs, LION_COLREC_ENTRY, (uint32) size, entry,
								 size);
		return;
	}

	lion_build_level_add(bs, bs->leaf, entry, size);
	/* the keys, for the §17 guard; a summary (§32) is not one */
	if (!LionEntryIsSummary(entry))
		bs->ndistinct++;

	/*
	 * ... and each column's values and the rows, for the planner (DESIGN.md
	 * §33): exact here, where every entry passes once.
	 */
	if (lion_entry_kind(entry) == LION_KIND_VALUE)
		bs->nvalues[entry->attno - 1]++;
	if (entry->attno == bs->rowcol && !LionEntryIsSummary(entry))
		bs->rows += lion_entry_ntids(entry);
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
 * Columns written by the workers (DESIGN.md §24, "Parallel build")
 *
 * A worker writes a column with the builders a serial build uses, into a file
 * instead of the index: it numbers blocks by a counter of its own from 0,
 * each finished page goes to the file with its number, and so does each
 * entry, which a serial build would add to the directory there and then.
 * Every record says how many blocks the worker had handed out by then.
 *
 * The leader replays the file when the column's turn comes.  A serial build
 * hands out a column's posting and position tree blocks from its one
 * counter, interleaved with the directory pages the column's entries open as
 * they are added; so at each record the leader first gives the worker's
 * blocks handed out since the last one the next numbers of its own counter,
 * and only then adds the entry, which opens directory pages just where a
 * serial build opens them.  Every block gets the number a serial build gives
 * it, and once the block references in the pages and entries are renumbered
 * the pages are a serial build's too.
 *
 * There are few references to renumber, because a worker writes only the
 * pages of posting and position trees (DESIGN.md §22, §17): a page's
 * rightlink, its owner stamp (the tree's root) and an internal page's
 * downlinks, and an entry's head, tail and position tree root.
 * --------------------------------------------------------------------- */

typedef struct LionColRecord
{
	uint32		kind;			/* LION_COLREC_* */
	BlockNumber nblocks;		/* blocks the worker had handed out */
	uint32		arg;			/* PAGE: its block; ENTRY: its size */
} LionColRecord;

static void
lion_build_colout_record(LionBuildState *bs, uint32 kind, uint32 arg,
						 const void *data, Size len)
{
	LionColRecord rec;

	rec.kind = kind;
	rec.nblocks = bs->nblocks;
	rec.arg = arg;
	BufFileWrite(bs->colout, &rec, sizeof(rec));
	if (len > 0)
		BufFileWrite(bs->colout, data, len);
}

static void
lion_build_colout_page(LionBuildState *bs, BlockNumber blk,
					   BulkWriteBuffer buf)
{
	lion_build_colout_record(bs, LION_COLREC_PAGE, blk, buf->data, BLCKSZ);
	pfree(buf);
}

#if PG_VERSION_NUM >= 170000

/*
 * The leader's numbers for a worker's blocks: runs of consecutive blocks,
 * each starting at `from` of the worker's and `to` of the index's, broken
 * wherever the directory took blocks in between.
 */
typedef struct LionBlockRun
{
	BlockNumber from;
	BlockNumber to;
} LionBlockRun;

typedef struct LionBlockMap
{
	LionBlockRun *runs;
	int			nruns;
	int			maxruns;
	BlockNumber mapped;			/* the worker's blocks numbered so far */
} LionBlockMap;

/* Number the worker's blocks up to nblocks with the leader's next ones. */
static void
lion_blockmap_extend(LionBuildState *bs, LionBlockMap *map,
					 BlockNumber nblocks)
{
	LionBlockRun *last = (map->nruns > 0) ? &map->runs[map->nruns - 1] : NULL;

	if (nblocks < map->mapped)
		elog(ERROR, "lion index build: a column's record says %u blocks after %u",
			 nblocks, map->mapped);
	if (nblocks == map->mapped)
		return;

	if (last == NULL || last->to + (map->mapped - last->from) != bs->nblocks)
	{
		if (map->nruns >= map->maxruns)
		{
			map->maxruns = Max(64, map->maxruns * 2);
			map->runs = (map->runs == NULL) ?
				MemoryContextAlloc(bs->buildctx,
								   sizeof(LionBlockRun) * map->maxruns) :
				repalloc(map->runs, sizeof(LionBlockRun) * map->maxruns);
		}
		map->runs[map->nruns].from = map->mapped;
		map->runs[map->nruns].to = bs->nblocks;
		map->nruns++;
	}
	bs->nblocks += nblocks - map->mapped;
	map->mapped = nblocks;
}

static BlockNumber
lion_blockmap_get(const LionBlockMap *map, BlockNumber blk)
{
	int			lo = 0;
	int			hi = map->nruns - 1;

	if (!BlockNumberIsValid(blk))
		return blk;
	if (blk >= map->mapped)
		elog(ERROR, "lion index build: a column refers to block %u of %u",
			 blk, map->mapped);

	/* the last run that starts at or before blk; the first starts at 0 */
	while (lo < hi)
	{
		int			mid = lo + (hi - lo + 1) / 2;

		if (map->runs[mid].from <= blk)
			lo = mid;
		else
			hi = mid - 1;
	}
	return map->runs[lo].to + (blk - map->runs[lo].from);
}

static void
lion_build_renumber_page(Page page, const LionBlockMap *map)
{
	LionPageOpaque opaque;

	if (PageGetSpecialSize(page) != LION_SPECIAL_SIZE ||
		!LionPageIsContainer(page))
		elog(ERROR, "lion index build: a worker wrote a page that is not a container page");
	opaque = LionPageGetOpaque(page);
	opaque->rightlink = lion_blockmap_get(map, opaque->rightlink);
	opaque->leftlink = lion_blockmap_get(map, opaque->leftlink);
	opaque->owner_head = lion_blockmap_get(map, opaque->owner_head);

	/* an internal page of a posting or position tree: its downlinks */
	if (opaque->level > 0)
	{
		OffsetNumber maxoff = PageGetMaxOffsetNumber(page);
		OffsetNumber off;

		for (off = FirstOffsetNumber; off <= maxoff; off++)
		{
			LionPostingPivot *pivot = (LionPostingPivot *)
				PageGetItem(page, PageGetItemId(page, off));

			pivot->child = lion_blockmap_get(map, pivot->child);
		}
	}
}

static void
lion_build_renumber_entry(LionEntryTuple *e, const LionBlockMap *map)
{
	if (lion_entry_is_chain(e))
	{
		lion_entry_set_head(e, lion_blockmap_get(map, lion_entry_head(e)));
		lion_entry_set_tail(e, lion_blockmap_get(map, lion_entry_tail(e)));
	}
	if ((e->flags & LION_ENTRY_POSITIONS) != 0)
	{
		LionEntryPosExt *x = (LionEntryPosExt *)
			((char *) e + LionEntryExtOffset(e));

		x->pos_root = lion_blockmap_get(map, x->pos_root);
	}
}

/* Write a column a worker wrote to file, in the place a serial build would. */
static void
lion_build_replay_column(LionBuildState *bs, BufFile *file)
{
	LionBlockMap map;
	LionEntryTuple *entry = (LionEntryTuple *)
		MemoryContextAlloc(bs->buildctx, BLCKSZ);

	memset(&map, 0, sizeof(map));
	for (;;)
	{
		LionColRecord rec;

		CHECK_FOR_INTERRUPTS();
		BufFileReadExact(file, &rec, sizeof(rec));
		lion_blockmap_extend(bs, &map, rec.nblocks);

		if (rec.kind == LION_COLREC_ENTRY)
		{
			if (rec.arg < sizeof(LionEntryTuple) || rec.arg > BLCKSZ)
				elog(ERROR, "lion index build: a column's entry of %u bytes",
					 rec.arg);
			BufFileReadExact(file, entry, rec.arg);
			lion_build_renumber_entry(entry, &map);
			lion_build_add_entry(bs, entry, rec.arg);
		}
		else if (rec.kind == LION_COLREC_PAGE)
		{
			BulkWriteBuffer buf = smgr_bulk_get_buf(bs->bulk);

			BufFileReadExact(file, buf->data, BLCKSZ);
			lion_build_renumber_page((Page) buf->data, &map);
			smgr_bulk_write(bs->bulk, lion_blockmap_get(&map, rec.arg), buf,
							true);
		}
		else if (rec.kind == LION_COLREC_END)
			break;
		else
			elog(ERROR, "lion index build: a column's record of kind %u",
				 rec.kind);
	}

	pfree(entry);
	if (map.runs != NULL)
		pfree(map.runs);
}

#endif							/* PG_VERSION_NUM >= 170000 */

/* ---------------------------------------------------------------------
 * Posting set builders
 * --------------------------------------------------------------------- */

static LionBuilder *
lion_builder_create(LionBuildState *bs, Datum key, int keykind, uint32 hash,
					bool summary)
{
	LionBuilder *b = (LionBuilder *) palloc0(sizeof(LionBuilder));
	LionState  *cs = bs->cur;

	b->keykind = keykind;
	b->hash = hash;
	b->key = (keykind != LION_KEY_REAL) ? (Datum) 0 :
		datumCopy(key, cs->typbyval, cs->typlen);
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
	b->owner = lion_owner_stamp(summary ? LION_KIND_SUMMARY : keykind,
								b->rawkey, b->rawlen);
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
								   lion_entry_payoff_for(LION_ENTRY_INLINE |
														 lion_key_layout_flags(cs),
														 b->rawlen) :
								   lion_entry_payoff_for(LION_ENTRY_INLINE, 0),
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
	lion_page_set_owner(page, b->owner, b->head);

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

	lion_build_put_page(bs, thisblk, buf);

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
 * The set needs one leaf more than a set without an internal level may have
 * (LION_POSTING_FLAT_LEAVES), so it needs a ROOT above them.  Reserve its
 * block, re-stamp the held leaves, whose images have not been written yet,
 * and write them with their downlinks.
 */
static void
lion_builder_start_tree(LionBuildState *bs, LionBuilder *b)
{
	int			i;

	Assert(!b->hastree);

	b->rootblk = lion_build_alloc_block(bs);
	b->head = b->rootblk;
	b->hastree = true;
	b->plevel = lion_post_level(bs, 1);
	for (i = 0; i < b->nheld; i++)
	{
		Page		leaf = (Page) b->held[i]->data;

		lion_page_set_owner(leaf, b->owner, b->head);
		lion_post_level_add(bs, b, b->plevel, LionPageGetOpaque(leaf)->minckey,
							b->heldblk[i]);
		lion_build_put_page(bs, b->heldblk[i], b->held[i]);
	}
	b->nheld = 0;
}

/* Write the leaves of a set that stays without an internal level. */
static void
lion_builder_write_held(LionBuildState *bs, LionBuilder *b)
{
	int			i;

	for (i = 0; i < b->nheld; i++)
		lion_build_put_page(bs, b->heldblk[i], b->held[i]);
	b->nheld = 0;
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
 * Is the posting leaf under construction final, given the next item?  It is
 * when the item does not fit, and when taking it would fill the page past
 * `posting_fillfactor` (DESIGN.md §22, "Bulk build") - unless the page is
 * still empty: a leaf always takes its first item, which every item fits.
 * The budget counts what PageAddItem() spends, the MAXALIGNed item and its
 * line pointer, so at 100 the second test is the first one again.
 */
static bool
lion_builder_leaf_full(const LionBuildState *bs, Page img, Size csize)
{
	Size		used;

	if (PageGetFreeSpace(img) < MAXALIGN(csize))
		return true;
	if (PageGetMaxOffsetNumber(img) == 0)
		return false;

	used = LION_PAGE_CAPACITY - PageGetExactFreeSpace(img);
	return used + MAXALIGN(csize) + sizeof(ItemIdData) > bs->postbudget;
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
		lion_page_set_owner((Page) b->pagebuf->data, b->owner, b->head);
	}

	img = (Page) b->pagebuf->data;

	/*
	 * The growth slack an insert would give the item (DESIGN.md §4), when the
	 * build gives it: the item goes onto the page at that length, zero-padded
	 * in a buffer of the largest item size, and it is that length the leaf's
	 * fill counts.  A BITSET or a NARROW gets none (lion_item_alloc_size()).
	 */
	if (bs->slack)
	{
		Size		alloc = lion_item_alloc_size(c, csize);

		if (alloc > csize)
		{
			if (bs->slackbuf == NULL)
				bs->slackbuf = (LionContainer *)
					MemoryContextAlloc(bs->buildctx, LION_CONTAINER_MAX_SIZE);
			memcpy(bs->slackbuf, c, csize);
			lion_item_zero_slack(bs->slackbuf, csize, alloc);
			c = bs->slackbuf;
			csize = alloc;
		}
	}

	if (lion_builder_leaf_full(bs, img, csize))
	{
		BlockNumber next;

		next = lion_build_alloc_block(bs);
		lion_page_update_minmax(img);
		LionPageGetOpaque(img)->rightlink = next;

		/*
		 * The page is final.  Up to LION_POSTING_FLAT_LEAVES leaves make a
		 * set without an internal level, headed by its first leaf, and they
		 * are held back until that is certain; the leaf after that makes the
		 * set a TREE: the root block is reserved now and every held leaf is
		 * re-stamped with it before it goes out, because every page of a set
		 * carries its root block as the owner stamp (DESIGN.md §18, §22).
		 */
		if (!b->hastree && b->nheld + 2 <= LION_POSTING_FLAT_LEAVES)
		{
			b->held[b->nheld] = b->pagebuf;
			b->heldblk[b->nheld] = b->curblk;
			b->nheld++;
		}
		else
		{
			if (!b->hastree)
			{
				lion_builder_start_tree(bs, b);
				lion_page_set_owner(img, b->owner, b->head);
			}
			lion_post_level_add(bs, b, b->plevel,
								LionPageGetOpaque(img)->minckey, b->curblk);
			/* the page is final: hand the image itself to the bulk writer */
			lion_build_put_page(bs, b->curblk, b->pagebuf);
		}

		b->curblk = next;
		b->pagebuf = lion_build_get_page(bs, LION_PAGE_CONTAINER);
		img = (Page) b->pagebuf->data;
		lion_page_set_owner(img, b->owner, b->head);
	}

	if (PageAddItemExtended(img, c, csize, InvalidOffsetNumber, 0) ==
		InvalidOffsetNumber)
		elog(ERROR, "lion index: failed to add container to build page");
}

/* The posting set has outgrown the entry tuple: move it to pages. */
static void
lion_builder_to_pages(LionBuildState *bs, LionBuilder *b)
{
	Size		off = 0;
	Size		used = b->inlineused;

	Assert(!b->spilled);
	b->spilled = true;
	b->inlineused = 0;
	while (lion_inline_fetch(b->inlinebuf, used, &off, b->cbuf) > 0)
		lion_builder_spill(bs, b, b->cbuf);
}

static void
lion_builder_emit(LionBuildState *bs, LionBuilder *b, LionContainer *c)
{
	Size		csize = lion_item_size(c);

	/* every item the index gets comes this way, a summary's included */
	if (c->type == LION_CT_NARROW)
		bs->wrote_narrow = true;

	/* A collecting summary builder puts its items aside (DESIGN.md §32). */
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

		lion_builder_to_pages(bs, b);
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
 * Would the pending members of the group fit the open segment?  They are
 * appended above its last pair, each a varint of its code's delta from the
 * one before.
 */
static bool
lion_builder_group_fits(LionBuilder *b)
{
	uint64		prev;
	uint32		bytes = lion_sparse_nbytes(b->seg);
	int			i;

	if (b->seg->cardinality == 0)
		return true;			/* at most three pairs of one ckey always do */
	/* a group of more becomes a container (lion_builder_add()) */
	if (b->npend >= LION_SPARSE_THRESHOLD)
		elog(ERROR, "lion index: %d pending members of a sparse group",
			 b->npend);
	if ((uint32) b->seg->cardinality + (uint32) b->npend > LION_SPARSE_MAX_PAIRS)
		return false;

	prev = b->seglast;
	for (i = 0; i < b->npend; i++)
	{
		uint64		code = lion_make_code(b->curckey, b->pend[i]);

		bytes += lion_sparse_varint_size(code - prev);
		prev = code;
	}
	return bytes <= LION_SPARSE_MAX_BYTES;
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
		if (!lion_builder_group_fits(b))
			lion_builder_close_segment(bs, b);

		for (i = 0; i < b->npend; i++)
		{
			if (!lion_sparse_append(b->seg, &b->seglast, b->curckey,
									b->pend[i]))
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
 * entry, which the flush below has made, as an entry that stores positions
 * (DESIGN.md §17): the INLINE one with its chunk after the payload, the
 * CHAIN one pointing at its position tree - the one written for it, or a
 * one-leaf tree for positions that fit a chunk.
 */
static LionEntryTuple *
lion_builder_add_positions(LionBuildState *bs, LionBuilder *b,
						   LionEntryTuple *entry, Size *size)
{
	LionEntryTuple *ext;
	Size		extsz;

	if (b->posmembers != b->ntids)
		elog(ERROR, "lion index \"%s\": the build has positions for " UINT64_FORMAT " rows of a key it has " UINT64_FORMAT " for",
			 RelationGetRelationName(bs->index), b->posmembers, b->ntids);

	ext = lion_entry_add_posext(entry, &extsz);
	pfree(entry);
	if ((ext->flags & LION_ENTRY_INLINE) != 0)
	{
		LionEntryTuple *withpos;

		Assert(b->poschunk != NULL);
		withpos = lion_entry_rebuild_pos(ext, b->inlinebuf, b->inlineused,
										 b->inlineused,
										 (const char *) b->poschunk,
										 lion_poschunk_size(b->poschunk),
										 size);
		pfree(ext);
		return withpos;
	}

	lion_entry_posext(ext)->pos_root = BlockNumberIsValid(b->posroot) ?
		b->posroot : lion_posbuild_chunk_tree(&bs->pw, b->owner, b->poschunk);
	*size = extsz;
	return ext;
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

	/*
	 * A key's positions are known only once its codes are in (DESIGN.md
	 * §17), and what they leave of the entry with them: the posting set goes
	 * to pages if it does not fit beside them.
	 */
	if (!b->spilled && b->inlineused > b->inlinemax)
	{
		Assert(b->haspos);
		lion_builder_to_pages(bs, b);
	}

	if (b->spilled)
	{
		Page		img = (Page) b->pagebuf->data;

		Assert(b->haspage);
		lion_page_update_minmax(img);
		LionPageGetOpaque(img)->rightlink = InvalidBlockNumber;
		if (b->hastree)
			lion_post_level_add(bs, b, b->plevel,
								LionPageGetOpaque(img)->minckey, b->curblk);
		else
			lion_builder_write_held(bs, b);
		lion_build_put_page(bs, b->curblk, b->pagebuf);
		b->pagebuf = NULL;
		b->haspage = false;

		/* The internal levels, bottom-up; the root lands on b->head. */
		if (b->hastree)
			lion_builder_finish_tree(bs, b);

		entry = (b->keykind != LION_KEY_REAL) ?
			lion_make_reserved_entry((AttrNumber) bs->cur->attno,
									lion_reserved_flag(b->keykind),
									LION_ENTRY_CHAIN, NULL, 0, &size) :
			lion_make_entry(bs->cur, b->key, LION_ENTRY_CHAIN, NULL, 0, &size);
		lion_entry_set_head(entry, b->head);
		lion_entry_set_tail(entry, b->curblk);
	}
	else
	{
		entry = (b->keykind != LION_KEY_REAL) ?
			lion_make_reserved_entry((AttrNumber) bs->cur->attno,
									lion_reserved_flag(b->keykind),
									LION_ENTRY_INLINE, b->inlinebuf,
									b->inlineused, &size) :
			lion_make_entry(bs->cur, b->key, LION_ENTRY_INLINE,
							b->inlinebuf, b->inlineused, &size);
	}

	lion_entry_set_ncontainers(entry, b->nitems);
	lion_entry_set_ntids(entry, b->ntids);
	entry->flags |= b->sumflags;

	/*
	 * An INLINE payload's growth slack (DESIGN.md §4), when the build gives
	 * it: the zeroed bytes an insert's rewrite would have allotted the entry,
	 * by the same rule and the same caps (inline_limit, the largest entry),
	 * so that the first member an insert adds lands inside the entry and no
	 * other entry on the leaf moves.  An entry that stores positions keeps
	 * its exact size: its payload is followed by a positions chunk.
	 */
	if (bs->slack && !b->spilled && !b->haspos && b->inlineused > 0)
	{
		Size		payoff = LionEntryPayloadOffset(entry);
		Size		alloc;

		Assert(size == payoff + b->inlineused);
		alloc = lion_entry_alloc_size(payoff, b->inlineused,
									  (Size) bs->inline_limit,
									  (Size) LION_MAX_ENTRY_SIZE);
		if (alloc > size)
		{
			entry = (LionEntryTuple *) repalloc(entry, alloc);
			memset((char *) entry + size, 0, alloc - size);
			size = alloc;
		}
	}

	if (b->haspos)
		entry = lion_builder_add_positions(bs, b, entry, &size);

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
 * Summary posting sets (DESIGN.md §32)
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
 * What a column's summaries take of the memory of the process writing the
 * column - maintenance_work_mem, or its share in a parallel build - while its
 * values are written: an eighth for the codes of the bucket being collected
 * and, once a bucket outgrows that, as much for the tuplesort that takes them
 * over (lion_sum_add()).  The two meet only while the codes move from one to
 * the other, and the spool leaves the summaries both (lion_build_column()).
 * How the codes are held does not change what is written.
 */
#define LION_SUM_SHARE_KB(memkb)	Max((memkb) / 8, 64)
#define LION_SUM_MEMORY(memkb)	((Size) LION_SUM_SHARE_KB(memkb) * 1024 * 2)

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
	 * column of few keys with summaries = on - and past sharekb kilobytes of
	 * codes the rest of such a bucket is sorted by a tuplesort of as much,
	 * which spills, and the array goes (lion_sum_add()).
	 */
	sum->sharekb = LION_SUM_SHARE_KB(bs->memkb);
	sum->maxcodes = (int64) sum->sharekb * 1024 / (int64) sizeof(uint64);
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

	/* the first code after a bucket the tuplesort took (see below) */
	if (sum->codes == NULL)
	{
		sum->capcodes = 1024;
		sum->codes = (uint64 *) MemoryContextAlloc(sum->cxt,
												   sizeof(uint64) * sum->capcodes);
	}

	if (sum->ncodes >= sum->capcodes)
	{
		if (sum->ncodes >= sum->maxcodes)
		{
			MemoryContext old = MemoryContextSwitchTo(sum->bucketcxt);
			int64		i;

			sum->sort = tuplesort_begin_datum(INT8OID, Int8LessOperator,
											  InvalidOid, false,
											  sum->sharekb, NULL,
											  TUPLESORT_NONE);
			MemoryContextSwitchTo(old);
			for (i = 0; i < sum->ncodes; i++)
				tuplesort_putdatum(sum->sort,
								   Int64GetDatum((int64) sum->codes[i]), false);
			tuplesort_putdatum(sum->sort, Int64GetDatum((int64) code), false);
			sum->ncodes++;

			/*
			 * The array's share is the tuplesort's now: the array goes, and
			 * the bucket after this one starts a small one again.
			 */
			pfree(sum->codes);
			sum->codes = NULL;
			sum->capcodes = 0;
			return;
		}
		sum->capcodes = Min(sum->capcodes * 2, sum->maxcodes);
		sum->codes = (uint64 *) repalloc_huge(sum->codes,
											  sizeof(uint64) * sum->capcodes);
	}
	sum->codes[sum->ncodes++] = code;
}

/*
 * The sort of a bucket's array of codes, which is up to sharekb kilobytes
 * of them - millions at a large maintenance_work_mem - and so has to
 * answer a cancel: the template checks for interrupts as it partitions, where
 * qsort() with a comparison function did not check at all.
 */
#define ST_SORT lion_sum_sort_codes
#define ST_ELEMENT_TYPE uint64
#define ST_COMPARE(a, b) ((int) (*(a) > *(b)) - (int) (*(a) < *(b)))
#define ST_CHECK_FOR_INTERRUPTS
#define ST_SCOPE static
#define ST_DEFINE
#include "lib/sort_template.h"

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
	uint64		nseen = 0;

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

	/*
	 * A bucket is summary_tids codes and a key's worth more, and one key can
	 * be any number of rows (lion_sum_begin()): both loops below check for
	 * interrupts every 64K codes, as the build's loop over a key's codes does
	 * (lion_build_emit()), and the array's sort checks as it goes.  Nothing
	 * is locked here; the items go to the file.
	 */
	if (sum->sort != NULL)
	{
		Datum		val;
		bool		isnull;

		tuplesort_performsort(sum->sort);
		while (tuplesort_getdatum(sum->sort, true, false, &val, &isnull, NULL))
		{
			uint64		code = (uint64) DatumGetInt64(val);

			if ((++nseen & 0xFFFF) == 0)
				CHECK_FOR_INTERRUPTS();
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
			lion_sum_sort_codes(sum->codes, (size_t) sum->ncodes);
		for (i = 0; i < sum->ncodes; i++)
		{
			if ((++nseen & 0xFFFF) == 0)
				CHECK_FOR_INTERRUPTS();
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
 * now the largest key of the bucket.
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
}

/*
 * A key is about to hand its codes to the bucket: close the bucket first when
 * it already holds summary_tids codes - at a key boundary, since a key's rows
 * are never split between two buckets.  Closing here and not as soon as the
 * bucket fills is what leaves the column's LAST bucket open (DESIGN.md §32):
 * one that fills exactly at the column's last key is still the one
 * lion_sum_finish() makes the SUMLAST entry, so a column with rows always has
 * one.  (It used to be closed as a regular summary, leaving a column with no
 * open bucket, whose next insert above it then created one below whatever
 * pivot the closed one's leaf had.)
 */
static void
lion_sum_key_begin(LionBuildState *bs, LionSumBuild *sum)
{
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
		Size		rawcap = 64;
		char	   *raw = (char *) MemoryContextAlloc(sum->cxt, rawcap);
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
				/* as lion_sum_key_done() grows lastraw, not a new one a key */
				rawcap = Max((Size) hdr.rawlen, rawcap * 2);
				raw = (char *) repalloc(raw, rawcap);
			}
			BufFileReadExact(sum->file, raw, hdr.rawlen);

			old = MemoryContextSwitchTo(bs->tmpctx);
			key = lion_fetch_key(cs, raw);

			Assert(bs->nbuilders == 0);
			b = lion_builder_create(bs, key, LION_KEY_REAL,
									LION_SUMMARY_HASH, true);
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
	bool		haspos;

	/*
	 * A summarized column's values go into its current summary bucket as well
	 * (DESIGN.md §32); its reserved entries are in no bucket.  A full bucket
	 * is closed before this key's builders exist, as it used to be after the
	 * previous key's were flushed.
	 */
	sumkey = (bs->sum != NULL &&
			  lion_spool_group_entry(group, 0)->kind == LION_KIND_VALUE);
	if (sumkey)
		lion_sum_key_begin(bs, bs->sum);

	Assert(bs->nbuilders == 0);
	for (i = 0; i < n; i++)
	{
		const LionSpoolEntry *e = lion_spool_group_entry(group, i);

		b[i] = lion_builder_create(bs, e->key, e->kind, e->hash, false);
	}

	/*
	 * A key of a column that stores positions (DESIGN.md §17): its positions
	 * come with its codes, and go to lion_posbuild.c as they come.
	 */
	haspos = (bs->curpos &&
			  lion_spool_group_entry(group, 0)->kind == LION_KIND_VALUE);
	if (haspos)
	{
		if (n != 1)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("lion index \"%s\": keys that compare equal but differ cannot store positions",
							RelationGetRelationName(bs->index))));
		b[0]->haspos = true;
		lion_posbuild_key_begin(bs->posbuild, &bs->pw, b[0]->owner);
	}

	while (lion_spool_group_next(group, &which, &code))
	{
		lion_builder_add(bs, b[which], code);
		if (haspos)
		{
			LionKeyPositions kp;

			lion_spool_group_positions(group, &kp);
			lion_posbuild_key_add(bs->posbuild, code, &kp);
		}
		if (sumkey)
		{
			lion_sum_add(bs->sum, code);
			bs->sum->ntids++;
		}
		if ((++ncodes & 0xFFFF) == 0)
			CHECK_FOR_INTERRUPTS();
	}

	/*
	 * Positions that fit a chunk travel with the entry, and an INLINE posting
	 * set then has to leave them room (lion_builder_flush()); a tree means a
	 * CHAIN entry.
	 */
	if (haspos)
	{
		LionContainer *chunk;

		lion_posbuild_key_end(bs->posbuild, &chunk, &b[0]->posroot,
							  &b[0]->posmembers);
		if (chunk != NULL)
		{
			Size		poslen = lion_poschunk_size(chunk);
			Size		payoff =
				lion_entry_payoff_for(LION_ENTRY_INLINE | LION_ENTRY_POSITIONS |
									  lion_key_layout_flags(bs->cur),
									  b[0]->rawlen);
			Size		limit = lion_inline_max(payoff, (Size) bs->inline_limit);

			b[0]->poschunk = (LionContainer *) palloc(poslen);
			memcpy(b[0]->poschunk, chunk, poslen);
			b[0]->inlinemax = Min(b[0]->inlinemax,
								  limit > poslen ?
								  MAXALIGN_DOWN(limit - poslen) : 0);
		}
		else
			b[0]->inlinemax = 0;
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
	meta.version = LION_VERSION_V11;
	meta.offset_bits = LION_OFFSET_BITS;
	meta.container_bits = LION_CONTAINER_BITS;
	meta.inline_limit = inline_limit;
	meta.root = InvalidBlockNumber;
	if (opts == NULL || opts->store_positions != LION_POSOPT_OFF)
		meta.order_flags |= LION_META_POSITIONS;
	lion_fill_index_state(index, ix, &meta, cxt);
	lion_settle_store_positions(index, ix);

	return inline_limit;
}

/*
 * Everything a process writing columns needs, the leader of a parallel build
 * and its workers alike: the options, the budgets, the key columns' state,
 * the builders and the positions' builder.  memkb is what writing a column
 * may take.
 */
static void
lion_build_state_init(LionBuildState *bs, Relation index, int memkb)
{
	LionOptions *opts = (LionOptions *) index->rd_options;
	int			ncols;

	memset(bs, 0, sizeof(LionBuildState));
	bs->index = index;
	bs->indtuples = 0;
	bs->memkb = memkb;
	bs->fillfactor = opts ? opts->fillfactor : LION_DEFAULT_FILLFACTOR;
	bs->posting_fillfactor = opts ? opts->posting_fillfactor :
		LION_DEFAULT_POSTING_FILLFACTOR;
	switch (opts ? opts->build_slack : LION_DEFAULT_BUILD_SLACK)
	{
		case LION_SLACKOPT_ON:
			bs->slack = true;
			break;
		case LION_SLACKOPT_OFF:
			bs->slack = false;
			break;
		default:

			/*
			 * AUTO: where a moved item costs the most.  A generic-WAL record
			 * is a byte diff of the page, so the first insert into an item
			 * written at its exact size - which moves every item stored
			 * below it - logs all of them; a resource-manager record logs the
			 * item's own change whether or not others move (DESIGN.md §4).
			 * A parallel worker takes the leader's decision instead
			 * (lion_parallel_write_columns()).
			 */
			bs->slack = (lion_wal_mode_for_build(index) == LION_WAL_MODE_GENERIC);
			break;
	}
	bs->slackbuf = NULL;
	bs->max_entries = lion_max_entries(index);
	bs->root = InvalidBlockNumber;
	bs->height = 0;
	bs->sumopt = opts ? opts->summaries : LION_SUMOPT_OFF;
	bs->sumtids = opts ? (uint32) opts->summary_tids : LION_DEFAULT_SUMMARY_TIDS;
	bs->sum = NULL;
	bs->summary_cols = 0;
	bs->nsummaries = 0;

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
	bs->leafbudget = Max(LION_PAGE_CAPACITY * (Size) bs->fillfactor / 100, 1);
	bs->dirbudget = LION_PAGE_CAPACITY * (Size) LION_NONLEAF_FILLFACTOR / 100;

	/*
	 * And a posting-tree leaf (DESIGN.md §22, "Bulk build").  At 100 this is
	 * the whole page, and the test in lion_builder_spill() is then exactly
	 * "the next item does not fit".
	 */
	bs->postbudget = LION_PAGE_CAPACITY * (Size) bs->posting_fillfactor / 100;

	bs->buildctx = AllocSetContextCreate(CurrentMemoryContext,
										 "lion index build",
										 ALLOCSET_DEFAULT_SIZES);
	/*
	 * A key's builders take four buffers of up to a container each; a first
	 * block that holds them all keeps the reset after every key from handing
	 * blocks back to malloc and asking for them again.
	 */
	bs->tmpctx = AllocSetContextCreate(bs->buildctx,
									   "lion index build temporary",
									   0, 64 * 1024, ALLOCSET_DEFAULT_MAXSIZE);

	bs->inline_limit = lion_build_index_state(index, &bs->ix, bs->buildctx);
	ncols = bs->ix.ncolumns;
	bs->nvalues = (uint64 *) MemoryContextAllocZero(bs->buildctx,
													sizeof(uint64) * ncols);
	bs->rows = 0;
	bs->rowcol = lion_index_row_column(&bs->ix);

	bs->maxbuilders = 8;
	bs->builders = (LionBuilder **) MemoryContextAlloc(bs->buildctx,
													  sizeof(LionBuilder *) * bs->maxbuilders);
	bs->nbuilders = 0;
	bs->cur = NULL;

	/*
	 * A column that stores positions (DESIGN.md §17) keeps them in the spool
	 * with its codes, and the write hands them to lion_posbuild.c.
	 */
	bs->posbuild = lion_posbuild_begin(index, &bs->ix);
	bs->pw.arg = bs;
	bs->pw.alloc = lion_build_pw_alloc;
	bs->pw.get = lion_build_pw_get;
	bs->pw.put = lion_build_pw_put;
}

static void
lion_build_state_end(LionBuildState *bs)
{
	lion_posbuild_end(bs->posbuild);
	MemoryContextDelete(bs->buildctx);
}

/*
 * Write column c from what feeds it: the serial build's spool, or the reader
 * over the participants' tapes of it.
 */
static void
lion_build_column(LionBuildState *bs, int c)
{
	Size		sumreserve;

	bs->cur = &bs->ix.cols[c];

	/*
	 * A column's summaries (DESIGN.md §32) are collected while its values are
	 * written and written after them, which is where they sort.  What they
	 * take comes out of the spool's budget, not on top of it.
	 */
	bs->curpos = bs->cur->positions;
	bs->sum = lion_sum_begin(bs, bs->cur);
	sumreserve = (bs->sum != NULL) ? LION_SUM_MEMORY(bs->memkb) : 0;
	if (bs->reader != NULL)
		lion_spool_reader_emit(bs->reader, lion_build_emit, bs, sumreserve);
	else
		lion_spool_emit_column(bs->spool, c, lion_build_emit, bs, sumreserve);
	if (bs->sum != NULL)
	{
		lion_sum_finish(bs, bs->sum);
		bs->sum = NULL;
	}
	bs->curpos = false;
	bs->cur = NULL;
}

/* ---------------------------------------------------------------------
 * Parallel build (PostgreSQL 17 and later: amcanbuildparallel)
 *
 * nbtree's and GIN's shape for the scan.  The leader sets up a parallel heap
 * scan and a shared fileset, launches the workers and takes part itself; each
 * participant scans its share of the heap into a spool of its own, with
 * maintenance_work_mem divided between them, and exports one tape per key
 * column (lion_spool_export()).
 *
 * The write is split by column, since no two columns share a key: once every
 * participant's tapes are there, the workers take the columns one by one,
 * the largest left first, each merging the participants' tapes of it and
 * writing it to a file of its own ("Columns written by the workers", above).
 * The leader writes each column in turn: itself, straight into the index,
 * when no worker has taken it, or else by replaying the worker's file once
 * it is done.  The directory, the meta page and the one bulk writer stay the
 * leader's, and the index is the one a serial build writes, page for page.
 *
 * Replaying a column costs the leader its directory and its page writes,
 * which a worker cannot take off it, so two columns are the leader's own: the
 * first, which it can start on at once, and a column at least as large as
 * all the others together, whose replay would cost more than the workers
 * save on the rest (lion_parallel_own_column()).
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
#define PARALLEL_KEY_LION_COLUMNS	UINT64CONST(0xC100000000000006)

typedef struct LionBuildShared
{
	/* Set by the leader before any participant starts. */
	Oid			heaprelid;
	Oid			indexrelid;
	bool		isconcurrent;
	int			nrequested;		/* workers asked for, plus the leader */
	int			leaderfile;		/* the leader's file number */
	int			ncols;			/* key columns */
	bool		slack;			/* the build's growth slack (DESIGN.md §4) */
	uint64		queryid;
	SharedFileSet fileset;

	/* Participants report here when their tapes are ready. */
	ConditionVariable workersdonecv;
	slock_t		mutex;
	int			nparticipants;	/* workers launched plus the leader, or 0
								 * until the leader knows */
	int			nparticipantsdone;
	double		reltuples;
	double		indtuples;
	bool		brokenhotchain;

	/* The write (LionBuildColumn says which column is whose). */
	ConditionVariable coldonecv;	/* a worker finished a column */

	/*
	 * ParallelTableScanDescData follows, BUFFERALIGNed as shm_toc_allocate()
	 * aligns: a table AM's scan descriptor may need that.
	 */
} LionBuildShared;

#define ParallelTableScanFromLionBuildShared(shared) \
	((ParallelTableScanDesc) ((char *) (shared) + BUFFERALIGN(sizeof(LionBuildShared))))

/*
 * A column of the write, under the mutex: its size, whether someone has taken
 * it, and of one a worker took, what its writing set that the meta page says.
 */
typedef struct LionBuildColumn
{
	int64		blocks;			/* the participants' tapes of it, together */
	bool		taken;
	bool		done;			/* the worker's file is complete */
	bool		wrote_narrow;
	bool		summarized;
	int64		nsummaries;
} LionBuildColumn;

typedef struct LionBuildLeader
{
	ParallelContext *pcxt;
	int			nparticipants;	/* workers launched, plus the leader */
	LionBuildShared *shared;
	TapeShare  *tapes;			/* participant p's tape of column c at
								 * p * ncols + c, by file number */
	LionBuildColumn *columns;
	int			owncol;			/* lion_parallel_own_column() */
	int			nreplayed;		/* columns a worker wrote */
	Snapshot	snapshot;
	WalUsage   *walusage;
	BufferUsage *bufferusage;
} LionBuildLeader;

extern PGDLLEXPORT void lion_parallel_build_main(dsm_segment *seg,
												 shm_toc *toc);

/* The file of participant p's tape of column c. */
static inline int
lion_parallel_colfile(const LionBuildShared *shared, int p, int c)
{
	return p * shared->ncols + c;
}

/* The file a worker writes column c to. */
static void
lion_parallel_colname(char *name, int c)
{
	snprintf(name, MAXPGPATH, "lion.column.%d", c);
}

/*
 * One participant's part of the scan, the leader's included: scan its share
 * of the heap into a spool, export the spool to its tapes, and report.
 */
static void
lion_parallel_scan_and_spool(LionBuildShared *shared, TapeShare *tapes,
							 LionBuildColumn *columns, Relation heap,
							 Relation index, int filenum, int memkb,
							 bool progress)
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
	int64	   *blocks;
	int			c;

	(void) lion_build_index_state(index, &ix, cxt);
	spool = lion_spool_begin(&ix, (Size) memkb * 1024, &shared->fileset,
							 lion_parallel_colfile(shared, filenum, 0));

	indexInfo = BuildIndexInfo(index);
	indexInfo->ii_Concurrent = shared->isconcurrent;
	scan = lion_table_beginscan_parallel(heap,
										 ParallelTableScanFromLionBuildShared(shared));
	reltuples = table_index_build_scan(heap, index, indexInfo, true, progress,
									   lion_build_callback, spool, scan);

	blocks = palloc(sizeof(int64) * shared->ncols);
	lion_spool_export(spool, &tapes[filenum * shared->ncols], blocks);

	SpinLockAcquire(&shared->mutex);
	for (c = 0; c < shared->ncols; c++)
		columns[c].blocks += blocks[c];
	shared->nparticipantsdone++;
	shared->reltuples += reltuples;
	shared->indtuples += lion_spool_nrows(spool);
	if (indexInfo->ii_BrokenHotChain)
		shared->brokenhotchain = true;
	SpinLockRelease(&shared->mutex);
	ConditionVariableBroadcast(&shared->workersdonecv);

	lion_spool_end(spool);
	MemoryContextSwitchTo(oldctx);
	MemoryContextDelete(cxt);
}

/* Wait for every participant's tapes; returns how many participants. */
static int
lion_parallel_wait_scan(LionBuildShared *shared)
{
	int			n;

	for (;;)
	{
		SpinLockAcquire(&shared->mutex);
		n = shared->nparticipants;
		if (n > 0 && shared->nparticipantsdone == n)
		{
			SpinLockRelease(&shared->mutex);
			break;
		}
		SpinLockRelease(&shared->mutex);
		ConditionVariableSleep(&shared->workersdonecv,
							   WAIT_EVENT_PARALLEL_CREATE_INDEX_SCAN);
	}
	ConditionVariableCancelSleep();
	return n;
}

/*
 * The reader over the participants' tapes of column c: the workers' files are
 * numbered from 0, the leader's comes after, and the merge reads them in that
 * order.
 */
static LionSpoolReader *
lion_parallel_reader(LionBuildShared *shared, TapeShare *tapes,
					 LionIndexState *ix, int c, int nparticipants,
					 Size membytes)
{
	int		   *filenums = palloc(sizeof(int) * nparticipants);
	TapeShare  *shares = palloc(sizeof(TapeShare) * nparticipants);
	LionSpoolReader *rd;
	int			i;

	for (i = 0; i < nparticipants; i++)
	{
		int			p = (i < nparticipants - 1) ? i : shared->leaderfile;

		filenums[i] = lion_parallel_colfile(shared, p, c);
		shares[i] = tapes[p * shared->ncols + c];
	}
	rd = lion_spool_reader_begin(ix, c, &shared->fileset, nparticipants,
								 filenums, shares, membytes);
	pfree(filenums);
	pfree(shares);
	return rd;
}

/*
 * The column the leader keeps for itself besides the first: the largest, when
 * it is at least as large as all the others together, or else none (0).  A
 * worker writing such a column would leave the leader replaying it after the
 * others are long written; the leader writing it itself starts on it as soon
 * as the columns before it are in.  Every participant works it out from the
 * same sizes, which are final once the scan is over.
 */
static int
lion_parallel_own_column(LionBuildShared *shared, LionBuildColumn *columns)
{
	int64		total = 0;
	int			best = 0;
	int			c;

	SpinLockAcquire(&shared->mutex);
	for (c = 0; c < shared->ncols; c++)
	{
		total += columns[c].blocks;
		if (columns[c].blocks > columns[best].blocks)
			best = c;
	}
	if (columns[best].blocks * 2 < total)
		best = 0;
	SpinLockRelease(&shared->mutex);
	return best;
}

/*
 * A worker's next column: the largest that is neither taken nor the
 * leader's, or -1 when none is left.
 */
static int
lion_parallel_claim_column(LionBuildShared *shared, LionBuildColumn *columns,
						   int owncol)
{
	int			best = -1;
	int			c;

	SpinLockAcquire(&shared->mutex);
	for (c = 1; c < shared->ncols; c++)
	{
		if (c == owncol || columns[c].taken)
			continue;
		if (best < 0 || columns[c].blocks > columns[best].blocks)
			best = c;
	}
	if (best >= 0)
		columns[best].taken = true;
	SpinLockRelease(&shared->mutex);
	return best;
}

/*
 * A worker's part of the write: take a column nobody has, write it to its
 * file, and say so, until no column is left.  memkb is the worker's share of
 * maintenance_work_mem, as for the scan.
 */
static void
lion_parallel_write_columns(LionBuildShared *shared, LionBuildColumn *columns,
							TapeShare *tapes, Relation index, int memkb)
{
	LionBuildState bs;
	bool		started = false;
	int			nparticipants;
	int			owncol;

	/* an index of one column is the leader's alone */
	if (shared->ncols == 1)
		return;
	nparticipants = lion_parallel_wait_scan(shared);
	owncol = lion_parallel_own_column(shared, columns);

	for (;;)
	{
		char		name[MAXPGPATH];
		int			c = lion_parallel_claim_column(shared, columns, owncol);

		if (c < 0)
			break;

		if (!started)
		{
			lion_build_state_init(&bs, index, memkb);
			bs.slack = shared->slack;
			started = true;
		}

		/* each column counts its own blocks, from 0 */
		bs.nblocks = 0;
		bs.wrote_narrow = false;
		bs.summary_cols = 0;
		bs.nsummaries = 0;
		lion_parallel_colname(name, c);
		bs.colout = BufFileCreateFileSet(&shared->fileset.fs, name);
		bs.reader = lion_parallel_reader(shared, tapes, &bs.ix, c,
										 nparticipants, (Size) memkb * 1024);
		lion_build_column(&bs, c);
		lion_spool_reader_end(bs.reader);
		bs.reader = NULL;
		lion_build_colout_record(&bs, LION_COLREC_END, 0, NULL, 0);
		BufFileClose(bs.colout);
		bs.colout = NULL;

		SpinLockAcquire(&shared->mutex);
		columns[c].wrote_narrow = bs.wrote_narrow;
		columns[c].summarized = (bs.summary_cols != 0);
		columns[c].nsummaries = bs.nsummaries;
		columns[c].done = true;
		SpinLockRelease(&shared->mutex);
		ConditionVariableBroadcast(&shared->coldonecv);
	}

	if (started)
		lion_build_state_end(&bs);
}

/*
 * The leader's turn at column c: true when the leader is to write it itself,
 * because it is one of its own or nobody has taken it; false when a worker
 * has, and the leader is to replay it.  *alone says whether every worker is
 * done meanwhile: the columns before c are in, and every one after it that
 * is not the leader's own is written already.
 */
static bool
lion_parallel_take_column(LionBuildLeader *leader, int c, bool *alone)
{
	LionBuildShared *shared = leader->shared;
	LionBuildColumn *col = &leader->columns[c];
	bool		mine;
	int			i;

	SpinLockAcquire(&shared->mutex);
	mine = (c == 0 || c == leader->owncol || !col->taken);
	col->taken = true;
	*alone = true;
	for (i = c + 1; i < shared->ncols; i++)
	{
		if (i != leader->owncol && !leader->columns[i].done)
			*alone = false;
	}
	SpinLockRelease(&shared->mutex);
	return mine;
}

/* Wait for the worker that took column c, and write what it wrote. */
static void
lion_parallel_replay_column(LionBuildState *bs, int c)
{
	LionBuildLeader *leader = bs->leader;
	LionBuildShared *shared = leader->shared;
	LionBuildColumn *col = &leader->columns[c];
	char		name[MAXPGPATH];
	BufFile    *file;
	bool		done;

	for (;;)
	{
		SpinLockAcquire(&shared->mutex);
		done = col->done;
		SpinLockRelease(&shared->mutex);
		if (done)
			break;
		ConditionVariableSleep(&shared->coldonecv, WAIT_EVENT_PARALLEL_FINISH);
	}
	ConditionVariableCancelSleep();

	lion_parallel_colname(name, c);
	file = BufFileOpenFileSet(&shared->fileset.fs, name, O_RDONLY, false);
	lion_build_replay_column(bs, file);
	BufFileClose(file);
	BufFileDeleteFileSet(&shared->fileset.fs, name, false);
	leader->nreplayed++;

	if (col->wrote_narrow)
		bs->wrote_narrow = true;
	if (col->summarized)
		bs->summary_cols |= ((uint32) 1) << (bs->ix.cols[c].attno - 1);
	bs->nsummaries += col->nsummaries;
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
	Size		estcolumns;
	LionBuildShared *shared;
	TapeShare  *tapes;
	LionBuildColumn *columns;
	int			ncols = bs->ix.ncolumns;
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
	esttapes = mul_size(sizeof(TapeShare), mul_size(request + 1, ncols));
	estcolumns = mul_size(sizeof(LionBuildColumn), ncols);
	shm_toc_estimate_chunk(&pcxt->estimator, estshared);
	shm_toc_estimate_chunk(&pcxt->estimator, esttapes);
	shm_toc_estimate_chunk(&pcxt->estimator, estcolumns);
	shm_toc_estimate_keys(&pcxt->estimator, 3);
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
	shared->ncols = ncols;
	shared->slack = bs->slack;
	shared->queryid = pgstat_get_my_query_id();
	SharedFileSetInit(&shared->fileset, pcxt->seg);
	ConditionVariableInit(&shared->workersdonecv);
	SpinLockInit(&shared->mutex);
	shared->nparticipants = 0;
	shared->nparticipantsdone = 0;
	shared->reltuples = 0.0;
	shared->indtuples = 0.0;
	shared->brokenhotchain = false;
	ConditionVariableInit(&shared->coldonecv);
	table_parallelscan_initialize(heap,
								  ParallelTableScanFromLionBuildShared(shared),
								  snapshot);
	ParallelTableScanFromLionBuildShared(shared)->phs_syncscan = false;

	tapes = (TapeShare *) shm_toc_allocate(pcxt->toc, esttapes);
	memset(tapes, 0, esttapes);
	columns = (LionBuildColumn *) shm_toc_allocate(pcxt->toc, estcolumns);
	memset(columns, 0, estcolumns);

	shm_toc_insert(pcxt->toc, PARALLEL_KEY_LION_SHARED, shared);
	shm_toc_insert(pcxt->toc, PARALLEL_KEY_LION_TAPES, tapes);
	shm_toc_insert(pcxt->toc, PARALLEL_KEY_LION_COLUMNS, columns);
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
	leader->columns = columns;
	leader->snapshot = snapshot;

	if (pcxt->nworkers_launched == 0)
	{
		lion_end_parallel(leader);
		return;
	}
	bs->leader = leader;

	/* the workers wait for every participant before they write a column */
	SpinLockAcquire(&shared->mutex);
	shared->nparticipants = leader->nparticipants;
	SpinLockRelease(&shared->mutex);
	ConditionVariableBroadcast(&shared->workersdonecv);

	/* The leader takes part as a worker does, on the file after theirs. */
	lion_parallel_scan_and_spool(shared, tapes, columns, heap, index,
								 shared->leaderfile,
								 maintenance_work_mem / leader->nparticipants,
								 true);

	/* Make sure a worker that failed to start cannot leave us waiting. */
	WaitForParallelWorkersToAttach(pcxt);
}

/*
 * Wait for every participant's tapes.  Returns the number of heap tuples the
 * scan saw.
 */
static double
lion_parallel_heapscan(LionBuildState *bs, bool *brokenhotchain)
{
	LionBuildShared *shared = bs->leader->shared;
	double		reltuples;

	(void) lion_parallel_wait_scan(shared);
	bs->leader->owncol = lion_parallel_own_column(shared, bs->leader->columns);
	SpinLockAcquire(&shared->mutex);
	reltuples = shared->reltuples;
	bs->indtuples = shared->indtuples;
	if (shared->brokenhotchain)
		*brokenhotchain = true;
	SpinLockRelease(&shared->mutex);

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
	LionBuildColumn *columns;
	Relation	heap;
	Relation	index;
	LOCKMODE	heaplock;
	LOCKMODE	indexlock;
	WalUsage   *walusage;
	BufferUsage *bufferusage;
	int			memkb;

	debug_query_string = shm_toc_lookup(toc, PARALLEL_KEY_QUERY_TEXT, true);
	pgstat_report_activity(STATE_RUNNING, debug_query_string);

	shared = shm_toc_lookup(toc, PARALLEL_KEY_LION_SHARED, false);
	tapes = shm_toc_lookup(toc, PARALLEL_KEY_LION_TAPES, false);
	columns = shm_toc_lookup(toc, PARALLEL_KEY_LION_COLUMNS, false);

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

	memkb = maintenance_work_mem / shared->nrequested;
	lion_parallel_scan_and_spool(shared, tapes, columns, heap, index,
								 ParallelWorkerNumber, memkb, false);
	lion_parallel_write_columns(shared, columns, tapes, index, memkb);

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

	lion_build_state_init(&bs, index, maintenance_work_mem);
	ncols = bs.ix.ncolumns;

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
		bs.indtuples = lion_spool_nrows(bs.spool);
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
	 * In a parallel build a worker may have written a column already, and
	 * the leader then replays it; a column it writes itself it gives its
	 * share of maintenance_work_mem while a worker may still be writing one,
	 * and all of it once none can.
	 */
	for (c = 0; c < ncols; c++)
	{
#if PG_VERSION_NUM >= 170000
		if (bs.leader != NULL)
		{
			bool		alone;

			if (!lion_parallel_take_column(bs.leader, c, &alone))
			{
				lion_parallel_replay_column(&bs, c);
				continue;
			}
			bs.memkb = alone ? maintenance_work_mem :
				maintenance_work_mem / bs.leader->nparticipants;
			bs.reader = lion_parallel_reader(bs.leader->shared,
											 bs.leader->tapes, &bs.ix, c,
											 bs.leader->nparticipants,
											 (Size) bs.memkb * 1024);
		}
#endif
		lion_build_column(&bs, c);
#if PG_VERSION_NUM >= 170000
		if (bs.reader != NULL)
		{
			lion_spool_reader_end(bs.reader);
			bs.reader = NULL;
		}
#endif
	}

	lion_build_finish_dir(&bs);

	Assert(BlockNumberIsValid(bs.root));
	metabuf = smgr_bulk_get_buf(bs.bulk);
	lion_init_metapage((Page) metabuf->data, bs.inline_limit, bs.root,
					  bs.height, (uint32) bs.ndirpages,
					  lion_wal_mode_for_build(index));
	/* ... and the order the directory was just laid out in (§21). */
	lion_meta_record_order(LionPageGetMeta((Page) metabuf->data), &bs.ix);
	/* ... and which columns got summaries (§32). */
	lion_meta_record_summaries(LionPageGetMeta((Page) metabuf->data),
							   bs.summary_cols, bs.sumtids);
	/* ... and whether it has a NARROW item, which VACUUM may then add (§38). */
	lion_meta_record_narrow(LionPageGetMeta((Page) metabuf->data),
							bs.wrote_narrow);
	/* ... and each column's distinct keys, for the planner (§33). */
	{
		LionMetaNdistinct nd;

		lion_meta_fill_ndistinct(&nd, &bs.ix, bs.nvalues, bs.rows);
		lion_meta_record_ndistinct((Page) metabuf->data, &nd);
	}
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
#if PG_VERSION_NUM >= 170000
	if (bs.leader != NULL)
		elog(DEBUG2, "lion index \"%s\": %d of %d key columns written by workers",
			 RelationGetRelationName(index), bs.leader->nreplayed, ncols);
#endif

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
	if (bs.leader != NULL)
		lion_end_parallel(bs.leader);
#endif
	lion_build_state_end(&bs);

	/*
	 * The index's tuples are its ROWS, as VACUUM reports them (DESIGN.md §18,
	 * "Statistics"): pg_class.reltuples, which the planner sizes a partial
	 * index by (estimate_rel_size()), and the count pushdown prices one by
	 * (DESIGN.md §45).  The postings a row makes - one a key column, one an
	 * element - would make a partial index's rows a multiple of themselves
	 * until the first VACUUM or ANALYZE.
	 */
	result = (IndexBuildResult *) palloc0(sizeof(IndexBuildResult));
	result->heap_tuples = reltuples;
	result->index_tuples = bs.indtuples;

	return result;
}
