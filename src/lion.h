/*-------------------------------------------------------------------------
 * lion.h
 *	  On-disk structures and internal API of the lion index AM.
 *	  See DESIGN.md §4-§7.  The structs in this file are the on-disk format
 *	  and are fixed; the prototypes are the contract between modules.
 *-------------------------------------------------------------------------
 */
#ifndef LION_H
#define LION_H

#include "postgres.h"

#include "lion_compat.h"

#include "access/amapi.h"
#include "access/genam.h"
#include "access/generic_xlog.h"
#include "access/itup.h"
#include "access/transam.h"
#include "fmgr.h"
#include "nodes/tidbitmap.h"
#include "storage/bufmgr.h"
#include "storage/bufpage.h"
#include "utils/relcache.h"

#include "lion_tid.h"
#include "lion_container.h"
#include "lion_sparse.h"
#include "lion_wal.h"

/* ---------- page special area ---------- */

#define LION_PAGE_ID			0xFF87

#define LION_PAGE_META		0x0001
#define LION_PAGE_BUCKET		0x0002	/* directory LEAF page (DESIGN.md §21) */
#define LION_PAGE_CONTAINER	0x0004
#define LION_PAGE_DELETED	0x0008	/* freed container page, DESIGN.md §18 */
#define LION_PAGE_DIR		0x0010	/* directory INTERNAL page (§21) */
#define LION_PAGE_ROOT		0x0020	/* ... and it is the current root (§21) */
#define LION_PAGE_INCOMPLETE_SPLIT 0x0040	/* its right sibling has no
											 * downlink yet (§21) */

/* The three page KINDS; exactly one of them is set on every page. */
#define LION_PAGE_KINDS \
	(LION_PAGE_META | LION_PAGE_BUCKET | LION_PAGE_CONTAINER | LION_PAGE_DIR)

/*
 * The special area of every page (format version 4, DESIGN.md §18 and §21).
 *
 * owner_hash/owner_head name the posting set a CONTAINER page belongs to, and
 * every reader that reaches a container page through an entry's head or a
 * rightlink checks them.  They exist because a freed page can be reused, and
 * generic WAL cannot raise a recovery conflict: on a standby a reader holding
 * a stale chain link could otherwise land on a page that replay has already
 * given to another key.
 *
 * owner_head is the chain's head block, and it is a chain identity that is
 * unique for the whole life of the index: a chain's head page is the only
 * page lion_new_buffer() refuses to take from the free space map (see
 * lion_entry_spill()), so a head block always comes from extending the
 * relation and no two chains, at any two times, can share one.  That is what
 * lets a reader that knows only the head block - the count cursor, whose
 * LionPostingSet does not carry the entry's hash - validate a page on its own.
 * owner_hash is the second half of the check for the readers that do have the
 * entry in hand, and what verify() uses to prove a chain belongs to its key.
 *
 * Directory and meta pages leave both at their empty values (0 and
 * InvalidBlockNumber).
 *
 * `level` and `leftlink` belong to the entry directory (DESIGN.md §21): the
 * leaves are level 0 and the root is level `height`, and every directory page
 * is linked to both of its siblings so that a reader that had to move right
 * can tell a concurrent split from corruption and so that VACUUM can walk the
 * leaves in key order.
 *
 * A CONTAINER page uses `level` too, since DESIGN.md §22: a posting set is a
 * B-tree over container keys whose LEAVES are the container pages of §4
 * (level 0, still rightlinked in ckey order) and whose INTERNAL pages hold
 * LionPostingPivot downlinks (level > 0).  Container pages leave `leftlink`
 * at InvalidBlockNumber: a posting page's left sibling is never followed, and
 * leaving it out is what keeps a posting-page split inside the four buffers
 * GenericXLog allows.
 */
typedef struct LionPageOpaqueData
{
	BlockNumber rightlink;		/* next page in chain or InvalidBlockNumber */
	BlockNumber leftlink;		/* previous page, directory pages (§21) */
	uint32		minckey;		/* container pages only; 0 when empty */
	uint32		maxckey;		/* container pages only; 0 when empty */
	uint32		owner_hash;		/* hash of the entry that owns this chain */
	BlockNumber owner_head;		/* head block of that chain */
	uint16		level;			/* directory pages: 0 = leaf (§21) */
	uint16		flags;			/* LION_PAGE_* */
	uint16		page_id;		/* LION_PAGE_ID */
	uint16		unused;			/* pad to a multiple of MAXIMUM_ALIGNOF */
} LionPageOpaqueData;

typedef LionPageOpaqueData *LionPageOpaque;

#define LionPageGetOpaque(page)	((LionPageOpaque) PageGetSpecialPointer(page))
#define LION_SPECIAL_SIZE		MAXALIGN(sizeof(LionPageOpaqueData))
#define LionPageIsMeta(page)		((LionPageGetOpaque(page)->flags & LION_PAGE_META) != 0)
#define LionPageIsBucket(page)	((LionPageGetOpaque(page)->flags & LION_PAGE_BUCKET) != 0)
#define LionPageIsContainer(page) ((LionPageGetOpaque(page)->flags & LION_PAGE_CONTAINER) != 0)
#define LionPageIsDeleted(page)	((LionPageGetOpaque(page)->flags & LION_PAGE_DELETED) != 0)
#define LionPageIsDir(page)		((LionPageGetOpaque(page)->flags & LION_PAGE_DIR) != 0)
#define LionPageIsLeaf(page)		LionPageIsBucket(page)
#define LionPageIsRoot(page)		((LionPageGetOpaque(page)->flags & LION_PAGE_ROOT) != 0)
#define LionPageIncompleteSplit(page) \
	((LionPageGetOpaque(page)->flags & LION_PAGE_INCOMPLETE_SPLIT) != 0)
#define LionPageIsRightmost(page) \
	(!BlockNumberIsValid(LionPageGetOpaque(page)->rightlink))

/* Total bytes a page can hold in items and line pointers. */
#define LION_PAGE_CAPACITY \
	((Size) (BLCKSZ - SizeOfPageHeaderData - LION_SPECIAL_SIZE))

/*
 * The body of a DELETED page: the transaction id from which on no scan can
 * still hold a link to it, exactly as nbtree's BTDeletedPageData (see
 * BTPageIsRecyclable() in access/nbtree.h).  Nothing else is left on the page.
 */
typedef struct LionDeletedPageData
{
	FullTransactionId safexid;
} LionDeletedPageData;

/*
 * Is this page a live container page of the chain whose head block is head?
 *
 * This is the check DESIGN.md §18 requires of every reader that follows an
 * entry's head or a rightlink.  A mismatch is not an error outside verify():
 * it means the chain was freed after the reader copied the entry, which can
 * only happen once the posting set held nothing visible to anyone, so the
 * reader simply stops there.
 */
static inline bool
lion_page_owns(Page page, BlockNumber head)
{
	LionPageOpaque opaque;

	if (PageIsNew(page) || PageGetSpecialSize(page) != LION_SPECIAL_SIZE)
		return false;
	opaque = LionPageGetOpaque(page);

	return opaque->page_id == LION_PAGE_ID &&
		(opaque->flags & (LION_PAGE_CONTAINER | LION_PAGE_DELETED)) ==
		LION_PAGE_CONTAINER &&
		opaque->owner_head == head;
}

/* The same with the entry's hash checked as well, for readers that have it. */
static inline bool
lion_page_owns_entry(Page page, uint32 hash, BlockNumber head)
{
	return lion_page_owns(page, head) &&
		LionPageGetOpaque(page)->owner_hash == hash;
}

/* ---------- the per-key posting tree (DESIGN.md §22) ---------- */

/*
 * A CHAIN entry's posting set is a B-tree over container keys, GIN's
 * posting-tree shape.  Its LEAVES are the container pages of §4 - items sorted
 * by first ckey, minckey/maxckey, owner stamps, growth slack, DELETED marking,
 * and still linked left to right in ckey order, so every sequential walk
 * (scans, counts, VACUUM) is what it always was.  Its INTERNAL pages hold
 * these pivots and let an intersection SEEK instead of stream.
 *
 * `ckey` is a SEPARATOR: every container key in the child's subtree is at or
 * above it.  `child` is InvalidBlockNumber on the one pivot that is not a
 * downlink, the HIGH KEY: the first item of every non-rightmost internal page
 * is a strict upper bound on the container keys of its whole subtree, which is
 * what lets a reader tell "my key moved right in a split that has not finished"
 * from "my key is simply not here" (nbtree's rule, and §21's).  Leaves carry
 * no high key - they hold containers, not pivots - and use `maxckey` in the
 * special area instead; that is enough for them because every WRITER of a key
 * holds that key's directory leaf EXCLUSIVE (§5, §21) and therefore no leaf
 * split can be in flight while another writer descends.
 */
typedef struct LionPostingPivot
{
	uint32		ckey;
	BlockNumber child;
} LionPostingPivot;

#define LION_POSTING_PIVOT_SIZE		((Size) sizeof(LionPostingPivot))

/* The greatest level a posting tree can reach; verify() and the descent bound
 * themselves by it so that a corrupt `level` cannot loop for ever. */
#define LION_POSTING_MAX_HEIGHT		16

static inline bool
LionPageIsPostingLeaf(Page page)
{
	return LionPageIsContainer(page) && LionPageGetOpaque(page)->level == 0;
}

static inline bool
LionPageIsPostingInternal(Page page)
{
	return LionPageIsContainer(page) && LionPageGetOpaque(page)->level > 0;
}

/*
 * The first item of a posting page that is not its high key.  Leaves have no
 * high key at all, so only a non-rightmost INTERNAL page skips one.
 */
static inline OffsetNumber
lion_posting_first_data(Page page)
{
	return (LionPageGetOpaque(page)->level > 0 && !LionPageIsRightmost(page)) ?
		OffsetNumberNext(FirstOffsetNumber) : FirstOffsetNumber;
}

static inline LionPostingPivot *
lion_posting_pivot(Page page, OffsetNumber off)
{
	return (LionPostingPivot *) PageGetItem(page, PageGetItemId(page, off));
}

/* The high key of a non-rightmost internal posting page. */
static inline LionPostingPivot *
lion_posting_highkey(Page page)
{
	Assert(LionPageGetOpaque(page)->level > 0 && !LionPageIsRightmost(page));
	return lion_posting_pivot(page, FirstOffsetNumber);
}

/* ---------- meta page ---------- */

#define LION_METAPAGE_BLKNO	0
#define LION_MAGIC			0x52424931	/* 'LION1' */
/*
 * Version 2 indexes NULL keys (DESIGN.md §14).  The page format did not
 * change - a version 1 index is structurally valid - but it holds no NULL
 * entry, so `IS NULL` would answer "no rows" instead of the truth.  Refusing
 * to open it is the only safe reading: REINDEX turns it into a version 2 one.
 *
 * Version 3 (DESIGN.md §18) adds owner_hash/owner_head to the page special
 * area, which makes the special area 24 bytes instead of 16 and therefore
 * moves every item on every page.  A version 2 index cannot be read at all by
 * this code, so opening one is the same ERROR with the same REINDEX hint.
 *
 * Version 4 (DESIGN.md §21) replaces the hash-bucket entry directory with a
 * B-tree keyed by the index key: the special area grows again (leftlink and
 * level), the meta page points at a root instead of counting buckets, and
 * entry tuples live on directory leaves in key order.  Nothing of a version 3
 * index is readable, so opening one is the same ERROR with the same hint.
 *
 * Version 5 (DESIGN.md §22) makes each key's posting set a B-tree over
 * container keys: the entry's `head` is its ROOT, which is an internal page
 * of LionPostingPivot downlinks as soon as the set outgrows one page.  The
 * page HEADER did not have to change (§21 had already given container pages a
 * `level`), but a version 4 posting set of more than one page is a flat
 * rightlinked chain with no root above it, which this code cannot descend and
 * therefore cannot write to.  §22 planned to share §21's version number
 * because the two were meant to land together; §21 shipped first, so the bump
 * is separate.  Opening a version 4 index is the same ERROR with the same
 * REINDEX hint.
 *
 * Version 6 (DESIGN.md §24) gives every entry tuple the KEY COLUMN it belongs
 * to, in four bytes the header had been padding out, and makes that column the
 * leading term of the directory order.  A version 5 index has zeros there, so
 * every one of its entries would read as column 0 and no descent would find
 * anything; reading such an index by TREATING attno 0 as 1 was considered and
 * rejected in §24, because the two formats would then be told apart by a field
 * that means "column one" in one of them and "no column at all" in the other.
 * Opening a version 5 index is the same ERROR with the same REINDEX hint.
 */
#define LION_VERSION			6

typedef struct LionMetaPageData
{
	uint32		magic;
	uint32		version;
	uint16		offset_bits;	/* LION_OFFSET_BITS at build time */
	uint16		container_bits; /* LION_CONTAINER_BITS at build time */
	uint32		unused_nbuckets;	/* always 0 since version 4 (§21) */
	uint32		inline_limit;	/* max inline payload bytes in an entry tuple */
	BlockNumber root;			/* root of the entry directory (§21) */
	uint32		height;			/* level of the root; 0 = the root is a leaf */
	uint32		dirpages;		/* directory pages, leaves and internal both */

	/*
	 * How this index is WAL-logged (DESIGN.md §25): LION_WAL_MODE_GENERIC or
	 * LION_WAL_MODE_RMGR.  It lives in what was reserved space, so the format
	 * version did not have to move: a version 6 index written before §25 has
	 * a zero here, which is exactly "generic", and reads and writes as it
	 * always did.  REINDEX is what changes an index's mode.
	 */
	uint32		wal_mode;

	/*
	 * THE ORDER OF THE DIRECTORY, as the build laid it out (DESIGN.md §21,
	 * "The order is the index's").  Whether a key column's entries are in its
	 * comparison's order is a property of the directory, fixed when the index
	 * is built; recomputing it from the catalog at every relcache build let a
	 * btree opclass created (or dropped) later flip an existing directory
	 * between hash order and value order, and every descent then read it in
	 * an order it is not in.
	 *
	 *	order_flags		LION_META_ORDER_RECORDED once the two words below
	 *					mean something.  Zero on an index built before they
	 *					existed, which is read the old way.
	 *	ordered_cols	bit i - 1: key column i is in its comparison's order.
	 *	order_ident		a hash of WHICH comparison each ordered column was
	 *					built with - the source it runs (lion_proc_ident()),
	 *					which survives pg_upgrade, a schema move and a rename
	 *					where an Oid or a name does not - so that the common
	 *					ways of replacing a comparison since are noticed
	 *					rather than used.  A best-effort guard, not a proof:
	 *					what the function CALLS is not in its source (§21).
	 *
	 * They live in what was reserved space, zero on every index written
	 * before, so the format version stays at 6, as it did for wal_mode.
	 */
	uint32		order_flags;
	uint32		ordered_cols;
	uint32		order_ident;
	uint32		reserved[2];	/* room for the next field to need none */
} LionMetaPageData;

#define LION_META_ORDER_RECORDED	0x0001

/* ordered_cols has one bit per key column. */
StaticAssertDecl(INDEX_MAX_KEYS <= 32,
				 "lion's meta page records the order of at most 32 key columns");

/*
 * §25 and §21's order record spent four of the reserved words, so the struct
 * is exactly the size it has been since version 4 and a meta page written by
 * either version reads as the other - which is what lets the format version
 * stay at 6.
 */
StaticAssertDecl(sizeof(LionMetaPageData) == 56,
				 "the lion meta page payload must not change size");

#define LionPageGetMeta(page)	((LionMetaPageData *) PageGetContents(page))

/* The first block after the meta page; where ambuild puts the first leaf. */
#define LION_FIRST_BLKNO		((BlockNumber) 1)

/* ---------- entry tuples (items on bucket pages) ---------- */

#define LION_ENTRY_INLINE	0x0001
#define LION_ENTRY_CHAIN		0x0002
#define LION_ENTRY_NULLKEY	0x0004	/* the reserved NULL-key entry, DESIGN.md §14 */
#define LION_ENTRY_EMPTYKEY	0x0008	/* the reserved no-key entry, DESIGN.md §17 */

/*
 * Pivot tuples (DESIGN.md §21).  A directory page's first item is its HIGH
 * KEY when the page is not the rightmost of its level, and every item of an
 * internal page is a DOWNLINK whose `head` is the child block.  Both are
 * LionEntryTuple-shaped so that the key helpers apply unchanged; they carry
 * neither INLINE nor CHAIN, and their ncontainers/ntids are zero.  A
 * downlink flagged MINUSINF is the leftmost child of its page and compares
 * below every key.
 */
#define LION_ENTRY_HIGHKEY	0x0010
#define LION_ENTRY_DOWNLINK	0x0020
#define LION_ENTRY_MINUSINF	0x0040

#define LION_ENTRY_RESERVED	(LION_ENTRY_NULLKEY | LION_ENTRY_EMPTYKEY)
#define LION_ENTRY_PIVOT		(LION_ENTRY_HIGHKEY | LION_ENTRY_DOWNLINK)
#define LION_ENTRY_ALLFLAGS \
	(LION_ENTRY_INLINE | LION_ENTRY_CHAIN | LION_ENTRY_RESERVED | \
	 LION_ENTRY_PIVOT | LION_ENTRY_MINUSINF)

#define LionEntryIsPivot(e)		(((e)->flags & LION_ENTRY_PIVOT) != 0)
#define LionEntryIsHighKey(e)	(((e)->flags & LION_ENTRY_HIGHKEY) != 0)
#define LionEntryIsDownlink(e)	(((e)->flags & LION_ENTRY_DOWNLINK) != 0)
#define LionEntryIsMinusInf(e)	(((e)->flags & LION_ENTRY_MINUSINF) != 0)

/*
 * Two entries per index have no key at all: keylen 0, hash 0, always in
 * bucket 0, and recognised by a flag rather than by comparing keys.
 *
 *	- LION_ENTRY_NULLKEY holds the rows whose indexed VALUE is NULL
 *	  (DESIGN.md §14).
 *	- LION_ENTRY_EMPTYKEY holds the rows a multi-key opclass extracted no key
 *	  from at all - an empty array, a tsvector with no lexemes (DESIGN.md
 *	  §17).  A scalar opclass never creates it.
 *
 * Everything that searches a bucket for a key must skip both, and everything
 * that reads a stored key must ask first.  They are reached only through
 * lion_find_reserved_entry() and its two wrappers.  A row is in exactly one of
 * them or under its own keys, never in two.
 */
#define LionEntryIsNullKey(e)	(((e)->flags & LION_ENTRY_NULLKEY) != 0)
#define LionEntryIsEmptyKey(e)	(((e)->flags & LION_ENTRY_EMPTYKEY) != 0)
#define LionEntryIsReserved(e)	(((e)->flags & LION_ENTRY_RESERVED) != 0)
#define LION_NULLKEY_HASH		0

/*
 * THE DIRECTORY ORDER (DESIGN.md §21, §24).
 *
 * Entries sort by (attno, kind, key, hash, stored bytes).  The KEY COLUMN
 * leads (§24): one relation holds every key column's entries, as independent
 * runs laid end to end, so a lookup is an (attno, key) descent and an entry
 * scan bounded to one column is the leaf walk from its first entry to the
 * first entry of the next column.  The leftmost downlink of an internal page
 * carries attno 0 and therefore sorts below every real entry whatever its
 * kind, which is what keeps the MINF rule of §21 intact.
 *
 * Within one column the kind puts the two reserved entries first - they have
 * no key to compare - and gives the leftmost downlink of an internal page a
 * value below every real one:
 *
 *		MINF < NULL < EMPTY < VALUE
 *
 * Within VALUE the order is the opclass's ordering support function (proc 4)
 * under the index collation, then the hash, then a bytewise comparison of the
 * stored datum.  The first three of those form the "prefix" comparison: two
 * entries whose prefixes tie are candidates for being the same key, and only
 * the opclass equality decides (citext's 'Alice' and 'alice' tie and ARE one
 * entry).  The bytewise tail exists so that the order is total even for a
 * type with no btree opclass at all, where the prefix is just (kind, hash).
 */
#define LION_KIND_MINF		0
#define LION_KIND_NULL		1
#define LION_KIND_EMPTY		2
#define LION_KIND_VALUE		3

typedef struct LionEntryTuple
{
	uint32		hash;
	uint16		flags;			/* LION_ENTRY_INLINE or LION_ENTRY_CHAIN,
								 * plus LION_ENTRY_NULLKEY for the null entry */
	uint16		keylen;			/* bytes of key data (0 for the null entry) */
	BlockNumber head;			/* CHAIN: first container page */
	BlockNumber tail;			/* CHAIN: last container page */
	uint32		ncontainers;	/* ITEMS: containers and sparse segments */
	uint16		attno;			/* 1-based KEY COLUMN (DESIGN.md §24); 0 only
								 * on a minus-infinity downlink */
	uint16		unused;			/* the rest of what was alignment padding */
	uint64		ntids;
	/* key data (keylen bytes), MAXALIGN padding, then inline items */
} LionEntryTuple;

#define LION_ENTRY_HDRSZ			(offsetof(LionEntryTuple, ntids) + sizeof(uint64))	/* 32 */

/*
 * DESIGN.md §24 spends the four bytes the header was padding out before
 * `ntids`, so the header is the same 32 bytes it has been since format 3 and
 * no item on any page moves.  Both halves of that are checked here rather
 * than trusted, because a compiler that laid the struct out differently would
 * make every existing index unreadable without a word of warning.
 */
StaticAssertDecl(offsetof(LionEntryTuple, attno) == 20,
				 "LionEntryTuple.attno must occupy the old header padding");
StaticAssertDecl(sizeof(LionEntryTuple) == 32,
				 "the lion entry header must stay 32 bytes");
#define LionEntryGetKey(e)		((char *) (e) + LION_ENTRY_HDRSZ)
#define LionEntryPayloadOffset(e) MAXALIGN(LION_ENTRY_HDRSZ + (e)->keylen)
#define LionEntryGetPayload(e)	((char *) (e) + LionEntryPayloadOffset(e))
/* payload length must be computed from the item size: itemsz - LionEntryPayloadOffset(e) */

#define LION_MAX_KEY_SIZE		2000
#define LION_DEFAULT_INLINE_LIMIT 4096
#define LION_MIN_INLINE_LIMIT	64
#define LION_MAX_INLINE_LIMIT	4096

/* The kind of an entry tuple, for the directory order above. */
static inline int
lion_entry_kind(const LionEntryTuple *e)
{
	if ((e->flags & LION_ENTRY_MINUSINF) != 0)
		return LION_KIND_MINF;
	if ((e->flags & LION_ENTRY_NULLKEY) != 0)
		return LION_KIND_NULL;
	if ((e->flags & LION_ENTRY_EMPTYKEY) != 0)
		return LION_KIND_EMPTY;
	return LION_KIND_VALUE;
}

/*
 * The largest PIVOT tuple (a high key or a downlink) the directory can hold,
 * and therefore what every split has to keep room for on both halves.
 */
#define LION_MAX_PIVOT_SIZE \
	(MAXALIGN(LION_ENTRY_HDRSZ + LION_MAX_KEY_SIZE) + sizeof(ItemIdData))

/*
 * The largest ENTRY tuple, which is smaller than the largest item a page can
 * hold: a leaf that holds one entry must still have room for the high key a
 * split would give it (DESIGN.md §21).  A payload that would exceed this
 * spills onto container pages instead, exactly as one that exceeds
 * inline_limit does.
 */
#define LION_MAX_ENTRY_SIZE \
	MAXALIGN_DOWN(LION_MAX_ITEM_SIZE - LION_MAX_PIVOT_SIZE - sizeof(ItemIdData))

/* ---------- reloptions ---------- */

#define LION_DEFAULT_FILLFACTOR	90
#define LION_MIN_FILLFACTOR		10

/*
 * How full ambuild packs an INTERNAL directory page, and the fewest downlinks
 * one may carry (DESIGN.md §21).
 *
 * `fillfactor` is about the LEAVES - it leaves room for the entries a later
 * INSERT grows or adds - and must not reach the internal levels: an internal
 * page that holds one downlink makes every level as large as the one below
 * it, so the bottom-up build never reaches a root at all.  nbtree keeps a
 * separate BTREE_NONLEAF_FILLFACTOR of 70 for exactly this reason, and a hard
 * minimum on top of it, because a page's fill target says nothing about how
 * large one item is: three 2000-byte pivots plus a high key do not fit a
 * block, so the floor that can always be honoured is two.
 */
#define LION_NONLEAF_FILLFACTOR	70
#define LION_MIN_DOWNLINKS		3
#define LION_ABS_MIN_DOWNLINKS	2

typedef struct LionOptions
{
	int32		vl_len_;		/* varlena header (do not touch directly) */
	int			buckets;		/* accepted and ignored since format 4 (§21) */
	int			inline_limit;
	int			max_entries;	/* 0 = unlimited (DESIGN.md §17) */
	int			fillfactor;		/* directory leaf fill at build time (§21) */
	int			wal_mode;		/* LION_WALOPT_*, DESIGN.md §25 */
} LionOptions;

/*
 * The `wal_mode` reloption (DESIGN.md §25).  AUTO is the default and means
 * "rmgr when this server registered the resource manager, generic when it did
 * not", which is what lets the same CREATE INDEX script work on a cluster
 * that preloads the library and on one that does not.  Asking for RMGR
 * explicitly on a server without it is an ERROR with the preload hint.
 */
#define LION_WALOPT_AUTO		0
#define LION_WALOPT_GENERIC		1
#define LION_WALOPT_RMGR		2

#define LION_DEFAULT_MAX_ENTRIES		0

/* ---------- per-relation cached state (rd_amcache) ---------- */

/*
 * DESIGN.md §24 made the cached state TWO structs, because an index now has
 * one key column per entry run and each of them has an opclass of its own:
 *
 *	LionState		everything about ONE key column - its key type, hash,
 *					equality, ordering and multi-key extraction.  Every
 *					function that looks a key up, builds an entry or compares
 *					two of them takes the state of the column it is working
 *					on, so the great majority of this extension did not have
 *					to change at all.
 *	LionIndexState	what belongs to the relation: the meta page (and with it
 *					the cached directory root) and the array of columns.
 *
 * `ix` is the column's way back to the relation, and `attno` is the column
 * number every entry tuple of that column carries.  lion_get_state() hands
 * back column 1, which is what a single-column index has and all a caller
 * that predates §24 ever means.
 */
typedef struct LionIndexState LionIndexState;

typedef struct LionState
{
	LionIndexState *ix;			/* the index this column belongs to; NULL in
								 * the throw-away states the cost model and
								 * the planner build for one extractQuery
								 * call */
	uint16		attno;			/* 1-based key column number (DESIGN.md §24) */

	/*
	 * The KEY type: what an entry tuple stores, hashes and compares.  It is
	 * the type of the index's own tuple descriptor column, which core has
	 * already resolved from the opclass: the indexed column's type for a
	 * scalar opclass, and the opclass STORAGE type - with a polymorphic
	 * anyelement replaced by the column's element type - for a multi-key one
	 * (ConstructTupleDescriptor() in catalog/index.c does that, so a
	 * roaring array_ops index on text[] has a text key column).
	 */
	Oid			typid;
	int16		typlen;
	bool		typbyval;
	char		typalign;
	Oid			collation;
	FmgrInfo	hashproc;		/* opclass support proc 1, or the key type's
								 * default hash opclass proc */
	FmgrInfo	eqproc;			/* strategy-1 operator's function, or the key
								 * type's default equality */

	/*
	 * ORDERING (DESIGN.md §21).  Support proc 4 is a btree comparison
	 * function for the KEY type; when the opclass has none - a polymorphic
	 * multi-key class - the key type's own default btree comparison is used
	 * instead.  `ordered` is false when the type has no btree opclass at all,
	 * and then the directory is ordered by (kind, hash) alone: still a
	 * well-defined tree, but its entry order is not the type's order, so
	 * lion_index_stats() reports ordered = false and the count pushdown
	 * claims no pathkeys for it.
	 *
	 * ltopr is the `<` operator of the same ordering, which ambuild's
	 * tuplesort needs; an ordering without one is treated as no ordering,
	 * because the build and the search must agree on the order exactly.
	 */
	bool		ordered;
	FmgrInfo	cmpproc;		/* valid iff ordered */
	Oid			ltopr;			/* valid iff ordered */

	/*
	 * The comparison has a SQL-standard (`RETURN`) body, which has no source
	 * the recorded order could recognise it by, so a BUILD lays this column
	 * out in hash order instead and says so (DESIGN.md §21).
	 */
	bool		sqlbodycmp;

	/*
	 * Multi-key opclasses (DESIGN.md §17): one indexed value yields many
	 * keys through GIN's extractValue, and a query yields keys and a mode
	 * through GIN's extractQuery.  multikey is false for every scalar
	 * opclass, and then neither FmgrInfo is valid.
	 */
	bool		multikey;
	FmgrInfo	extractvalue;	/* support proc 2 */
	FmgrInfo	extractquery;	/* support proc 3 */
} LionState;

struct LionIndexState
{
	LionMetaPageData meta;		/* copy of the meta page */
	int			ncolumns;		/* key columns, 1 .. INDEX_MAX_KEYS (§24) */
	LionState  *cols;			/* [ncolumns]; cols[i] is key column i + 1 */

	/*
	 * The backend's pg_proc invalidation count when the recorded order's
	 * comparisons were last checked against the catalog (§21): a cached
	 * state is checked again once a function has changed since.
	 */
	uint64		procgen;
};

/* The state of one key column of an index whose state is already in hand. */
static inline LionState *
lion_column(LionIndexState *ix, AttrNumber attno)
{
	Assert(attno >= 1 && attno <= ix->ncolumns);
	return &ix->cols[attno - 1];
}

/* ---------- multi-key extraction (DESIGN.md §17, lion_multikey.c) ---------- */

/*
 * Strategy numbers of the roaring AM.  1 is equality, which every scalar
 * opclass has; 2 .. 5 belong to the multi-key classes; 6 .. 9 are the range
 * comparisons of DESIGN.md §28, in btree's order, which an ORDERED scalar
 * class has beside its proc 4.
 */
#define LION_STRAT_EQUAL			1
#define LION_STRAT_CONTAINS		2	/* anyarray @> anyarray */
#define LION_STRAT_OVERLAP		3	/* anyarray && anyarray */
#define LION_STRAT_CONTAINED		4	/* anyarray <@ anyarray */
#define LION_STRAT_MATCH			5	/* tsvector @@ tsquery */
#define LION_STRAT_LT			6	/* key < value (§28) */
#define LION_STRAT_LE			7	/* key <= value */
#define LION_STRAT_GE			8	/* key >= value */
#define LION_STRAT_GT			9	/* key > value */
#define LION_NSTRATEGIES			9

#define LION_STRAT_IS_RANGE(s) \
	((s) >= LION_STRAT_LT && (s) <= LION_STRAT_GT)
/* a LOWER bound (>=, >) as opposed to an upper one (<, <=) */
#define LION_STRAT_IS_LOWER(s) \
	((s) == LION_STRAT_GE || (s) == LION_STRAT_GT)

/* Support procedure numbers. */
#define LION_HASH_PROC			1
#define LION_EXTRACTVALUE_PROC	2
#define LION_EXTRACTQUERY_PROC	3
#define LION_CMP_PROC			4	/* btree comparison of the KEY type (§21) */
#define LION_NPROC				4

/*
 * What a query over a multi-key index selects.
 *
 *	LION_QMODE_NONE	nothing at all (`tags && '{}'`)
 *	LION_QMODE_KEYS	exactly the rows the key tree selects; no recheck
 *	LION_QMODE_ALL	every indexed row, with recheck (`tags @> '{}'`, `<@`,
 *					a tsquery with NOT/phrase/prefix/weights, a NULL key)
 */
typedef enum LionQueryMode
{
	LION_QMODE_NONE = 0,
	LION_QMODE_KEYS,
	LION_QMODE_ALL
} LionQueryMode;

/*
 * A boolean tree over extracted keys.  A leaf names a key by its position in
 * the array the extraction produced; the count code reuses the very same
 * structure with keyno naming a located posting set instead, which is what
 * lets one evaluator serve both the bitmap scan and the count pushdown.
 */
typedef enum LionKeyNodeKind
{
	LION_KN_KEY = 0,
	LION_KN_AND,
	LION_KN_OR
} LionKeyNodeKind;

typedef struct LionKeyNode
{
	LionKeyNodeKind kind;
	int			keyno;			/* LION_KN_KEY only */
	int			nargs;
	struct LionKeyNode **args;
} LionKeyNode;

typedef struct LionQuery
{
	LionQueryMode mode;
	int			nkeys;			/* keys the extraction produced */
	Datum	   *keys;
	LionKeyNode *tree;			/* LION_QMODE_KEYS only; over keys[] */
} LionQuery;

/*
 * Extract the keys of one indexed value with support proc 2, drop the NULL
 * ones and the duplicates, and return them in *keys (palloc'd in the current
 * context) with the count as the return value.  Zero keys means the row
 * belongs in the reserved EMPTY entry.
 */
extern int lion_extract_value(LionState *state, Datum value, Datum **keys);

/*
 * Extract a query with support proc 3 and work out how its keys combine
 * (DESIGN.md §17).  Everything is palloc'd in the current context.
 */
extern void lion_extract_query(LionState *state, Datum query,
							  StrategyNumber strategy, LionQuery *q);


/* ---------- lion_pages.c: primitives shared by build/insert/scan/vacuum ---------- */

/* The whole cached state of an index, built on first use (DESIGN.md §24). */
extern LionIndexState *lion_get_index_state(Relation index);

/*
 * The state of one key column.  attno is the INDEX column number, 1-based, as
 * it appears in a ScanKey's sk_attno and in IndexOptInfo->indexkeys[i] + 1 -
 * never a heap attribute number.  ERRORs when the index has no such column.
 */
extern LionState *lion_index_column_state(Relation index, AttrNumber attno);

/* Column 1, which is all a single-column index has. */
extern LionState *lion_get_state(Relation index);

/* Key columns of an index, without building its state. */
#define lion_index_ncolumns(index) \
	((int) IndexRelationGetNumberOfKeyAttributes(index))

extern void lion_init_page(Page page, uint16 flags);

/*
 * A page for the index: a recycled one from the free space map when there is
 * one that is safe to take, else a brand new block (DESIGN.md §18).
 *
 * heaprel is what GlobalVisCheckRemovableFullXid() needs in order to decide
 * whether a DELETED page's safexid is old enough for the page to be reused;
 * NULL means "never recycle", which is what ambuildempty wants.  reuse =
 * false is the same thing for one call, and is how a chain's HEAD page is
 * allocated: head blocks must be unique for the life of the index, because
 * that is what makes owner_head a chain identity a reader can trust.
 *
 * The buffer comes back pinned and EXCLUSIVE with the page initialised.
 */
extern Buffer lion_new_buffer(Relation index, Relation heaprel, uint16 flags);

/*
 * Take a block for the index WITHOUT starting a record: a recycled one from
 * the free space map when one is safe to take, else a fresh one from
 * extending the relation.  The buffer comes back pinned and EXCLUSIVE with an
 * uninitialised page.
 *
 * This is the fallible half of allocating a page, and DESIGN.md §25 requires
 * it to happen BEFORE lion_wal_begin(): in rmgr mode the record is written
 * inside a critical section, where extending a relation would turn a full
 * disk into a PANIC.  Every split therefore works out how many pages it needs
 * from the page it already holds, takes them here, and only then opens its
 * record.
 */
extern Buffer lion_alloc_page(Relation index, Relation heaprel, bool reuse);

/* Give back a page taken by lion_alloc_page() and not used after all. */
extern void lion_release_unused_page(Relation index, Buffer buf);

extern void lion_init_metapage(Page page, uint32 inline_limit,
							  BlockNumber root, uint32 height,
							  uint32 dirpages, uint32 wal_mode);

/* Stamp a container page with the chain it belongs to (DESIGN.md §18). */
static inline void
lion_page_set_owner(Page page, uint32 hash, BlockNumber head)
{
	LionPageOpaque opaque = LionPageGetOpaque(page);

	opaque->owner_hash = hash;
	opaque->owner_head = head;
}

/*
 * Mark a container page free.  The page keeps its owner (verify() and the
 * leak sweep read it) but loses everything else; safexid is the transaction
 * id from which on nothing can still hold a link to it.
 */
extern void lion_page_set_deleted(Page page, FullTransactionId safexid);
extern FullTransactionId lion_page_get_safexid(Page page);
extern void lion_check_key_offset(ItemPointer tid);	/* ERROR if offset > LION_MAX_OFFSET */
extern bool lion_table_am_supported(Relation heap);	/* the heap AM only (lion_am.c) */
extern void lion_check_table_am(Relation heap);	/* ERROR if not supported */

/* Key handling */
extern uint32 lion_hash_key(LionState *state, Datum key);
extern Size lion_key_datum_size(LionState *state, Datum key);	/* bytes needed to store the key */
extern void lion_store_key(LionState *state, Datum key, char *dest);
extern Datum lion_fetch_key(LionState *state, const char *src);
extern bool lion_keys_equal(LionState *state, Datum a, Datum b);

/* ---------- lion_dir.c: the sorted entry directory (DESIGN.md §21) ---------- */

/*
 * What a descent compares stored entries against.
 *
 *	attno		the KEY COLUMN being searched (DESIGN.md §24), which is the
 *				leading term of the order
 *	col			that column's state: what fetches a stored key, and where a
 *				caller that did not override them took cmpproc/eqproc from.
 *				It may be the state of ANOTHER index's column only in the
 *				sense that the caller owns it; nothing here reads the meta
 *				page through it
 *	kind		LION_KIND_NULL / EMPTY / VALUE
 *	key, hash	the value being looked for, and its hash (the hash must be the
 *				one the index would have stored for it, which for a cross-type
 *				search is the SEARCH type's own hash function: a roaring
 *				opfamily's types hash compatibly)
 *	cmpproc		comparison of a STORED key against this one, stored key on the
 *				LEFT; NULL means the index has no ordering and the prefix is
 *				(kind, hash) alone
 *	eqproc		equality of a stored key against this one, stored key on the
 *				left; this and not the comparison is what decides that two
 *				keys are ONE entry
 *	raw/rawlen	the search value in its STORED form, when the caller has it.
 *				NULL makes the key compare as the smallest member of its own
 *				prefix run, which is what positions a lookup at the start of
 *				the run it then scans; a caller that wants the exact insert
 *				position supplies it.
 */
typedef struct LionSearchKey
{
	uint16		attno;
	LionState  *col;
	int			kind;
	Datum		key;
	uint32		hash;
	FmgrInfo   *cmpproc;
	FmgrInfo   *eqproc;
	Oid			collation;
	const char *raw;
	Size		rawlen;
} LionSearchKey;

/* A search key for a value of that column's own key type. */
extern void lion_search_key_init(LionState *state, LionSearchKey *sk,
								int kind, Datum key, uint32 hash);

/*
 * ... and one that positions exactly, from a built entry tuple.  The COLUMN
 * comes from the entry, which is why this takes the index state and not one
 * column's: a directory page holds entries of every column, and the repair of
 * an unfinished split builds this from whatever item the page happens to
 * carry (DESIGN.md §24).
 */
extern void lion_search_key_exact(LionIndexState *ix, LionSearchKey *sk,
								 const LionEntryTuple *entry);

/*
 * Compare a stored entry tuple with a search key.  <0: the item sorts first.
 * Everything the comparison needs is in the search key, the column state it
 * carries included, so an item of ANOTHER column is answered by the leading
 * attno term without touching a single opclass function.
 */
extern int	lion_cmp_entry(const LionEntryTuple *item, const LionSearchKey *sk);
/* The same, stopping before the bytewise tail (DESIGN.md §21). */
extern int	lion_cmp_prefix(const LionEntryTuple *item, const LionSearchKey *sk);
/* Compare two stored entry tuples in full; used by verify() and the build. */
extern int	lion_cmp_entries(LionIndexState *ix, const LionEntryTuple *a,
							 const LionEntryTuple *b);

/* The root block of the directory, refreshing the cached copy if need be. */
extern BlockNumber lion_dir_root(Relation index, LionIndexState *ix,
								uint32 *height);
/* The leftmost leaf, where an ordered walk of every entry starts. */
extern BlockNumber lion_dir_leftmost_leaf(Relation index, LionIndexState *ix);

/*
 * The leaf where key column `col`'s run of entries begins, and the offset of
 * its first entry on that leaf (DESIGN.md §24).  The run ends at the first
 * entry of the next column, which a walk recognises by its attno.
 */
extern BlockNumber lion_dir_column_first(Relation index, LionState *col,
										 OffsetNumber *offp);

/* The first data item of a directory page (offset 1, or 2 under a high key). */
static inline OffsetNumber
lion_page_first_data(Page page)
{
	return LionPageIsRightmost(page) ? FirstOffsetNumber :
		OffsetNumberNext(FirstOffsetNumber);
}

static inline LionEntryTuple *
lion_page_entry(Page page, OffsetNumber off)
{
	return (LionEntryTuple *) PageGetItem(page, PageGetItemId(page, off));
}

/*
 * Descend to the leaf that owns sk and return it locked in lockmode, with
 * *offp the offset of the first item that does not sort before sk.  With
 * forwrite, internal pages whose split never finished are repaired on the way
 * (DESIGN.md §21) and lockmode must be BUFFER_LOCK_EXCLUSIVE.
 */
extern Buffer lion_dir_search(Relation index, Relation heaprel,
							 LionIndexState *ix, const LionSearchKey *sk,
							 int lockmode, bool forwrite, OffsetNumber *offp);

/*
 * Locate the entry for sk.  On true *buf is a leaf locked in lockmode and
 * *offnum its offset.  On false *buf is still a locked leaf and *offnum is
 * the offset the entry would be inserted at, EXCEPT when *movedright says the
 * scan had to follow a right link.  With forwrite that leaf is then the one
 * the prefix run is entered from, held since the lookup so that no other
 * writer can create the key meanwhile, and *offnum is invalid:
 * lion_dir_add_entry() walks right from it to the exact position (DESIGN.md
 * §21).  movedright may be NULL.
 */
extern bool lion_dir_find(Relation index, Relation heaprel, LionIndexState *ix,
						 const LionSearchKey *sk, int lockmode, bool forwrite,
						 Buffer *buf, OffsetNumber *offnum, bool *movedright);

/* The pieces of the above, for a caller that walks the leaves itself. */
extern LionEntryTuple *lion_dir_highkey(Page page);
extern Buffer lion_dir_step_right(Relation index, Buffer buf, int lockmode);
extern OffsetNumber lion_dir_binsrch(Page page, const LionSearchKey *sk);
extern bool lion_dir_scan_run(Relation index, const LionSearchKey *sk,
							 int lockmode, Buffer *bufp, OffsetNumber *offp,
							 bool *movedright);

/*
 * Insert a brand new entry where lion_dir_find() with forwrite said it
 * belongs.  *bufp is held EXCLUSIVE and stays so; it is the caller's to
 * release.
 */
extern void lion_dir_add_entry(Relation index, Relation heaprel,
							  LionIndexState *ix, Buffer *bufp,
							  OffsetNumber off, bool movedright,
							  LionEntryTuple *entry, Size size);

/*
 * Put item at (buf, off) on a directory page the caller holds EXCLUSIVE,
 * replacing what is there or inserting before it, and splitting the page when
 * it does not fit.  buf stays locked and pinned; after a split the item may
 * live on the new right sibling instead.
 */
extern void lion_dir_place(Relation index, Relation heaprel, LionIndexState *ix,
						  Buffer buf, OffsetNumber off, bool replace,
						  LionEntryTuple *item, Size size);

/* Delete the items at the given ascending offsets in one record. */
extern void lion_dir_delete(Relation index, Buffer buf, OffsetNumber *offs,
						   int noffs);

/* Build an entry tuple in palloc'd memory; *size receives its total length. */
extern LionEntryTuple *lion_make_entry(LionState *state, Datum key, uint32 hash, uint16 flags,
									 const char *payload, Size payloadlen, Size *size);

/*
 * The same for a reserved entry (DESIGN.md §14 and §17): no key, hash 0, and
 * reservedflag - exactly one of LION_ENTRY_NULLKEY and LION_ENTRY_EMPTYKEY -
 * set on top of the caller's INLINE/CHAIN flag.  There is one of each PER KEY
 * COLUMN (DESIGN.md §24), which is why the column has to be named here.
 */
extern LionEntryTuple *lion_make_reserved_entry(AttrNumber attno,
											  uint16 reservedflag, uint16 flags,
											  const char *payload,
											  Size payloadlen, Size *size);

/*
 * Locate the entry for a real key (the caller supplies its hash), or one of
 * the two reserved key-less entries of DESIGN.md §14 and §17.  Both descend
 * the directory and hand back the leaf locked in lockmode; the caller
 * releases it.
 */
extern bool lion_find_entry(Relation index, LionState *state, int lockmode,
						   Datum key, uint32 hash, Buffer *buf,
						   OffsetNumber *offnum);

extern bool lion_find_reserved_entry(Relation index, LionState *state,
									int lockmode, uint16 reservedflag,
									Buffer *buf, OffsetNumber *offnum);

static inline bool
lion_find_null_entry(Relation index, LionState *state, int lockmode,
					Buffer *buf, OffsetNumber *offnum)
{
	return lion_find_reserved_entry(index, state, lockmode,
								   LION_ENTRY_NULLKEY, buf, offnum);
}

/*
 * Warn once per backend per index when an index has grown past its
 * max_entries reloption (DESIGN.md §17).  nentries is an estimate; the
 * warning never rejects a row.
 */
extern void lion_warn_max_entries(Relation index, int64 nentries);

/* The max_entries reloption of an index, 0 when unlimited. */
extern int lion_max_entries(Relation index);

/*
 * Replace the entry at (buf, offnum) with a new version (buf held EXCLUSIVE).
 * Handles size changes; if the new tuple cannot fit even after defragmenting
 * the page, returns false and changes nothing (caller must spill).
 * Caller supplies the open record if it wants the change batched with other
 * buffers (state may be NULL: then this function logs by itself).
 */
extern bool lion_replace_entry(Relation index, LionWalState *state, Buffer buf,
							  OffsetNumber offnum, LionEntryTuple *entry, Size size);

/* ---------- lion_posting.c: the per-key posting tree (DESIGN.md §22) ------- */

/*
 * Posting-tree navigation.  hash/head identify the set - head is its ROOT,
 * and the root block never changes for the life of the set, which is what
 * makes owner_head the identity DESIGN.md §18 relies on - and are checked
 * against every page the descent touches; a page that does not belong ends
 * the walk (the set was deleted after the caller copied the entry).
 *
 * lion_posting_search() hands back the LEAF that owns ckey, locked in
 * lockmode, or InvalidBuffer when the set is gone.  Internal pages are always
 * taken SHARE and released before the child is locked (no coupling: a split
 * holds the child while it locks the parent, so coupling downwards would close
 * a cycle buffer locks have no detector for - §21 makes the same argument for
 * the directory).  With forwrite the descent finishes every unfinished split
 * it meets and starts again, and lockmode must be BUFFER_LOCK_EXCLUSIVE.
 */
extern Buffer lion_posting_search(Relation index, Relation heaprel,
								  uint32 hash, BlockNumber head, uint32 ckey,
								  int lockmode, bool forwrite);

/* The leftmost leaf, where every sequential walk of a posting set starts. */
extern BlockNumber lion_posting_leftmost_leaf(Relation index, uint32 hash,
											  BlockNumber head);

/* The same, as DESIGN.md §4 named it; now a descent rather than a walk. */
extern BlockNumber lion_chain_find_page(Relation index, uint32 hash,
									   BlockNumber head, BlockNumber tail,
									   uint32 ckey);

/*
 * Turn the one-page posting set whose root is `buf` into a two-level tree
 * WITHOUT moving the root: its items go to a brand new child, and the root
 * block - which is the entry's `head` and the owner stamp of every page of
 * the set - becomes an internal page with one downlink.  The child comes back
 * pinned and still EXCLUSIVE-locked - it has never been unlocked since it was
 * allocated, so no reader can have copied the items it now holds before the
 * caller has finished with them (DESIGN.md §11, §22) - and the entry's `tail`
 * is updated in the same record.  Only `tail`: the record logs the entry as
 * it is on entrybuf, because the counters in the caller's copy describe items
 * that a LATER record places, and that record may never be written.  The
 * caller's copy gets the new `tail` as well.
 *
 * nbtree and GIN both keep the root in place on a root split, and §22 needs
 * it for a second reason: `head` is the set's identity, so an entry never has
 * to be rewritten for a root split and the four-buffer budget of a
 * GenericXLog record is never the binding constraint.
 */
extern Buffer lion_posting_root_pushdown(Relation index, Relation heaprel,
										 Buffer buf, Buffer entrybuf,
										 OffsetNumber entryoff,
										 LionEntryTuple *entry);

/*
 * Finish the split of the posting page pbuf, which the caller holds EXCLUSIVE
 * and keeps: put the downlink of its right sibling into the parent and clear
 * LION_PAGE_INCOMPLETE_SPLIT.  Idempotent, so a crash between the two records
 * costs nothing but the next writer's repair.
 */
extern void lion_posting_finish_split(Relation index, Relation heaprel,
									  uint32 hash, BlockNumber head,
									  Buffer pbuf);

/*
 * The same with the separator supplied.  A split knows it - it is the first
 * container key it put on the new sibling - and saying so spares a read of a
 * page the caller may still be holding EXCLUSIVE.  NULL is the repair case,
 * which has to work it out from the pages.
 */
extern void lion_posting_finish_split_sep(Relation index, Relation heaprel,
										  uint32 hash, BlockNumber head,
										  Buffer pbuf, const uint32 *knownsep);

/*
 * Locate a container by ckey on a container page.  Returns the offset of the
 * item whose own ckey is exactly ckey, or the offset where it should be
 * inserted with *found=false.  Only for callers that know they are looking
 * for a regular container (a segment's own ckey is its first one).
 */
extern OffsetNumber lion_page_find_container(Page page, uint32 ckey, bool *found);

/*
 * Locate the item that covers ckey on a container page: the container with
 * that ckey, or the sparse segment whose range [first ckey, last ckey]
 * contains it.  With *found=false the return value is the offset at which an
 * item for ckey would have to be inserted to keep the items ordered by first
 * ckey (DESIGN.md §13; item ranges never overlap or interleave).
 */
extern OffsetNumber lion_page_find_item(Page page, uint32 ckey, bool *found);

/*
 * Insert-or-replace a container in the chain of an entry, splitting pages as
 * needed (DESIGN.md §4).  The entry's bucket buffer (entrybuf, held EXCLUSIVE)
 * is updated when tail changes.  ncontainers_delta receives +1 for a new
 * container, 0 for a replacement.  All changes WAL-logged.
 */
extern void lion_chain_put_container(Relation index, Relation heaprel,
									Buffer entrybuf, OffsetNumber entryoff,
									LionEntryTuple *entry, LionContainer *c,
									int *ncontainers_delta);

/* Page-level min/max maintenance after any change of a container page's items. */
extern void lion_page_update_minmax(Page page);

/* Convenience: read the meta page once and validate it (used by lion_get_state). */
extern void lion_read_meta(Relation index, LionMetaPageData *meta);

/* ---------- AM entry points ---------- */

extern IndexBuildResult *lionbuild(Relation heap, Relation index, struct IndexInfo *indexInfo);
extern void lionbuildempty(Relation index);
extern bool lioninsert(Relation index, Datum *values, bool *isnull, ItemPointer ht_ctid,
					  Relation heapRel, IndexUniqueCheck checkUnique, bool indexUnchanged,
					  struct IndexInfo *indexInfo);
extern IndexBulkDeleteResult *lionbulkdelete(IndexVacuumInfo *info, IndexBulkDeleteResult *stats,
											IndexBulkDeleteCallback callback, void *callback_state);
extern IndexBulkDeleteResult *lionvacuumcleanup(IndexVacuumInfo *info, IndexBulkDeleteResult *stats);
extern IndexScanDesc lionbeginscan(Relation r, int nkeys, int norderbys);
extern void lionrescan(IndexScanDesc scan, ScanKey scankey, int nscankeys, ScanKey orderbys, int norderbys);
extern void lionendscan(IndexScanDesc scan);
extern int64 liongetbitmap(IndexScanDesc scan, TIDBitmap *tbm);
extern bool liongettuple(IndexScanDesc scan, ScanDirection dir);	/* DESIGN.md §29 */

/*
 * Container keys per window of a plain scan that reads a multi-key column
 * whole (DESIGN.md §29.3): 1024 container keys, 65536 heap blocks, at least.
 */
#define LION_UNION_MIN_WINDOW	1024
extern int	lion_union_window(void);
extern bytea *lionoptions(Datum reloptions, bool validate);
extern bool lionvalidate(Oid opclassoid);
extern void lioncostestimate(struct PlannerInfo *root, struct IndexPath *path, double loop_count,
							Cost *indexStartupCost, Cost *indexTotalCost, Selectivity *indexSelectivity,
							double *indexCorrelation, double *indexPages);

/*
 * Emit one item's members into a TIDBitmap (lion_scan.c), one tbm_add_tuples()
 * call per heap block; works for containers and sparse segments alike.
 * recheck is passed straight to tbm_add_tuples(): true when the scan is
 * emitting a superset of the matching TIDs and the heap scan has to apply the
 * original quals again.  Returns the number of members emitted.
 */
extern int64 lion_container_to_tbm(const LionContainer *c, TIDBitmap *tbm,
								  bool recheck);


/* ---------- additive helpers (wave 1, agent "am-core") ---------- */

/*
 * Largest item that can be placed on an otherwise empty lion index page.
 */
#define LION_MAX_ITEM_SIZE \
	MAXALIGN_DOWN(BLCKSZ - SizeOfPageHeaderData - LION_SPECIAL_SIZE - sizeof(ItemIdData))

/*
 * An INLINE entry payload holds containers back to back with no padding
 * (DESIGN.md section 4), so a container inside it is generally not aligned.
 * lion_inline_fetch() copies the container at byte offset *off into the
 * caller's aligned buffer (capacity LION_CONTAINER_MAX_SIZE), advances *off
 * past it, and returns its size; it returns 0 when the payload is exhausted.
 * Containers stored as page items are always MAXALIGNed by PageAddItem and
 * can be used in place.
 */
extern Size lion_inline_fetch(const char *payload, Size paylen, Size *off,
							 LionContainer *buf);

/* Length in bytes of an INLINE entry's payload, given the page item size. */
#define LION_ENTRY_PAYLOAD_LEN(e, itemsz)	((Size) (itemsz) - LionEntryPayloadOffset(e))

/*
 * Fill *ix - every key column of it - using the supplied meta page image.
 * ambuild uses this before the meta page exists; lion_get_index_state() uses
 * it afterwards.  The column array and the FmgrInfos are allocated in cxt.
 */
extern void lion_fill_index_state(Relation index, LionIndexState *ix,
								 const LionMetaPageData *meta,
								 MemoryContext cxt);
/* The meta page's wal_mode, remembered per relfilenode (§25; lion_pages.c). */
extern uint32 lion_index_meta_wal_mode(Relation index);

/* Record on a meta page image the order a build laid the directory out in (§21). */
extern void lion_meta_record_order(LionMetaPageData *meta, LionIndexState *ix);

/*
 * Like lion_find_entry(), but with the comparison functions the caller wants:
 * used by scans and by the count pushdown, where the search value may be of a
 * type that is only cross-type comparable with the indexed one.  Both procs
 * take the STORED key on the left; NULL means the index's own.  A cross-type
 * search whose opfamily has no cross-type ordering function (support proc 4)
 * cannot descend at all and falls back to a walk of every leaf, which is
 * correct and slow; the default opclasses of pg_lion all carry one.
 */
extern bool lion_find_entry_ext(Relation index, LionState *state, int lockmode,
							   Datum key, uint32 hash,
							   FmgrInfo *eqproc, FmgrInfo *cmpproc,
							   Oid collation,
							   Buffer *buf, OffsetNumber *offnum);

/*
 * May this transaction use index for a query under snapshot?  Requires
 * indisvalid and indisready, and honours indcheckxmin exactly as the planner
 * does in get_relation_info().  Callers that open an index by name (the SQL
 * count functions, the verifier) must ask, because nothing else checked for
 * them; *why receives the reason when the answer is false.
 */
extern bool lion_index_usable(Relation index, Snapshot snapshot,
							 const char **why);


/* ---------- additive helpers (wave 2, insert/vacuum/verify) ---------- */

/*
 * Bring a page that lion_alloc_page() took into the caller's open record: it
 * is registered as a page this record INITIALISES (a generic record logs a
 * full image, an rmgr record a PAGE_INIT operation and no image at all) and
 * initialised with `flags`.  Registering the new page in the record that
 * links it into a chain is what keeps a crash from leaving an initialised
 * page nothing points at.
 */
extern Page lion_wal_init_buffer(LionWalState *state, Buffer buf, uint16 flags);

/*
 * Log the page's special area as it now stands.  A caller that changes
 * rightlink, level, owner stamps, min/max or the page flags calls this once,
 * when they are final; in generic mode it does nothing, because the diff has
 * them already.
 */
extern void lion_wal_log_special(LionWalState *state, Page page);

/*
 * Private copy of an entry tuple (header, key and padding taken from entry,
 * which may point into a page) with a new payload.  A zero-length payload
 * gives the shape of a CHAIN entry.  The caller fills in flags, head, tail,
 * ncontainers and ntids.
 */
extern LionEntryTuple *lion_entry_rebuild(const LionEntryTuple *entry,
										const char *payload, Size payloadlen,
										Size *size);

/*
 * The same with the result ALLOCATED at allocsz bytes and the tail past the
 * payload zeroed: an INLINE entry's growth slack (DESIGN.md §4).  The zeroes
 * terminate the payload - lion_inline_fetch() stops at the first zero item
 * header - so nothing else has to know the slack is there.
 */
extern LionEntryTuple *lion_entry_rebuild_slack(const LionEntryTuple *entry,
												const char *payload,
												Size payloadlen, Size allocsz,
												Size *size);

/*
 * How many bytes to give an INLINE entry an INSERT is rewriting: its payload
 * plus growth slack by the rule of DESIGN.md §4, capped by inline_limit (so
 * that slack can never make a key spill) and by maxsize (so that it can never
 * be worth splitting a leaf for).
 */
extern Size lion_entry_alloc_size(Size payoff, Size paylen, Size inline_limit,
								  Size maxsize);

/*
 * The largest INLINE payload an entry whose payload starts at `payoff` may
 * carry: inline_limit, unless the key leaves less than that of
 * LION_MAX_ENTRY_SIZE.  A posting set past it spills onto container pages.
 * ambuild and INSERT both decide with this, so that every set an INSERT
 * could produce, a rebuild can write again.
 */
static inline Size
lion_inline_max(Size payoff, Size inline_limit)
{
	Assert(payoff < (Size) LION_MAX_ENTRY_SIZE);
	return Min(inline_limit, (Size) LION_MAX_ENTRY_SIZE - payoff);
}

/*
 * Like lion_chain_put_container(), but for a container page the caller has
 * already located and locked EXCLUSIVE (a cleanup lock counts); the buffer
 * stays locked.  The page must be the one that owns c->ckey.
 */
extern void lion_chain_put_container_locked(Relation index, Relation heaprel,
										   Buffer buf,
										   Buffer entrybuf, OffsetNumber entryoff,
										   LionEntryTuple *entry, LionContainer *c,
										   int *ncontainers_delta);

/*
 * The general form of the above: replace the item at off (replace = true) or
 * insert at off (replace = false) with nitems items, which must be in
 * ascending, non-overlapping ckey order and belong exactly where off points.
 * This is what lets an insert turn one sparse segment into (left segment,
 * container, right segment) atomically, in one WAL record, without the page
 * ever holding a ckey twice (DESIGN.md §13).
 *
 * buf is held EXCLUSIVE (a cleanup lock counts) and stays locked; entry is
 * the caller's private copy with its counters already updated, and travels in
 * the same record.  The page is split as in DESIGN.md §4 when the items do
 * not fit; the caller's items must together fit on an empty page.
 */
#define LION_MAX_PUT_ITEMS	3

extern void lion_chain_put_items_locked(Relation index, Relation heaprel,
									   Buffer buf,
									   Buffer entrybuf, OffsetNumber entryoff,
									   LionEntryTuple *entry,
									   OffsetNumber off, bool replace,
									   LionContainer **items, int nitems);

/*
 * Move an INLINE payload onto a chain of freshly allocated container pages
 * and rewrite the entry (a private copy in CHAIN shape, ncontainers/ntids
 * already set) as a CHAIN entry.  The entry page is held EXCLUSIVE by the
 * caller.
 */
extern void lion_entry_spill(Relation index, Relation heaprel, Buffer entrybuf,
							OffsetNumber entryoff, LionEntryTuple *entry,
							const char *payload, Size paylen);


/* ---------- additive helpers (wave 3, write path) ---------- */

/*
 * GROWTH SLACK INSIDE AN ITEM (DESIGN.md §4).
 *
 * An item on a container page may be allocated LARGER than the bytes its
 * header says it needs.  The spare bytes at its end are slack that the insert
 * path grows into: adding a member to an ARRAY or RUN container, or a pair to
 * a sparse segment, then happens by memmove INSIDE the item - under the same
 * lock, in the same WAL record as the entry tuple, without changing the
 * item's offset or its ItemIdGetLength() - so no other item on the page
 * moves and the GenericXLog delta is a handful of bytes instead of a page.
 *
 * The two sizes of an item are therefore different things, and both are
 * needed:
 *
 *	 lion_item_size(item)		its LOGICAL size, derived from the header;
 *								what every reader uses, and what the item
 *								would be written back as.
 *	 ItemIdGetLength(iid)		its ALLOCATED length on the page, which is
 *								what the item may grow to in place.
 *
 * Readers never look at the allocated length (they walk items by their
 * headers), so slack is invisible to them; code that copies an item out of a
 * page by ItemIdGetLength() - VACUUM does - still works because the allocated
 * length is capped at LION_CONTAINER_MAX_SIZE, the size of every item work
 * buffer.  Slack bytes are always zeroed when an item is written, so that
 * the page image stays deterministic and the WAL delta stays small.
 *
 * A bulk-built index gets no slack at all: it is read-mostly, and the space
 * would be pure loss.  Slack appears when an insert first grows an item.
 */
#define LION_ITEM_SLACK_MIN		8	/* smallest slack worth having */
#define LION_ITEM_SLACK_MAX		64	/* and the most, per item */
#define LION_ITEM_SLACK_FRACTION 8	/* size/8, clamped into the above */

/* The most bytes of slack an INSERT may leave (alignment padding included). */
#define LION_ITEM_SLACK_LIMIT	(LION_ITEM_SLACK_MAX + MAXIMUM_ALIGNOF - 1)

/*
 * SHRINKING AN ITEM IN PLACE (DESIGN.md §18).
 *
 * VACUUM removes members from an item and writes it back.  Writing it back at
 * its new, smaller size would make PageIndexTupleOverwrite() move every item
 * after it on the page, and a GenericXLog delta is a byte-wise diff of the
 * page image: the record would then carry the whole tail of the page instead
 * of the handful of bytes the removal really changed.  So a filtered item
 * KEEPS the number of bytes the page has allotted it and the freed bytes
 * become slack, exactly like the growth slack an insert leaves behind - which
 * the next insert into that item then grows into for free.
 *
 * The bound is larger than an insert's because a filter can free more than a
 * growth adds (a 1% deletion takes ~40 bytes out of a full ARRAY container),
 * and it is finite because unbounded slack would be indistinguishable from
 * corruption and would waste the page: an item that has lost more than this
 * is compacted, paying the page-tail delta once.
 */
#define LION_ITEM_SLACK_VACUUM	256

/* The most slack any item may hold, and what verify() accepts. */
#define LION_ITEM_SLACK_BOUND \
	((LION_ITEM_SLACK_LIMIT > LION_ITEM_SLACK_VACUUM) ? \
	 LION_ITEM_SLACK_LIMIT : LION_ITEM_SLACK_VACUUM)

/*
 * The same for the INLINE payload of an entry tuple on a bucket page: a
 * filtered payload is written back into the bytes the entry already has, with
 * the remainder zeroed.  lion_inline_fetch() stops at the first zero item
 * header, which no real item can have (type 0 is not a container kind), so
 * the trailing zeroes are self-describing and no length field is needed.
 */
#define LION_ENTRY_SLACK_BOUND	256

/*
 * Allocated length to write an item of logical size `size` with, so that it
 * has room to grow in place.  Always >= size, always MAXALIGNed and never
 * above LION_CONTAINER_MAX_SIZE; a BITSET container, which is already the
 * largest an item can be, gets its exact size.  The caller checks that the
 * result fits on the page and falls back to `size` if it does not.
 */
extern Size lion_item_alloc_size(const LionContainer *item, Size size);

/* Free bytes inside an item of allocated length itemlen. */
static inline Size
lion_item_slack(const LionContainer *item, Size itemlen)
{
	Size		size = lion_item_size(item);

	return (itemlen > size) ? itemlen - size : 0;
}

/*
 * lion_chain_put_items_locked() and lion_chain_put_container_locked() with
 * control over slack: with slack = true the items are written with room to
 * grow (and the items[] buffers must have LION_CONTAINER_MAX_SIZE bytes of
 * capacity, because their slack is zeroed in place before the copy).  The
 * two original functions are these with slack = false, which is what VACUUM
 * and the build want: they write items at their exact size.
 */
extern void lion_chain_put_items_locked_ext(Relation index, Relation heaprel,
										   Buffer buf,
										   Buffer entrybuf, OffsetNumber entryoff,
										   LionEntryTuple *entry,
										   OffsetNumber off, bool replace,
										   LionContainer **items, int nitems,
										   bool slack);

extern void lion_chain_put_container_locked_ext(Relation index, Relation heaprel,
											   Buffer buf,
											   Buffer entrybuf, OffsetNumber entryoff,
											   LionEntryTuple *entry, LionContainer *c,
											   int *ncontainers_delta, bool slack);

/*
 * The number of DIRECTORY PAGES the current backend has read: the counter
 * behind EXPLAIN ANALYZE's "Directory Pages Read" (DESIGN.md §21).  It is a
 * plain process-local counter because every read of a leaf or an internal
 * page goes through one of a handful of places, and the count is only ever
 * used as a difference over a node's execution.
 */
extern PGDLLIMPORT int64 lion_dir_pages_read;

#endif							/* LION_H */
