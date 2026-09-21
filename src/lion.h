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

#include "access/amapi.h"
#include "access/genam.h"
#include "access/generic_xlog.h"
#include "access/itup.h"
#include "fmgr.h"
#include "nodes/tidbitmap.h"
#include "storage/bufmgr.h"
#include "storage/bufpage.h"
#include "utils/relcache.h"

#include "lion_tid.h"
#include "lion_container.h"
#include "lion_sparse.h"

/* ---------- page special area ---------- */

#define LION_PAGE_ID			0xFF87

#define LION_PAGE_META		0x0001
#define LION_PAGE_BUCKET		0x0002
#define LION_PAGE_CONTAINER	0x0004

typedef struct LionPageOpaqueData
{
	BlockNumber rightlink;		/* next page in chain or InvalidBlockNumber */
	uint32		minckey;		/* container pages only; 0 when empty */
	uint32		maxckey;		/* container pages only; 0 when empty */
	uint16		flags;			/* LION_PAGE_* */
	uint16		page_id;		/* LION_PAGE_ID */
} LionPageOpaqueData;

typedef LionPageOpaqueData *LionPageOpaque;

#define LionPageGetOpaque(page)	((LionPageOpaque) PageGetSpecialPointer(page))
#define LION_SPECIAL_SIZE		MAXALIGN(sizeof(LionPageOpaqueData))
#define LionPageIsMeta(page)		((LionPageGetOpaque(page)->flags & LION_PAGE_META) != 0)
#define LionPageIsBucket(page)	((LionPageGetOpaque(page)->flags & LION_PAGE_BUCKET) != 0)
#define LionPageIsContainer(page) ((LionPageGetOpaque(page)->flags & LION_PAGE_CONTAINER) != 0)

/* ---------- meta page ---------- */

#define LION_METAPAGE_BLKNO	0
#define LION_MAGIC			0x52424931	/* 'LION1' */
/*
 * Version 2 indexes NULL keys (DESIGN.md §14).  The page format did not
 * change - a version 1 index is structurally valid - but it holds no NULL
 * entry, so `IS NULL` would answer "no rows" instead of the truth.  Refusing
 * to open it is the only safe reading: REINDEX turns it into a version 2 one.
 */
#define LION_VERSION			2

typedef struct LionMetaPageData
{
	uint32		magic;
	uint32		version;
	uint16		offset_bits;	/* LION_OFFSET_BITS at build time */
	uint16		container_bits; /* LION_CONTAINER_BITS at build time */
	uint32		nbuckets;		/* any count in 1 .. LION_MAX_BUCKETS */
	uint32		inline_limit;	/* max inline payload bytes in an entry tuple */
	uint32		reserved[9];	/* pad to 64 bytes */
} LionMetaPageData;

#define LionPageGetMeta(page)	((LionMetaPageData *) PageGetContents(page))
#define LION_BUCKET_BLKNO(b)		((BlockNumber) (1 + (b)))
#define LION_MAX_BUCKETS			65536
#define LION_DEFAULT_BUCKETS		64	/* used by ambuildempty / when no data */

/*
 * The bucket a key hash belongs to.  Bucket counts are NOT powers of two:
 * ambuild sizes them from the bytes the entries are expected to need
 * (DESIGN.md §4 and §5), and the `buckets` reloption means an exact count.
 * Every module that maps a hash to a bucket goes through this one helper, so
 * that the mapping cannot drift apart between build, insert, scan, count and
 * verify.
 */
static inline uint32
lion_bucket_of(uint32 hash, uint32 nbuckets)
{
	Assert(nbuckets > 0);
	return hash % nbuckets;
}

/* ---------- entry tuples (items on bucket pages) ---------- */

#define LION_ENTRY_INLINE	0x0001
#define LION_ENTRY_CHAIN		0x0002
#define LION_ENTRY_NULLKEY	0x0004	/* the reserved NULL-key entry, DESIGN.md §14 */
#define LION_ENTRY_EMPTYKEY	0x0008	/* the reserved no-key entry, DESIGN.md §17 */

#define LION_ENTRY_RESERVED	(LION_ENTRY_NULLKEY | LION_ENTRY_EMPTYKEY)

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
#define LION_NULLKEY_BUCKET		0

typedef struct LionEntryTuple
{
	uint32		hash;
	uint16		flags;			/* LION_ENTRY_INLINE or LION_ENTRY_CHAIN,
								 * plus LION_ENTRY_NULLKEY for the null entry */
	uint16		keylen;			/* bytes of key data (0 for the null entry) */
	BlockNumber head;			/* CHAIN: first container page */
	BlockNumber tail;			/* CHAIN: last container page */
	uint32		ncontainers;	/* ITEMS: containers and sparse segments */
	uint64		ntids;
	/* key data (keylen bytes), MAXALIGN padding, then inline items */
} LionEntryTuple;

#define LION_ENTRY_HDRSZ			(offsetof(LionEntryTuple, ntids) + sizeof(uint64))	/* 32 */
#define LionEntryGetKey(e)		((char *) (e) + LION_ENTRY_HDRSZ)
#define LionEntryPayloadOffset(e) MAXALIGN(LION_ENTRY_HDRSZ + (e)->keylen)
#define LionEntryGetPayload(e)	((char *) (e) + LionEntryPayloadOffset(e))
/* payload length must be computed from the item size: itemsz - LionEntryPayloadOffset(e) */

#define LION_MAX_KEY_SIZE		2000
#define LION_DEFAULT_INLINE_LIMIT 4096
#define LION_MIN_INLINE_LIMIT	64
#define LION_MAX_INLINE_LIMIT	4096

/* ---------- reloptions ---------- */

typedef struct LionOptions
{
	int32		vl_len_;		/* varlena header (do not touch directly) */
	int			buckets;		/* 0 = auto */
	int			inline_limit;
	int			max_entries;	/* 0 = unlimited (DESIGN.md §17) */
} LionOptions;

#define LION_DEFAULT_MAX_ENTRIES		0

/* ---------- per-relation cached state (rd_amcache) ---------- */

typedef struct LionState
{
	LionMetaPageData meta;		/* copy of the meta page */

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
	 * Multi-key opclasses (DESIGN.md §17): one indexed value yields many
	 * keys through GIN's extractValue, and a query yields keys and a mode
	 * through GIN's extractQuery.  multikey is false for every scalar
	 * opclass, and then neither FmgrInfo is valid.
	 */
	bool		multikey;
	FmgrInfo	extractvalue;	/* support proc 2 */
	FmgrInfo	extractquery;	/* support proc 3 */
} LionState;

/* ---------- multi-key extraction (DESIGN.md §17, lion_multikey.c) ---------- */

/*
 * Strategy numbers of the roaring AM.  1 is the only one a scalar opclass
 * has; 2 .. 5 belong to the multi-key classes.
 */
#define LION_STRAT_EQUAL			1
#define LION_STRAT_CONTAINS		2	/* anyarray @> anyarray */
#define LION_STRAT_OVERLAP		3	/* anyarray && anyarray */
#define LION_STRAT_CONTAINED		4	/* anyarray <@ anyarray */
#define LION_STRAT_MATCH			5	/* tsvector @@ tsquery */
#define LION_NSTRATEGIES			5

/* Support procedure numbers. */
#define LION_HASH_PROC			1
#define LION_EXTRACTVALUE_PROC	2
#define LION_EXTRACTQUERY_PROC	3
#define LION_NPROC				3

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

extern LionState *lion_get_state(Relation index);
extern void lion_init_page(Page page, uint16 flags);
extern Buffer lion_new_buffer(Relation index, uint16 flags);	/* extends rel; returns pinned+X-locked, page initialised */
extern void lion_init_metapage(Page page, uint32 nbuckets, uint32 inline_limit);
extern void lion_check_key_offset(ItemPointer tid);	/* ERROR if offset > LION_MAX_OFFSET */

/* Key handling */
extern uint32 lion_hash_key(LionState *state, Datum key);
extern Size lion_key_datum_size(LionState *state, Datum key);	/* bytes needed to store the key */
extern void lion_store_key(LionState *state, Datum key, char *dest);
extern Datum lion_fetch_key(LionState *state, const char *src);
extern bool lion_keys_equal(LionState *state, Datum a, Datum b);

/*
 * Entry lookup in bucket chain.  Caller holds the bucket head buffer locked
 * (SHARE for readers, EXCLUSIVE for writers).  On success returns true and
 * sets *buf (pinned and locked with the caller's lock mode; may be the head
 * buffer itself, in which case no extra pin is taken) and *offnum.
 */
extern bool lion_find_entry(Relation index, LionState *state, Buffer headbuf, int lockmode,
						   Datum key, uint32 hash, Buffer *buf, OffsetNumber *offnum);

/* Build an entry tuple in palloc'd memory; *size receives its total length. */
extern LionEntryTuple *lion_make_entry(LionState *state, Datum key, uint32 hash, uint16 flags,
									 const char *payload, Size payloadlen, Size *size);

/*
 * The same for a reserved entry (DESIGN.md §14 and §17): no key, hash 0, and
 * reservedflag - exactly one of LION_ENTRY_NULLKEY and LION_ENTRY_EMPTYKEY -
 * set on top of the caller's INLINE/CHAIN flag.
 */
extern LionEntryTuple *lion_make_reserved_entry(uint16 reservedflag, uint16 flags,
											  const char *payload,
											  Size payloadlen, Size *size);

/*
 * Find a reserved entry in bucket 0, whose head page the caller holds locked
 * in lockmode (and keeps locked).  The buf and offnum outputs work exactly as
 * they do for lion_find_entry().
 */
extern bool lion_find_reserved_entry(Relation index, Buffer headbuf,
									int lockmode, uint16 reservedflag,
									Buffer *buf, OffsetNumber *offnum);

static inline bool
lion_find_null_entry(Relation index, Buffer headbuf, int lockmode,
					Buffer *buf, OffsetNumber *offnum)
{
	return lion_find_reserved_entry(index, headbuf, lockmode,
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
 * Number of entries on the bucket chain starting at headbuf, which the caller
 * holds locked.  Only the cardinality guard calls this.
 */
extern int64 lion_bucket_nentries(Relation index, Buffer headbuf);

/*
 * Add a new entry to the bucket chain starting at headbuf (held EXCLUSIVE).
 * Appends a bucket page if needed.  WAL-logs via GenericXLog internally.
 */
extern void lion_add_entry(Relation index, Buffer headbuf, LionEntryTuple *entry, Size size);

/*
 * Replace the entry at (buf, offnum) with a new version (buf held EXCLUSIVE).
 * Handles size changes; if the new tuple cannot fit even after defragmenting
 * the page, returns false and changes nothing (caller must spill).
 * Caller supplies the GenericXLogState if it wants the change batched with
 * other buffers (state may be NULL: then this function logs by itself).
 */
extern bool lion_replace_entry(Relation index, GenericXLogState *state, Buffer buf,
							  OffsetNumber offnum, LionEntryTuple *entry, Size size);

/* Container chain navigation */
extern BlockNumber lion_chain_find_page(Relation index, BlockNumber head, BlockNumber tail, uint32 ckey);

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
extern void lion_chain_put_container(Relation index, Buffer entrybuf, OffsetNumber entryoff,
									LionEntryTuple *entry, LionContainer *c, int *ncontainers_delta);

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
 * Fill *state for index, using the supplied meta page image.  ambuild uses
 * this before the meta page exists; lion_get_state() uses it afterwards.
 * FmgrInfos are allocated in cxt.
 */
extern void lion_fill_state(Relation index, LionState *state,
						   const LionMetaPageData *meta, MemoryContext cxt);

/*
 * Like lion_find_entry(), but compares the stored key against key with the
 * caller-supplied equality function (used by scans, where the scan key may be
 * of a type that is only cross-type equal to the indexed type).  eqproc NULL
 * means "use state->eqproc with state->collation".  The stored key is always
 * the left-hand argument.
 */
extern bool lion_find_entry_ext(Relation index, LionState *state, Buffer headbuf,
							   int lockmode, Datum key, uint32 hash,
							   FmgrInfo *eqproc, Oid collation,
							   Buffer *buf, OffsetNumber *offnum);

/* Clamp a requested bucket count into [1, LION_MAX_BUCKETS]; no rounding. */
extern uint32 lion_clamp_buckets(int64 nbuckets);

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
 * Extend the index by one page inside the caller's GenericXLog record (the
 * page initialisation is logged as a full image).  The buffer comes back
 * pinned and EXCLUSIVE; *pagep, if not NULL, receives the registered image.
 * Allocating a page in the record that links it into a chain is what keeps a
 * crash from leaving an initialised page nothing points at.
 */
extern Buffer lion_new_buffer_xl(Relation index, GenericXLogState *xstate,
								uint16 flags, Page *pagep);

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
 * Like lion_chain_put_container(), but for a container page the caller has
 * already located and locked EXCLUSIVE (a cleanup lock counts); the buffer
 * stays locked.  The page must be the one that owns c->ckey.
 */
extern void lion_chain_put_container_locked(Relation index, Buffer buf,
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

extern void lion_chain_put_items_locked(Relation index, Buffer buf,
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
extern void lion_entry_spill(Relation index, Buffer entrybuf, OffsetNumber entryoff,
							LionEntryTuple *entry, const char *payload, Size paylen);


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

/* The most bytes of slack an item may hold (alignment padding included). */
#define LION_ITEM_SLACK_LIMIT	(LION_ITEM_SLACK_MAX + MAXIMUM_ALIGNOF - 1)

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
extern void lion_chain_put_items_locked_ext(Relation index, Buffer buf,
										   Buffer entrybuf, OffsetNumber entryoff,
										   LionEntryTuple *entry,
										   OffsetNumber off, bool replace,
										   LionContainer **items, int nitems,
										   bool slack);

extern void lion_chain_put_container_locked_ext(Relation index, Buffer buf,
											   Buffer entrybuf, OffsetNumber entryoff,
											   LionEntryTuple *entry, LionContainer *c,
											   int *ncontainers_delta, bool slack);

/*
 * THE BUCKET DIRECTORY GUARD (DESIGN.md §5).
 *
 * The number of buckets is chosen once, by ambuild, and never changes: there
 * is no online directory growth in v1.  An index created on an empty table
 * and filled afterwards therefore keeps LION_DEFAULT_BUCKETS buckets however
 * large it grows, and absorbs everything in bucket page chains - which every
 * lookup of a key has to walk.
 *
 * Inserts watch for that: when the chain of the bucket an insert walks is
 * longer than LION_BUCKET_PAGES_WARN pages, that bucket holds several times
 * the entry bytes ambuild sizes a bucket for (three quarters of a page), and
 * the backend says so once per index.  Like the cardinality guard of §17 this
 * is advisory - the index keeps working and keeps taking rows - and like it,
 * the estimate is one bucket's chain rather than the average over all
 * buckets, because hashes spread entries evenly enough and counting the whole
 * directory on every insert would cost more than it is worth.  An index whose
 * `buckets` reloption was set explicitly is never warned about: that count is
 * what its owner asked for.
 *
 * The threshold is four pages and not the two that "twice what ambuild aims
 * at" would suggest, because a MAXIMUM is being compared against an average:
 * hash skew and ambuild's own byte estimate (which is about 30% low for keys
 * whose TIDs spread thinly over container keys, DESIGN.md §13) leave a
 * freshly built, correctly sized index with three-page buckets - measured on
 * the 20000-key column of the bench/write_micro.sh portfolio.  An index that
 * really has outgrown its directory is far past this: 100k keys in an index
 * created empty give 64 buckets of twelve pages each.
 *
 * lion_index_stats() reports the same thing exactly, as max_bucket_pages.
 */
#define LION_BUCKET_PAGES_WARN	4

extern void lion_warn_bucket_chain(Relation index, int npages);

/*
 * Like lion_find_entry_ext(), but also counts the bucket pages the walk
 * visited (npages may be NULL).  A walk that finds its entry stops there, so
 * the count is a lower bound on the length of the chain - which is what the
 * bucket directory guard wants: it never warns about a chain it has not
 * actually walked.
 */
extern bool lion_find_entry_counted(Relation index, LionState *state,
								   Buffer headbuf, int lockmode, Datum key,
								   uint32 hash, FmgrInfo *eqproc, Oid collation,
								   Buffer *buf, OffsetNumber *offnum,
								   int *npages);

/* The same for the reserved NULL/EMPTY entries (DESIGN.md §14, §17). */
extern bool lion_find_reserved_entry_counted(Relation index, Buffer headbuf,
											int lockmode, uint16 reservedflag,
											Buffer *buf, OffsetNumber *offnum,
											int *npages);

/* Length of the bucket chain starting at headbuf, which the caller holds
 * locked; used by lion_index_stats(). */
extern int lion_bucket_npages(Relation index, Buffer headbuf);

#endif							/* LION_H */
