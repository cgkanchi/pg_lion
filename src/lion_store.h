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
} LionStoreCol;

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
