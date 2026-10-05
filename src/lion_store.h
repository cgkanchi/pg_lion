/*-------------------------------------------------------------------------
 *
 * lion_store.h
 *	  The window store (DESIGN.md §40): the values of an index's stored
 *	  columns - its scalar key columns under `store_values`, and its INCLUDE
 *	  columns - kept per 64-page window of the heap and addressed by TID.
 *
 * Three structures, all in lion_store.c:
 *
 *	the WINDOW MAP	pages of kind LION_PAGE_STOREMAP, a radix of fixed depth
 *					two (root, inner, leaf) whose leaves hold one BlockNumber
 *					per (window, stored column): the head of that window's
 *					store chain for that column, 0 = none yet.  Every map
 *					page holds one item of LION_STOREMAP_FANOUT BlockNumbers
 *					and is written with LION_OP_SETBYTES only.  Map pages are
 *					never freed and never move, so their block numbers can be
 *					cached for the life of the relation.
 *
 *	STORE PAGES		pages of kind LION_PAGE_STORE, rightlinked in heap-page
 *					order; each holds one stored column of a contiguous range
 *					of heap pages of one window, and everything needed to
 *					decode it (lion_store_fmt.h).  owner_head is the window
 *					(ckey) and owner_hash the column's ordinal, which every
 *					reader checks after pinning a page (§18).
 *
 *	the record		LionMetaStore on the meta page (lion.h).
 *
 * LOCKS.  A store page is never locked together with a directory or posting
 * page, in either order, so none of the lock-order arguments of §21 and §22
 * changes.  Among themselves: a writer holds at most one existing store page,
 * plus pages it has just allocated (which nobody else can reach), plus a map
 * page and then the meta page; nobody takes a store page while holding a map
 * page or the meta page except a page it has itself just allocated.  Readers
 * hold one page at a time.
 *
 *-------------------------------------------------------------------------
 */
#ifndef LION_STORE_H
#define LION_STORE_H

#include "access/genam.h"
#include "storage/buf.h"
#include "storage/itemptr.h"

#include "lion.h"
#include "lion_store_fmt.h"

/* ---------- the window map ---------- */

/* The one item of a map page, and the BlockNumbers it holds. */
#define LION_STOREMAP_ITEM_SIZE	((Size) LION_MAX_ITEM_SIZE)
#define LION_STOREMAP_FANOUT \
	((uint32) (LION_STOREMAP_ITEM_SIZE / sizeof(BlockNumber)))

/* LionPageOpaqueData.level of a map page */
#define LION_STOREMAP_LEAF		0
#define LION_STOREMAP_INNER		1
#define LION_STOREMAP_ROOT		2

/*
 * The slot of (window, ordinal) and where it lives: leaf slot / FANOUT,
 * entry (slot / FANOUT) mod FANOUT of inner slot / FANOUT².  A map page's
 * owner_head is its number in its level (the root's is 0), which verify()
 * checks against the path that reached it.
 */
static inline uint64
lion_storemap_slot(uint32 ckey, int nstored, int ord)
{
	return (uint64) ckey * (uint64) nstored + (uint64) ord;
}

/*
 * The map slots a window takes: one per stored column, and for an index in
 * key-ordered windows (DESIGN.md §41) one more, the permutation's, at
 * ordinal nstored.  Every slot computation passes this as its "nstored".
 */
#define lion_store_stride(ix) \
	((ix)->nstored + ((ix)->store_order >= 0 ? 1 : 0))

/* ---------- a stored column ---------- */

/*
 * What the store needs to know about a stored column, decided from the
 * catalog when the index state is built (lion_store_fill_state()).  The
 * stored datum is the COLUMN's - the heap attribute's or the expression's -
 * which for a key column is not necessarily the index tuple descriptor's
 * attribute type: amstorage lets an operator class declare a storage type.
 */
typedef struct LionStoreCol
{
	int			ord;			/* 0 .. nstored - 1 */
	AttrNumber	attno;			/* 1-based index column, key or INCLUDE */
	bool		iskey;			/* a key column (else INCLUDE) */
	Oid			typid;			/* the stored datum's type */
	int16		typlen;			/* > 0 fixed, -1 varlena */
	bool		typbyval;
	char		typalign;

	/*
	 * Can an index-only scan return the stored datum as this index column
	 * (DESIGN.md §40, "Index-only scans")?  Only when the stored type is the
	 * index tuple descriptor's own: core forms the scan's output from xs_itup
	 * with that descriptor.  Phase B's amcanreturn answers from this.
	 */
	bool		returnable;

	/* RAW slot bytes: typlen, or store_max_len + 2; 0 = DICT only */
	int			rawwidth;
	int			maxlen;			/* store_max_len for a varlena, else 0 */

	/*
	 * Which pages its chains have (DESIGN.md §41): LION_STORE_KIND_HEAP, a
	 * column of an index in heap order; _ORDERED, a column of an ordered
	 * index, over virtual pages; _PERM, the permutation (ordinal nstored).
	 */
	uint8		kind;
} LionStoreCol;

#define LION_STORE_KIND_HEAP		0
#define LION_STORE_KIND_ORDERED		1
#define LION_STORE_KIND_PERM		2

/* The heap or virtual pages a chain of col covers: 0 .. this - 1. */
#define lion_store_col_pages(col) \
	((col)->kind == LION_STORE_KIND_ORDERED ? LION_STORE_MAX_VPAGES : \
	 LION_BLOCKS_PER_CONTAINER)

/*
 * Which index columns a build stores (DESIGN.md §40, "Which columns"), as a
 * store_cols mask, from the reloptions and ix's key columns.  ERRORs for a
 * column that cannot be stored and for `store_values` with nothing to store;
 * with `report`, says so in a NOTICE for each key column it leaves out.
 */
extern uint32 lion_store_columns(Relation index, LionIndexState *ix,
								 bool report);

/*
 * Fill ix's stored columns from a store record (ambuild passes the one it is
 * about to write).  A record whose columns are not ones a build could have
 * chosen is damage.
 */
extern void lion_store_fill_state(Relation index, LionIndexState *ix,
								  const LionMetaStore *store,
								  MemoryContext cxt);

/* The stored ordinal of index column attno, or -1. */
extern int	lion_store_ordinal(const LionIndexState *ix, AttrNumber attno);

/*
 * The order column of a build (DESIGN.md §41): the stored ordinal its
 * windows are sorted by, or -1 for heap order, from `cluster_column` and
 * pg_lion.store_heap_order; ERRORs for a
 * `cluster_column` that names no stored column with an ordering.  *flags gets
 * LION_STORE_ORDER_CLUSTER when the reloption chose it.
 */
extern int	lion_store_choose_order(Relation index, LionIndexState *ix,
									uint16 *flags);

/*
 * Make ix an index in key-ordered windows by order column `order` (-1: heap
 * order, nothing changes): the columns' kinds and the permutation's column.
 */
extern void lion_store_fill_order(Relation index, LionIndexState *ix,
								  int order, uint32 order_cols,
								  MemoryContext cxt);

/*
 * The stored ordinals of an ordered index laid out in key order (bit i for
 * ordinal i; §41, "Revision 2: short chains in heap order"); 0 in heap order.
 */
extern uint32 lion_store_order_cols(const LionIndexState *ix);

/*
 * The columns an ordered index with no rows to measure lays out in key order,
 * by their types (§41): 0 says no column is worth it, and the index is built
 * in heap order.
 */
extern uint32 lion_store_order_cols_by_type(const LionIndexState *ix, int order);

/*
 * The order column a version 10 meta page records, -1 for any other, and the
 * columns it lays out in key order (*order_cols).
 */
extern int	lion_read_meta_store_order(Relation index,
									   const LionMetaPageData *meta,
									   uint32 *order_cols);

/* pg_lion.store_heap_order: builds write heap order (a testing knob, §41). */
extern bool lion_store_heap_order;

/*
 * pg_lion.store_order_min_pages: the heap-order chain pages a column's chain
 * takes in a build's first window for an ordered index to lay that column out
 * in key order (§41); 0 lays every column out in key order.
 */
extern int	lion_store_order_min_pages;

/*
 * Read the meta page's store record into *store, zeroed when meta's version
 * has none; a version 9 meta page without a sound one is damage.
 */
extern void lion_read_meta_store(Relation index, const LionMetaPageData *meta,
								 LionMetaStore *store);

/* A fresh map page image: level, number, and its one item of zeros. */
extern void lion_storemap_init_page(Page page, uint16 level, uint32 number);

/* ---------- build (serial and parallel, lion_build.c) ---------- */

typedef struct LionStoreBuild LionStoreBuild;

/*
 * The store half of ambuild.  Pages go through the build's bulk writer and
 * take their blocks from its counter (*nblocks, lion_build_alloc_block()'s),
 * so the writer must be started before the first row is added.  Rows must
 * come in heap block order (offsets within a page in any order); each
 * window's pages are written as soon as the next window begins.  _finish
 * writes the last window and then the map, and fills in the store record.
 */
extern LionStoreBuild *lion_store_build_begin(Relation index,
											  LionIndexState *ix,
											  BulkWriteState *bulk,
											  BlockNumber *nblocks);
extern void lion_store_build_add(LionStoreBuild *sb, ItemPointer tid,
								 const Datum *values, const bool *isnull);
extern void lion_store_build_finish(LionStoreBuild *sb, LionMetaStore *store);

/* ---------- insert (lioninsert) ---------- */

/*
 * Write tid's value of every stored column.  lioninsert() calls it BEFORE it
 * touches a posting set (DESIGN.md §40, "Writes"), with nothing held, and
 * nothing is held when it returns.
 */
extern void lion_store_insert(Relation index, Relation heaprel,
							  LionIndexState *ix, ItemPointer tid,
							  const Datum *values, const bool *isnull);

/* ---------- VACUUM (lion_vacuum.c) ---------- */

/* Mark a block accounted for in VACUUM's walk (the sweep leaves it alone). */
typedef void (*LionStoreVisit) (void *arg, BlockNumber blk);

typedef struct LionStoreVacStats
{
	int64		pages;			/* store pages visited */
	int64		mappages;		/* map pages visited */
	int64		cleared;		/* slots cleared */
	int64		rewritten;		/* pages rewritten */
	int64		records;		/* WAL records written */
} LionStoreVacStats;

/*
 * ambulkdelete's pass over the store (DESIGN.md §40, "VACUUM"): every map and
 * store page reachable from the root is visited once and marked, and, when
 * `write`, every written slot whose TID the callback calls dead is cleared,
 * under an EXCLUSIVE lock on its page.  A VACUUM that may not write the
 * index (an rmgr-mode index without the preload) only visits.
 */
extern void lion_store_bulkdelete(Relation index, LionIndexState *ix,
								  IndexBulkDeleteCallback callback,
								  void *callback_state, bool write,
								  LionStoreVisit visit, void *visitarg,
								  LionStoreVacStats *st);

/*
 * amvacuumcleanup's: clear the map slots of the windows that begin at or past
 * the heap's end, and free their chains through the DELETED protocol of
 * §18.  Takes the heap's extension lock for the part that must not race an
 * insert (see the function).  Returns the pages freed.
 */
extern int64 lion_store_vacuum_cleanup(Relation index, Relation heaprel,
									   LionIndexState *ix);

/*
 * verify()'s check of an ordered window whole (DESIGN.md §41, "verify()"):
 * the window header, the permutation naming distinct slots inside their
 * buckets, each row inside its bucket's range, the directory.  ERRORs on
 * damage; a no-op in heap order.
 */
extern void lion_store_verify_window(Relation index, LionIndexState *ix,
									 uint32 ckey);

/* lion_index_store_window()'s look at one window of an ordered index (§41). */
typedef struct LionStoreWindowInfo
{
	bool		exists;			/* the window has a store at all */
	int64		gen;			/* the window header's generation */
	int			buckets;		/* its buckets */
	int64		slots;			/* the slots they have handed out */
	int			ndir;
	bool		thin;
	int64		entries;		/* positions with a permutation entry */
	int64		orphaned;		/* ... naming a virtual page no bucket has */
	int			perm_pages;		/* the permutation's chain */
	int			pages;			/* the ordered columns' chains */
	int			heap_pages;		/* the chains of columns in heap order */
} LionStoreWindowInfo;

extern void lion_store_window_info(Relation index, LionIndexState *ix,
								   uint32 ckey, LionStoreWindowInfo *wi);

/*
 * The readers' page model (DESIGN.md §41, "Costs").  A planner's view of an
 * index's store: whether it is in key-ordered windows, its rows a window,
 * the permutation's pages a window, the store pages the columns have (the
 * meta page's count less the permutation's), and the values of the order
 * column the gathered members have (0: not pinned, lion_store_shape_pin()).  Every reader's cost
 * function prices a window's chain of a column as for heap order -
 * heapreads pages of colpages for `members` members of the window - and
 * lion_store_window_pages() says what the order makes of it: for a column in
 * key order, the pages the members of each pinned value span as adjacent
 * slots, never more than heapreads, plus the permutation's pages, which
 * every gather of one reads; for a column an ordered index keeps in heap
 * layout (§41, "Revision 2"), and in heap order, heapreads, unchanged.
 */
typedef struct LionStoreShape
{
	bool		ordered;
	double		rowsper;
	double		permpages;
	double		datapages;
	int			ordercol;		/* the order column's index column */
	double		nvals;
	uint32		keyordered;		/* index columns in key order, bit attno - 1 */
	double		fracordered;	/* ... the share of the stored columns */
} LionStoreShape;

struct IndexOptInfo;
extern void lion_store_shape(LionIndexState *ix, struct IndexOptInfo *idx,
							 double storepages, LionStoreShape *sh);

/*
 * Which values the members a reader gathers have in the order column: the
 * equality or IN list of constants or parameters among clauses (RestrictInfos
 * or bare clauses) on it, leaving out those in except - the filters a reader
 * applies after the gather, whose rows it gathers all the same.
 */
extern void lion_store_shape_pin(struct IndexOptInfo *idx, LionStoreShape *sh,
								 List *clauses, List *except);
/*
 * indexcol is the index column (1-based) whose chain is priced, or 0 for a
 * column of the index's stored ones not known, priced as their average.
 */
extern double lion_store_window_pages(const LionStoreShape *sh, int indexcol,
									  double heapreads, double colpages,
									  double members);

/*
 * The leak sweep's question about a live STORE page VACUUM's walk did not
 * reach (lion_vacuum_sweep()): is it on the chain its special area names?
 * Asked with nothing held.  A page that is not is an orphan of an interrupted
 * cleanup, which the sweep frees with lion_store_free_page().
 */
extern bool lion_store_page_linked(Relation index, LionIndexState *ix,
								   BlockNumber blk, uint32 ckey, uint16 ord);

/*
 * Mark the store page in buf, which the caller holds EXCLUSIVE, DELETED, take
 * it off the meta page's count and hand it to the free space map.  buf is
 * released.
 */
extern void lion_store_free_page(Relation index, Buffer buf);

/* ---------- reading: the gather (DESIGN.md §40, "Reads") ---------- */

typedef struct LionStoreReader LionStoreReader;

/*
 * A reader of one stored column, by ordinal (lion_store_ordinal()).  It keeps
 * its own memory in a child of cxt and caches the map blocks and the last
 * window's chain head it looked up; nothing it caches is trusted without the
 * check of the page it leads to.
 */
extern LionStoreReader *lion_store_open(Relation index, LionIndexState *ix,
										int ord, MemoryContext cxt);
extern void lion_store_close(LionStoreReader *r);

/*
 * Let reader r take an ordered window's permutation (DESIGN.md §41) from
 * leader, a reader of another column of the same index: when r gathers the
 * window and the members leader gathered last, it uses the entries leader
 * read instead of reading the permutation again, and checks every data page
 * against their generation as it would its own.  The caller gathers with
 * leader first for each window, and closes the two together.  A no-op in
 * heap order.
 */
extern void lion_store_share(LionStoreReader *r, LionStoreReader *leader);

/*
 * The values of window ckey's members lo[0 .. nlo - 1], which must be in
 * ascending order (heap page, then offset: the order a container iterates
 * in), into values[] and isnull[].  A by-reference value is copied into the
 * reader's memory, which lion_store_gather_reset() frees; nothing returned
 * points into a page.  Bit k of *absent_pages is set for every heap page
 * lion_ckey_first_block(ckey) + k of the members that the store cannot
 * supply - an ABSENT sub-array, or a window or heap page with no store yet -
 * and its members come back NULL: the caller takes those from the heap, as it
 * does the members of a page that is not all-visible.
 *
 * WHY IT IS SAFE TO USE (DESIGN.md §40, "Why it is safe").  The gather takes
 * no position on visibility and holds nothing when it returns: it takes a
 * SHARE lock on one store page at a time, only while it decodes one heap
 * page's sub-array, and pins nothing across calls.  A caller may use a value
 * it returns for a TID only when that TID came from a container page the
 * caller still holds pinned (§9) AND the visibility map calls the TID's heap
 * page all-visible.  Then the row is visible to every snapshot, its slot was
 * written by the row's own insert before the TID reached any container (the
 * insert order of §40, "Writes"), and VACUUM - which clears a slot only for a
 * TID dead to every snapshot, and sets a page all-visible only after
 * ambulkdelete has returned, which it cannot do while the caller's container
 * pin stands - cannot have cleared or reused it.  For any other TID the caller
 * goes to the heap, which settles visibility and value together, and the
 * store is not consulted.  On a hot standby a generic-mode index has no such
 * interlock (§9) and every TID goes to the heap.
 */
extern void lion_store_gather(LionStoreReader *r, uint32 ckey,
							  const uint16 *lo, int nlo,
							  Datum *values, bool *isnull,
							  uint64 *absent_pages);

/* Free the values every gather since the last reset returned. */
extern void lion_store_gather_reset(LionStoreReader *r);

/* ---------- page access for lion_index_stats() and verify() ---------- */

/*
 * Is page, reached as the store of window ckey for ordinal ord, a live store
 * page of that window and column?  The check every reader makes after
 * pinning (§18); false is not an error outside verify().
 */
extern bool lion_store_page_owned(Page page, uint32 ckey, uint16 ord);

/*
 * Check the structure of a store page lion_store_page_owned() accepted for
 * col: header, dictionary and every sub-array consistent with each other.
 * NULL when sound, else what is wrong (palloc'd).  Codes are not checked
 * against ndict here (lion_store_page_check_codes() does that).
 */
extern char *lion_store_page_check(Page page, const LionStoreCol *col);
extern char *lion_store_page_check_codes(Page page);

/*
 * Is page number `number` of `level` of the window map sound?  NULL when it
 * is, else what is wrong (palloc'd).
 */
extern char *lion_storemap_page_check(Page page, uint16 level, uint64 number);

/* The entries of a map page lion_storemap_page_check() accepted. */
static inline BlockNumber *
lion_storemap_page_entries(Page page)
{
	return (BlockNumber *) PageGetItem(page, PageGetItemId(page, FirstOffsetNumber));
}

/*
 * Is blk, a sound map page of (level, number) that a walk did not reach,
 * linked into the map now (a page an insert added behind the walk)?
 */
extern bool lion_storemap_linked(Relation index, LionIndexState *ix,
								 BlockNumber blk, uint16 level, uint64 number);

/*
 * verify()'s heapallindexed question: does the store hold (d, isnull) as the
 * value of tid, a row the check's snapshot sees?  ABSENT says the store
 * leaves that heap page to the heap; MISSING that it has no slot for the
 * row, which for a visible row is damage.
 */
#define LION_STORE_CMP_EQUAL		0
#define LION_STORE_CMP_DIFFERENT	1
#define LION_STORE_CMP_ABSENT		2
#define LION_STORE_CMP_MISSING		3

extern int	lion_store_compare(LionStoreReader *r, ItemPointer tid, Datum d,
							   bool isnull);

/* The header of a page lion_store_page_check() accepted. */
static inline LionStoreHeader *
lion_store_page_header(Page page)
{
	return (LionStoreHeader *) PageGetItem(page,
										   PageGetItemId(page, LION_STORE_HDR_OFF));
}

/* Sub-array k (heap page k of the window) of such a page. */
static inline LionStoreSub *
lion_store_page_sub(Page page, int k, Size *len)
{
	LionStoreHeader *h = lion_store_page_header(page);
	ItemId		iid = PageGetItemId(page,
									(OffsetNumber) (LION_STORE_SUB_FIRST + k - h->lo));

	*len = ItemIdGetLength(iid);
	return (LionStoreSub *) PageGetItem(page, iid);
}

#endif							/* LION_STORE_H */
