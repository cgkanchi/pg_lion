/*-------------------------------------------------------------------------
 * rbi.h
 *	  On-disk structures and internal API of the roaring index AM.
 *	  See DESIGN.md §4-§7.  The structs in this file are the on-disk format
 *	  and are fixed; the prototypes are the contract between modules.
 *-------------------------------------------------------------------------
 */
#ifndef RBI_H
#define RBI_H

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

#include "rbi_tid.h"
#include "rbi_container.h"
#include "rbi_sparse.h"

/* ---------- page special area ---------- */

#define RBI_PAGE_ID			0xFF87

#define RBI_PAGE_META		0x0001
#define RBI_PAGE_BUCKET		0x0002
#define RBI_PAGE_CONTAINER	0x0004

typedef struct RBIPageOpaqueData
{
	BlockNumber rightlink;		/* next page in chain or InvalidBlockNumber */
	uint32		minckey;		/* container pages only; 0 when empty */
	uint32		maxckey;		/* container pages only; 0 when empty */
	uint16		flags;			/* RBI_PAGE_* */
	uint16		page_id;		/* RBI_PAGE_ID */
} RBIPageOpaqueData;

typedef RBIPageOpaqueData *RBIPageOpaque;

#define RBIPageGetOpaque(page)	((RBIPageOpaque) PageGetSpecialPointer(page))
#define RBI_SPECIAL_SIZE		MAXALIGN(sizeof(RBIPageOpaqueData))
#define RBIPageIsMeta(page)		((RBIPageGetOpaque(page)->flags & RBI_PAGE_META) != 0)
#define RBIPageIsBucket(page)	((RBIPageGetOpaque(page)->flags & RBI_PAGE_BUCKET) != 0)
#define RBIPageIsContainer(page) ((RBIPageGetOpaque(page)->flags & RBI_PAGE_CONTAINER) != 0)

/* ---------- meta page ---------- */

#define RBI_METAPAGE_BLKNO	0
#define RBI_MAGIC			0x52424931	/* 'RBI1' */
/*
 * Version 2 indexes NULL keys (DESIGN.md §14).  The page format did not
 * change - a version 1 index is structurally valid - but it holds no NULL
 * entry, so `IS NULL` would answer "no rows" instead of the truth.  Refusing
 * to open it is the only safe reading: REINDEX turns it into a version 2 one.
 */
#define RBI_VERSION			2

typedef struct RBIMetaPageData
{
	uint32		magic;
	uint32		version;
	uint16		offset_bits;	/* RBI_OFFSET_BITS at build time */
	uint16		container_bits; /* RBI_CONTAINER_BITS at build time */
	uint32		nbuckets;		/* any count in 1 .. RBI_MAX_BUCKETS */
	uint32		inline_limit;	/* max inline payload bytes in an entry tuple */
	uint32		reserved[9];	/* pad to 64 bytes */
} RBIMetaPageData;

#define RBIPageGetMeta(page)	((RBIMetaPageData *) PageGetContents(page))
#define RBI_BUCKET_BLKNO(b)		((BlockNumber) (1 + (b)))
#define RBI_MAX_BUCKETS			65536
#define RBI_DEFAULT_BUCKETS		64	/* used by ambuildempty / when no data */

/*
 * The bucket a key hash belongs to.  Bucket counts are NOT powers of two:
 * ambuild sizes them from the bytes the entries are expected to need
 * (DESIGN.md §4 and §5), and the `buckets` reloption means an exact count.
 * Every module that maps a hash to a bucket goes through this one helper, so
 * that the mapping cannot drift apart between build, insert, scan, count and
 * verify.
 */
static inline uint32
rbi_bucket_of(uint32 hash, uint32 nbuckets)
{
	Assert(nbuckets > 0);
	return hash % nbuckets;
}

/* ---------- entry tuples (items on bucket pages) ---------- */

#define RBI_ENTRY_INLINE	0x0001
#define RBI_ENTRY_CHAIN		0x0002
#define RBI_ENTRY_NULLKEY	0x0004	/* the reserved NULL-key entry, DESIGN.md §14 */
#define RBI_ENTRY_EMPTYKEY	0x0008	/* the reserved no-key entry, DESIGN.md §17 */

#define RBI_ENTRY_RESERVED	(RBI_ENTRY_NULLKEY | RBI_ENTRY_EMPTYKEY)

/*
 * Two entries per index have no key at all: keylen 0, hash 0, always in
 * bucket 0, and recognised by a flag rather than by comparing keys.
 *
 *	- RBI_ENTRY_NULLKEY holds the rows whose indexed VALUE is NULL
 *	  (DESIGN.md §14).
 *	- RBI_ENTRY_EMPTYKEY holds the rows a multi-key opclass extracted no key
 *	  from at all - an empty array, a tsvector with no lexemes (DESIGN.md
 *	  §17).  A scalar opclass never creates it.
 *
 * Everything that searches a bucket for a key must skip both, and everything
 * that reads a stored key must ask first.  They are reached only through
 * rbi_find_reserved_entry() and its two wrappers.  A row is in exactly one of
 * them or under its own keys, never in two.
 */
#define RBIEntryIsNullKey(e)	(((e)->flags & RBI_ENTRY_NULLKEY) != 0)
#define RBIEntryIsEmptyKey(e)	(((e)->flags & RBI_ENTRY_EMPTYKEY) != 0)
#define RBIEntryIsReserved(e)	(((e)->flags & RBI_ENTRY_RESERVED) != 0)
#define RBI_NULLKEY_HASH		0
#define RBI_NULLKEY_BUCKET		0

typedef struct RBIEntryTuple
{
	uint32		hash;
	uint16		flags;			/* RBI_ENTRY_INLINE or RBI_ENTRY_CHAIN,
								 * plus RBI_ENTRY_NULLKEY for the null entry */
	uint16		keylen;			/* bytes of key data (0 for the null entry) */
	BlockNumber head;			/* CHAIN: first container page */
	BlockNumber tail;			/* CHAIN: last container page */
	uint32		ncontainers;	/* ITEMS: containers and sparse segments */
	uint64		ntids;
	/* key data (keylen bytes), MAXALIGN padding, then inline items */
} RBIEntryTuple;

#define RBI_ENTRY_HDRSZ			(offsetof(RBIEntryTuple, ntids) + sizeof(uint64))	/* 32 */
#define RBIEntryGetKey(e)		((char *) (e) + RBI_ENTRY_HDRSZ)
#define RBIEntryPayloadOffset(e) MAXALIGN(RBI_ENTRY_HDRSZ + (e)->keylen)
#define RBIEntryGetPayload(e)	((char *) (e) + RBIEntryPayloadOffset(e))
/* payload length must be computed from the item size: itemsz - RBIEntryPayloadOffset(e) */

#define RBI_MAX_KEY_SIZE		2000
#define RBI_DEFAULT_INLINE_LIMIT 4096
#define RBI_MIN_INLINE_LIMIT	64
#define RBI_MAX_INLINE_LIMIT	4096

/* ---------- reloptions ---------- */

typedef struct RBIOptions
{
	int32		vl_len_;		/* varlena header (do not touch directly) */
	int			buckets;		/* 0 = auto */
	int			inline_limit;
	int			max_entries;	/* 0 = unlimited (DESIGN.md §17) */
} RBIOptions;

#define RBI_DEFAULT_MAX_ENTRIES		0

/* ---------- per-relation cached state (rd_amcache) ---------- */

typedef struct RBIState
{
	RBIMetaPageData meta;		/* copy of the meta page */

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
} RBIState;

/* ---------- multi-key extraction (DESIGN.md §17, rbi_multikey.c) ---------- */

/*
 * Strategy numbers of the roaring AM.  1 is the only one a scalar opclass
 * has; 2 .. 5 belong to the multi-key classes.
 */
#define RBI_STRAT_EQUAL			1
#define RBI_STRAT_CONTAINS		2	/* anyarray @> anyarray */
#define RBI_STRAT_OVERLAP		3	/* anyarray && anyarray */
#define RBI_STRAT_CONTAINED		4	/* anyarray <@ anyarray */
#define RBI_STRAT_MATCH			5	/* tsvector @@ tsquery */
#define RBI_NSTRATEGIES			5

/* Support procedure numbers. */
#define RBI_HASH_PROC			1
#define RBI_EXTRACTVALUE_PROC	2
#define RBI_EXTRACTQUERY_PROC	3
#define RBI_NPROC				3

/*
 * What a query over a multi-key index selects.
 *
 *	RBI_QMODE_NONE	nothing at all (`tags && '{}'`)
 *	RBI_QMODE_KEYS	exactly the rows the key tree selects; no recheck
 *	RBI_QMODE_ALL	every indexed row, with recheck (`tags @> '{}'`, `<@`,
 *					a tsquery with NOT/phrase/prefix/weights, a NULL key)
 */
typedef enum RBIQueryMode
{
	RBI_QMODE_NONE = 0,
	RBI_QMODE_KEYS,
	RBI_QMODE_ALL
} RBIQueryMode;

/*
 * A boolean tree over extracted keys.  A leaf names a key by its position in
 * the array the extraction produced; the count code reuses the very same
 * structure with keyno naming a located posting set instead, which is what
 * lets one evaluator serve both the bitmap scan and the count pushdown.
 */
typedef enum RBIKeyNodeKind
{
	RBI_KN_KEY = 0,
	RBI_KN_AND,
	RBI_KN_OR
} RBIKeyNodeKind;

typedef struct RBIKeyNode
{
	RBIKeyNodeKind kind;
	int			keyno;			/* RBI_KN_KEY only */
	int			nargs;
	struct RBIKeyNode **args;
} RBIKeyNode;

typedef struct RBIQuery
{
	RBIQueryMode mode;
	int			nkeys;			/* keys the extraction produced */
	Datum	   *keys;
	RBIKeyNode *tree;			/* RBI_QMODE_KEYS only; over keys[] */
} RBIQuery;

/*
 * Extract the keys of one indexed value with support proc 2, drop the NULL
 * ones and the duplicates, and return them in *keys (palloc'd in the current
 * context) with the count as the return value.  Zero keys means the row
 * belongs in the reserved EMPTY entry.
 */
extern int rbi_extract_value(RBIState *state, Datum value, Datum **keys);

/*
 * Extract a query with support proc 3 and work out how its keys combine
 * (DESIGN.md §17).  Everything is palloc'd in the current context.
 */
extern void rbi_extract_query(RBIState *state, Datum query,
							  StrategyNumber strategy, RBIQuery *q);


/* ---------- rbi_pages.c: primitives shared by build/insert/scan/vacuum ---------- */

extern RBIState *rbi_get_state(Relation index);
extern void rbi_init_page(Page page, uint16 flags);
extern Buffer rbi_new_buffer(Relation index, uint16 flags);	/* extends rel; returns pinned+X-locked, page initialised */
extern void rbi_init_metapage(Page page, uint32 nbuckets, uint32 inline_limit);
extern void rbi_check_key_offset(ItemPointer tid);	/* ERROR if offset > RBI_MAX_OFFSET */

/* Key handling */
extern uint32 rbi_hash_key(RBIState *state, Datum key);
extern Size rbi_key_datum_size(RBIState *state, Datum key);	/* bytes needed to store the key */
extern void rbi_store_key(RBIState *state, Datum key, char *dest);
extern Datum rbi_fetch_key(RBIState *state, const char *src);
extern bool rbi_keys_equal(RBIState *state, Datum a, Datum b);

/*
 * Entry lookup in bucket chain.  Caller holds the bucket head buffer locked
 * (SHARE for readers, EXCLUSIVE for writers).  On success returns true and
 * sets *buf (pinned and locked with the caller's lock mode; may be the head
 * buffer itself, in which case no extra pin is taken) and *offnum.
 */
extern bool rbi_find_entry(Relation index, RBIState *state, Buffer headbuf, int lockmode,
						   Datum key, uint32 hash, Buffer *buf, OffsetNumber *offnum);

/* Build an entry tuple in palloc'd memory; *size receives its total length. */
extern RBIEntryTuple *rbi_make_entry(RBIState *state, Datum key, uint32 hash, uint16 flags,
									 const char *payload, Size payloadlen, Size *size);

/*
 * The same for a reserved entry (DESIGN.md §14 and §17): no key, hash 0, and
 * reservedflag - exactly one of RBI_ENTRY_NULLKEY and RBI_ENTRY_EMPTYKEY -
 * set on top of the caller's INLINE/CHAIN flag.
 */
extern RBIEntryTuple *rbi_make_reserved_entry(uint16 reservedflag, uint16 flags,
											  const char *payload,
											  Size payloadlen, Size *size);

/*
 * Find a reserved entry in bucket 0, whose head page the caller holds locked
 * in lockmode (and keeps locked).  The buf and offnum outputs work exactly as
 * they do for rbi_find_entry().
 */
extern bool rbi_find_reserved_entry(Relation index, Buffer headbuf,
									int lockmode, uint16 reservedflag,
									Buffer *buf, OffsetNumber *offnum);

static inline bool
rbi_find_null_entry(Relation index, Buffer headbuf, int lockmode,
					Buffer *buf, OffsetNumber *offnum)
{
	return rbi_find_reserved_entry(index, headbuf, lockmode,
								   RBI_ENTRY_NULLKEY, buf, offnum);
}

/*
 * Warn once per backend per index when an index has grown past its
 * max_entries reloption (DESIGN.md §17).  nentries is an estimate; the
 * warning never rejects a row.
 */
extern void rbi_warn_max_entries(Relation index, int64 nentries);

/* The max_entries reloption of an index, 0 when unlimited. */
extern int rbi_max_entries(Relation index);

/*
 * Number of entries on the bucket chain starting at headbuf, which the caller
 * holds locked.  Only the cardinality guard calls this.
 */
extern int64 rbi_bucket_nentries(Relation index, Buffer headbuf);

/*
 * Add a new entry to the bucket chain starting at headbuf (held EXCLUSIVE).
 * Appends a bucket page if needed.  WAL-logs via GenericXLog internally.
 */
extern void rbi_add_entry(Relation index, Buffer headbuf, RBIEntryTuple *entry, Size size);

/*
 * Replace the entry at (buf, offnum) with a new version (buf held EXCLUSIVE).
 * Handles size changes; if the new tuple cannot fit even after defragmenting
 * the page, returns false and changes nothing (caller must spill).
 * Caller supplies the GenericXLogState if it wants the change batched with
 * other buffers (state may be NULL: then this function logs by itself).
 */
extern bool rbi_replace_entry(Relation index, GenericXLogState *state, Buffer buf,
							  OffsetNumber offnum, RBIEntryTuple *entry, Size size);

/* Container chain navigation */
extern BlockNumber rbi_chain_find_page(Relation index, BlockNumber head, BlockNumber tail, uint32 ckey);

/*
 * Locate a container by ckey on a container page.  Returns the offset of the
 * item whose own ckey is exactly ckey, or the offset where it should be
 * inserted with *found=false.  Only for callers that know they are looking
 * for a regular container (a segment's own ckey is its first one).
 */
extern OffsetNumber rbi_page_find_container(Page page, uint32 ckey, bool *found);

/*
 * Locate the item that covers ckey on a container page: the container with
 * that ckey, or the sparse segment whose range [first ckey, last ckey]
 * contains it.  With *found=false the return value is the offset at which an
 * item for ckey would have to be inserted to keep the items ordered by first
 * ckey (DESIGN.md §13; item ranges never overlap or interleave).
 */
extern OffsetNumber rbi_page_find_item(Page page, uint32 ckey, bool *found);

/*
 * Insert-or-replace a container in the chain of an entry, splitting pages as
 * needed (DESIGN.md §4).  The entry's bucket buffer (entrybuf, held EXCLUSIVE)
 * is updated when tail changes.  ncontainers_delta receives +1 for a new
 * container, 0 for a replacement.  All changes WAL-logged.
 */
extern void rbi_chain_put_container(Relation index, Buffer entrybuf, OffsetNumber entryoff,
									RBIEntryTuple *entry, RBIContainer *c, int *ncontainers_delta);

/* Page-level min/max maintenance after any change of a container page's items. */
extern void rbi_page_update_minmax(Page page);

/* Convenience: read the meta page once and validate it (used by rbi_get_state). */
extern void rbi_read_meta(Relation index, RBIMetaPageData *meta);

/* ---------- AM entry points ---------- */

extern IndexBuildResult *rbibuild(Relation heap, Relation index, struct IndexInfo *indexInfo);
extern void rbibuildempty(Relation index);
extern bool rbiinsert(Relation index, Datum *values, bool *isnull, ItemPointer ht_ctid,
					  Relation heapRel, IndexUniqueCheck checkUnique, bool indexUnchanged,
					  struct IndexInfo *indexInfo);
extern IndexBulkDeleteResult *rbibulkdelete(IndexVacuumInfo *info, IndexBulkDeleteResult *stats,
											IndexBulkDeleteCallback callback, void *callback_state);
extern IndexBulkDeleteResult *rbivacuumcleanup(IndexVacuumInfo *info, IndexBulkDeleteResult *stats);
extern IndexScanDesc rbibeginscan(Relation r, int nkeys, int norderbys);
extern void rbirescan(IndexScanDesc scan, ScanKey scankey, int nscankeys, ScanKey orderbys, int norderbys);
extern void rbiendscan(IndexScanDesc scan);
extern int64 rbigetbitmap(IndexScanDesc scan, TIDBitmap *tbm);
extern bytea *rbioptions(Datum reloptions, bool validate);
extern bool rbivalidate(Oid opclassoid);
extern void rbicostestimate(struct PlannerInfo *root, struct IndexPath *path, double loop_count,
							Cost *indexStartupCost, Cost *indexTotalCost, Selectivity *indexSelectivity,
							double *indexCorrelation, double *indexPages);

/*
 * Emit one item's members into a TIDBitmap (rbi_scan.c), one tbm_add_tuples()
 * call per heap block; works for containers and sparse segments alike.
 * recheck is passed straight to tbm_add_tuples(): true when the scan is
 * emitting a superset of the matching TIDs and the heap scan has to apply the
 * original quals again.  Returns the number of members emitted.
 */
extern int64 rbi_container_to_tbm(const RBIContainer *c, TIDBitmap *tbm,
								  bool recheck);


/* ---------- additive helpers (wave 1, agent "am-core") ---------- */

/*
 * Largest item that can be placed on an otherwise empty roaring index page.
 */
#define RBI_MAX_ITEM_SIZE \
	MAXALIGN_DOWN(BLCKSZ - SizeOfPageHeaderData - RBI_SPECIAL_SIZE - sizeof(ItemIdData))

/*
 * An INLINE entry payload holds containers back to back with no padding
 * (DESIGN.md section 4), so a container inside it is generally not aligned.
 * rbi_inline_fetch() copies the container at byte offset *off into the
 * caller's aligned buffer (capacity RBI_CONTAINER_MAX_SIZE), advances *off
 * past it, and returns its size; it returns 0 when the payload is exhausted.
 * Containers stored as page items are always MAXALIGNed by PageAddItem and
 * can be used in place.
 */
extern Size rbi_inline_fetch(const char *payload, Size paylen, Size *off,
							 RBIContainer *buf);

/* Length in bytes of an INLINE entry's payload, given the page item size. */
#define RBI_ENTRY_PAYLOAD_LEN(e, itemsz)	((Size) (itemsz) - RBIEntryPayloadOffset(e))

/*
 * Fill *state for index, using the supplied meta page image.  ambuild uses
 * this before the meta page exists; rbi_get_state() uses it afterwards.
 * FmgrInfos are allocated in cxt.
 */
extern void rbi_fill_state(Relation index, RBIState *state,
						   const RBIMetaPageData *meta, MemoryContext cxt);

/*
 * Like rbi_find_entry(), but compares the stored key against key with the
 * caller-supplied equality function (used by scans, where the scan key may be
 * of a type that is only cross-type equal to the indexed type).  eqproc NULL
 * means "use state->eqproc with state->collation".  The stored key is always
 * the left-hand argument.
 */
extern bool rbi_find_entry_ext(Relation index, RBIState *state, Buffer headbuf,
							   int lockmode, Datum key, uint32 hash,
							   FmgrInfo *eqproc, Oid collation,
							   Buffer *buf, OffsetNumber *offnum);

/* Clamp a requested bucket count into [1, RBI_MAX_BUCKETS]; no rounding. */
extern uint32 rbi_clamp_buckets(int64 nbuckets);


/* ---------- additive helpers (wave 2, insert/vacuum/verify) ---------- */

/*
 * Extend the index by one page inside the caller's GenericXLog record (the
 * page initialisation is logged as a full image).  The buffer comes back
 * pinned and EXCLUSIVE; *pagep, if not NULL, receives the registered image.
 * Allocating a page in the record that links it into a chain is what keeps a
 * crash from leaving an initialised page nothing points at.
 */
extern Buffer rbi_new_buffer_xl(Relation index, GenericXLogState *xstate,
								uint16 flags, Page *pagep);

/*
 * Private copy of an entry tuple (header, key and padding taken from entry,
 * which may point into a page) with a new payload.  A zero-length payload
 * gives the shape of a CHAIN entry.  The caller fills in flags, head, tail,
 * ncontainers and ntids.
 */
extern RBIEntryTuple *rbi_entry_rebuild(const RBIEntryTuple *entry,
										const char *payload, Size payloadlen,
										Size *size);

/*
 * Like rbi_chain_put_container(), but for a container page the caller has
 * already located and locked EXCLUSIVE (a cleanup lock counts); the buffer
 * stays locked.  The page must be the one that owns c->ckey.
 */
extern void rbi_chain_put_container_locked(Relation index, Buffer buf,
										   Buffer entrybuf, OffsetNumber entryoff,
										   RBIEntryTuple *entry, RBIContainer *c,
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
#define RBI_MAX_PUT_ITEMS	3

extern void rbi_chain_put_items_locked(Relation index, Buffer buf,
									   Buffer entrybuf, OffsetNumber entryoff,
									   RBIEntryTuple *entry,
									   OffsetNumber off, bool replace,
									   RBIContainer **items, int nitems);

/*
 * Move an INLINE payload onto a chain of freshly allocated container pages
 * and rewrite the entry (a private copy in CHAIN shape, ncontainers/ntids
 * already set) as a CHAIN entry.  The entry page is held EXCLUSIVE by the
 * caller.
 */
extern void rbi_entry_spill(Relation index, Buffer entrybuf, OffsetNumber entryoff,
							RBIEntryTuple *entry, const char *payload, Size paylen);

#endif							/* RBI_H */
