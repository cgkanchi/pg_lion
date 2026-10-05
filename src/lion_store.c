/*-------------------------------------------------------------------------
 *
 * lion_store.c
 *		The window store (DESIGN.md §40): which columns are stored, the
 *		format of the map and of a store page, the build's emitter, the
 *		insert path, VACUUM's pass over it, and the gather that reads it.
 *
 * lion_store.h describes the structures and their locking; lion_store_fmt.h
 * the bytes of a store page.  The code is in this order:
 *
 *	1. stored columns and their values
 *	2. checking a store page
 *	3. the page MODEL: a store page decoded into one value per slot, and
 *	   encoded back into a page image - every structural change (a build's
 *	   pages, a rewrite, a split, DICT to RAW, ABSENT, VACUUM's rewrite) goes
 *	   through it, so that there is one encoder and one decoder
 *	4. the window map
 *	5. the build
 *	6. the insert path
 *	7. VACUUM
 *	8. the gather, and what verify() asks of the store
 *
 * Every page change goes through the WAL shim of DESIGN.md §25: in rmgr mode
 * a LION_XLOG_STORE record made of the existing operations (INIT, ADD,
 * ADDMANY, REPLACE/DELTA, SETBYTES, SPECIAL, DELETED) and LION_OP_STORE_META
 * for the meta page's count; in generic mode a GenericXLog record.  Pages are
 * allocated (lion_alloc_page()) and the meta page locked before a record
 * opens, because in rmgr mode the record is a critical section.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <math.h>

#include "access/htup_details.h"
#include "access/tupmacs.h"
#include "access/xact.h"
#include "catalog/pg_collation.h"
#include "catalog/pg_type.h"
#include "commands/vacuum.h"
#include "common/hashfn.h"
#include "miscadmin.h"
#include "nodes/nodeFuncs.h"
#include "nodes/pathnodes.h"
#include "port/pg_bitutils.h"
#include "storage/bufmgr.h"
#include "storage/indexfsm.h"
#include "storage/lmgr.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/sortsupport.h"
#include "utils/spccache.h"
#include "utils/typcache.h"
#include "varatt.h"

#include "lion.h"
#include "lion_compat.h"
#include "lion_store.h"
#include "lion_wal.h"

/*
 * How many times one write starts again from the map before it gives up.  A
 * write starts again after every split it causes (at most one per halving of
 * a page's range, six at 8K) and after finding a page freed under it, which
 * only a concurrent cleanup of a window past the heap's end does; anything
 * beyond this is a loop that damage keeps going.
 */
#define LION_STORE_MAX_ATTEMPTS		64

/*
 * The window lock of an ordered index (DESIGN.md §41, "The window lock"): a
 * heavyweight page lock on a block number no index reaches, one per window,
 * which inserts take in share mode and VACUUM's sort exclusively.
 */
#define LION_STORE_WINLOCK(ckey)	((BlockNumber) (MaxBlockNumber - (ckey)))

/* What an item takes on a page: itself, aligned, and its line pointer. */
#define LION_STORE_ITEM_COST(len)	(MAXALIGN(len) + sizeof(ItemIdData))

/*
 * The insert path's memory of the map (LionIndexState.storecache): the last
 * inner and leaf page it found, which can never change once they exist (map
 * pages never move), and the last window head, which can and is therefore
 * only a hint the page check confirms.
 */
typedef struct LionStoreCache
{
	uint64		innerno;
	BlockNumber innerblk;
	uint64		leafno;
	BlockNumber leafblk;
	uint64		slot;
	BlockNumber head;
} LionStoreCache;

/* pg_lion.store_heap_order (DESIGN.md §41): builds write heap order. */
bool		lion_store_heap_order = false;

/* One slot's value, as the model holds it: data NULL is SQL NULL. */
typedef struct LionStoreVal
{
	const char *data;
	uint32		len;
} LionStoreVal;

/* ---------------------------------------------------------------------
 * 1. Stored columns and their values
 * --------------------------------------------------------------------- */

/*
 * The type of the datum index column attno holds: the heap attribute's, or
 * the expression's.  An INCLUDE column is always a plain attribute.
 */
static Oid
lion_store_column_type(Relation index, AttrNumber attno)
{
	Form_pg_index idx = index->rd_index;
	AttrNumber	heapatt = idx->indkey.values[attno - 1];
	List	   *exprs;
	ListCell   *lc;
	int			nexpr = 0;
	int			i;

	if (heapatt != 0)
		return get_atttype(idx->indrelid, heapatt);

	/* the expressions are in column order, one per zero in indkey */
	for (i = 0; i < attno - 1; i++)
		if (idx->indkey.values[i] == 0)
			nexpr++;
	exprs = RelationGetIndexExpressions(index);
	foreach(lc, exprs)
	{
		if (nexpr-- == 0)
			return exprType((Node *) lfirst(lc));
	}
	elog(ERROR, "lion index \"%s\": no expression for column %d",
		 RelationGetRelationName(index), attno);
	return InvalidOid;			/* keep the compiler quiet */
}

uint32
lion_store_columns(Relation index, LionIndexState *ix, bool report)
{
	LionOptions *opts = (LionOptions *) index->rd_options;
	bool		values = (opts != NULL && opts->store_values);
	int			maxlen = (opts != NULL) ? opts->store_max_len : 0;
	int			natts = IndexRelationGetNumberOfAttributes(index);
	int			nkeys = IndexRelationGetNumberOfKeyAttributes(index);
	uint32		cols = 0;
	int			i;

	Assert(natts <= LION_META_MAX_COLS);

	for (i = 0; i < natts; i++)
	{
		bool		iskey = (i < nkeys);
		Oid			typid;
		int16		typlen;
		bool		typbyval;
		char		typalign;

		if (iskey && !values)
			continue;

		typid = lion_store_column_type(index, (AttrNumber) (i + 1));
		get_typlenbyvalalign(typid, &typlen, &typbyval, &typalign);

		if (iskey)
		{
			/*
			 * DESIGN.md §40: only a scalar key column has one value per row
			 * to store.  A multi-key column - an array, a tsvector, jsonb
			 * through a multi-key class - gives a row many keys and is left
			 * out, and so is a key whose type the store cannot hold; neither
			 * stops the index from storing its other columns.
			 */
			if (ix->cols[i].multikey)
			{
				if (report)
					ereport(NOTICE,
							(errmsg("key column %d of lion index \"%s\" is not stored",
									i + 1, RelationGetRelationName(index)),
							 errdetail("Its operator class gives a row many keys, and the store keeps one value per row.")));
				continue;
			}
			if (typlen == -2 || typlen > LION_MAX_KEY_SIZE)
			{
				if (report)
					ereport(NOTICE,
							(errmsg("key column %d of lion index \"%s\" is not stored",
									i + 1, RelationGetRelationName(index)),
							 errdetail("Values of type %s cannot be stored.",
									   format_type_be(typid))));
				continue;
			}
		}
		else
		{
			/* an INCLUDE column is stored or the index is not built */
			if (typlen == -2)
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("lion index \"%s\" cannot include column %d of type %s",
								RelationGetRelationName(index), i + 1,
								format_type_be(typid)),
						 errdetail("A lion index stores its INCLUDE columns, and values of a null-terminated type cannot be stored.")));
			if (typlen > LION_MAX_KEY_SIZE)
				ereport(ERROR,
						(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
						 errmsg("lion index \"%s\" cannot include column %d of type %s",
								RelationGetRelationName(index), i + 1,
								format_type_be(typid)),
						 errdetail("Its values are %d bytes wide; a stored value can be at most %d bytes.",
								   typlen, LION_MAX_KEY_SIZE)));
		}
		cols |= ((uint32) 1) << i;
	}

	if (values && cols == 0)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("lion index \"%s\" has no column store_values can store",
						RelationGetRelationName(index)),
				 errdetail("store_values stores the scalar key columns, and every key column of this index gives a row many keys or has a type that cannot be stored."),
				 errhint("Leave store_values off, or list the columns to store in INCLUDE.")));

	if (report && cols == 0 && maxlen > 0)
		ereport(NOTICE,
				(errmsg("store_max_len has no effect on lion index \"%s\"",
						RelationGetRelationName(index)),
				 errdetail("The index stores no column: it has neither store_values nor INCLUDE columns.")));

	return cols;
}

void
lion_store_fill_state(Relation index, LionIndexState *ix,
					  const LionMetaStore *store, MemoryContext cxt)
{
	int			natts = IndexRelationGetNumberOfAttributes(index);
	int			nkeys = IndexRelationGetNumberOfKeyAttributes(index);
	TupleDesc	desc = RelationGetDescr(index);
	int			n = 0;
	int			i;

	ix->store = *store;
	ix->nstored = 0;
	ix->stored = NULL;
	ix->storecache = NULL;
	ix->store_order = -1;
	ix->storeperm = NULL;
	if (store->store_cols == 0)
		return;

	if ((natts < 32 && (store->store_cols >> natts) != 0) ||
		store->store_max_len > LION_MAX_STORE_MAX_LEN)
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg("index \"%s\" records stored columns %08X of an index of %d columns",
						RelationGetRelationName(index), store->store_cols, natts),
				 errhint("REINDEX the index.")));

	ix->stored = (LionStoreCol *)
		MemoryContextAllocZero(cxt, sizeof(LionStoreCol) * pg_popcount32(store->store_cols));
	for (i = 0; i < natts; i++)
	{
		LionStoreCol *col;

		if ((store->store_cols & (((uint32) 1) << i)) == 0)
			continue;
		col = &ix->stored[n];
		col->ord = n;
		col->attno = (AttrNumber) (i + 1);
		col->iskey = (i < nkeys);
		col->typid = lion_store_column_type(index, col->attno);
		get_typlenbyvalalign(col->typid, &col->typlen, &col->typbyval,
							 &col->typalign);

		/*
		 * A build stores only what it can (lion_store_columns()); a record
		 * that names anything else was not written by one.
		 */
		if (col->typlen == -2 || col->typlen == 0 ||
			col->typlen > LION_MAX_KEY_SIZE ||
			(col->iskey && ix->cols[i].multikey))
			ereport(ERROR,
					(errcode(ERRCODE_INDEX_CORRUPTED),
					 errmsg("index \"%s\" records column %d as stored, which cannot be",
							RelationGetRelationName(index), i + 1),
					 errhint("REINDEX the index.")));

		col->maxlen = (col->typlen == -1) ? (int) store->store_max_len : 0;
		if (col->typlen > 0)
			col->rawwidth = col->typlen;
		else
			col->rawwidth = (col->maxlen > 0) ? col->maxlen + (int) sizeof(uint16) : 0;
		col->returnable = (TupleDescAttr(desc, i)->atttypid == col->typid);
		col->kind = LION_STORE_KIND_HEAP;
		n++;
	}
	ix->nstored = n;

	ix->storecache = (LionStoreCache *) MemoryContextAlloc(cxt, sizeof(LionStoreCache));
	ix->storecache->innerno = 0;
	ix->storecache->innerblk = InvalidBlockNumber;
	ix->storecache->leafno = 0;
	ix->storecache->leafblk = InvalidBlockNumber;
	ix->storecache->slot = 0;
	ix->storecache->head = InvalidBlockNumber;
}

int
lion_store_ordinal(const LionIndexState *ix, AttrNumber attno)
{
	int			i;

	for (i = 0; i < ix->nstored; i++)
		if (ix->stored[i].attno == attno)
			return i;
	return -1;
}

/* Has the stored type of col a default B-tree ordering to sort by (§41)? */
static bool
lion_store_col_sortable(const LionStoreCol *col)
{
	TypeCacheEntry *tce = lookup_type_cache(col->typid, TYPECACHE_LT_OPR);

	return OidIsValid(tce->lt_opr);
}

int
lion_store_choose_order(Relation index, LionIndexState *ix, uint16 *flags)
{
	LionOptions *opts = (LionOptions *) index->rd_options;
	const char *name = NULL;
	int			ord;

	*flags = 0;
	if (opts != NULL && opts->cluster_column != 0)
		name = (const char *) opts + opts->cluster_column;

	if (name != NULL)
	{
		TupleDesc	desc = RelationGetDescr(index);
		AttrNumber	attno = InvalidAttrNumber;
		int			i;

		for (i = 0; i < desc->natts; i++)
			if (strcmp(NameStr(TupleDescAttr(desc, i)->attname), name) == 0)
			{
				attno = (AttrNumber) (i + 1);
				break;
			}
		if (attno == InvalidAttrNumber)
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_COLUMN),
					 errmsg("cluster_column \"%s\" is not a column of lion index \"%s\"",
							name, RelationGetRelationName(index))));
		ord = lion_store_ordinal(ix, attno);
		if (ord < 0)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("cluster_column \"%s\" of lion index \"%s\" is not a stored column",
							name, RelationGetRelationName(index)),
					 errhint("The windows are ordered by a stored column: name an INCLUDE column, or a scalar key column with store_values on.")));
		if (!lion_store_col_sortable(&ix->stored[ord]))
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_FUNCTION),
					 errmsg("cluster_column \"%s\" of lion index \"%s\" has type %s, which has no ordering",
							name, RelationGetRelationName(index),
							format_type_be(ix->stored[ord].typid))));
		if (lion_store_heap_order)
			return -1;
		*flags = LION_STORE_ORDER_CLUSTER;
		return ord;
	}

	/*
	 * No cluster_column: heap order, even when the first key column is
	 * stored.  An order helps the predicates on its column and costs every
	 * other one a permutation read and the heap's own locality (§41, "As
	 * built: the order is asked for"), so the index's owner chooses it.
	 */
	return -1;
}

void
lion_store_fill_order(Relation index, LionIndexState *ix, int order,
					  MemoryContext cxt)
{
	LionStoreCol *pc;
	int			i;

	ix->store_order = -1;
	ix->storeperm = NULL;
	if (order < 0)
		return;
	if (order >= ix->nstored)
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg("index \"%s\" records stored column %d as its order, of %d stored columns",
						RelationGetRelationName(index), order, ix->nstored),
				 errhint("REINDEX the index.")));

	for (i = 0; i < ix->nstored; i++)
		ix->stored[i].kind = LION_STORE_KIND_ORDERED;

	/* the permutation: a two-byte RAW column at ordinal nstored */
	pc = (LionStoreCol *) MemoryContextAllocZero(cxt, sizeof(LionStoreCol));
	pc->ord = ix->nstored;
	pc->attno = InvalidAttrNumber;
	pc->iskey = false;
	pc->typid = INT2OID;
	pc->typlen = sizeof(uint16);
	pc->typbyval = true;
	pc->typalign = TYPALIGN_SHORT;
	pc->returnable = false;
	pc->rawwidth = sizeof(uint16);
	pc->maxlen = 0;
	pc->kind = LION_STORE_KIND_PERM;
	ix->storeperm = pc;
	ix->store_order = order;
}

int
lion_read_meta_store_order(Relation index, const LionMetaPageData *meta)
{
	Buffer		buf;
	Page		page;
	bool		have;
	LionMetaStoreOrder mo;
	LionMetaStore ms;

	if (!LION_META_HAS_ORDER(meta))
		return -1;

	buf = ReadBuffer(index, LION_METAPAGE_BLKNO);
	LockBuffer(buf, BUFFER_LOCK_SHARE);
	page = BufferGetPage(buf);
	have = !PageIsNew(page) && PageGetSpecialSize(page) == LION_SPECIAL_SIZE &&
		LionPageIsMeta(page) && LionMetaHasOrderArea(page) &&
		((PageHeader) page)->pd_lower <= ((PageHeader) page)->pd_upper;
	if (have)
	{
		memcpy(&mo, LionPageGetMetaStoreOrder(page), sizeof(LionMetaStoreOrder));
		memcpy(&ms, LionPageGetMetaStore(page), sizeof(LionMetaStore));
	}
	UnlockReleaseBuffer(buf);

	if (!have || mo.order_ord < 0 ||
		mo.order_ord >= pg_popcount32(ms.store_cols) ||
		(mo.order_flags & ~LION_STORE_ORDER_CLUSTER) != 0)
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg("index \"%s\" is not a valid lion index",
						RelationGetRelationName(index)),
				 errdetail("Its meta page is of version %u and has no sound order record.",
						   meta->version)));
	return mo.order_ord;
}

void
lion_read_meta_store(Relation index, const LionMetaPageData *meta,
					 LionMetaStore *store)
{
	Buffer		buf;
	Page		page;
	bool		have;

	memset(store, 0, sizeof(LionMetaStore));
	if (!LION_META_HAS_STORE(meta))
		return;

	buf = ReadBuffer(index, LION_METAPAGE_BLKNO);
	LockBuffer(buf, BUFFER_LOCK_SHARE);
	page = BufferGetPage(buf);
	have = !PageIsNew(page) && PageGetSpecialSize(page) == LION_SPECIAL_SIZE &&
		LionPageIsMeta(page) && LionMetaHasStoreArea(page) &&
		((PageHeader) page)->pd_lower <= ((PageHeader) page)->pd_upper;
	if (have)
		memcpy(store, LionPageGetMetaStore(page), sizeof(LionMetaStore));
	UnlockReleaseBuffer(buf);

	if (!have || store->store_cols == 0 ||
		!BlockNumberIsValid(store->store_root) ||
		store->store_root == LION_METAPAGE_BLKNO ||
		store->store_max_len > LION_MAX_STORE_MAX_LEN)
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg("index \"%s\" is not a valid lion index",
						RelationGetRelationName(index)),
				 errdetail("Its meta page is of version %u and has no sound store record.",
						   meta->version)));
}

/*
 * A stored column's value of one row, as the store keeps it (DESIGN.md §40,
 * "what is stored"): a fixed-width value's typlen bytes, a varlena's bytes
 * detoasted and uncompressed, without the header.  fixbuf is where a
 * by-value datum is spelled out; v points into it, or into the datum, or into
 * a detoasted copy in the current memory context.
 */
static void
lion_store_value(Relation index, const LionStoreCol *col, Datum d, bool isnull,
				 LionStoreVal *v, Datum *fixbuf)
{
	if (isnull)
	{
		v->data = NULL;
		v->len = 0;
		return;
	}

	if (col->typlen > 0)
	{
		if (col->typbyval)
		{
			*fixbuf = 0;
			store_att_byval(fixbuf, d, col->typlen);
			v->data = (const char *) fixbuf;
		}
		else
			v->data = (const char *) DatumGetPointer(d);
		v->len = (uint32) col->typlen;
	}
	else
	{
		struct varlena *flat;
		Size		len;

		Assert(col->typlen == -1);
		flat = pg_detoast_datum_packed((struct varlena *) DatumGetPointer(d));
		len = VARSIZE_ANY_EXHDR(flat);

		/* the limit a key has (lion_key_datum_size()), and for the same reason */
		if (len + VARHDRSZ > LION_MAX_KEY_SIZE)
			ereport(ERROR,
					(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
					 errmsg("lion index stored value size %zu exceeds maximum %d",
							len + VARHDRSZ, LION_MAX_KEY_SIZE),
					 errdetail("Column %d of index \"%s\" is stored.",
							   col->attno, RelationGetRelationName(index))));
		if (col->maxlen > 0 && len > (Size) col->maxlen)
			ereport(ERROR,
					(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
					 errmsg("value of %zu bytes is longer than store_max_len %d of lion index \"%s\"",
							len, col->maxlen, RelationGetRelationName(index)),
					 errdetail("Column %d of the index is stored in slots of %d bytes.",
							   col->attno, col->maxlen),
					 errhint("REINDEX the index with a larger store_max_len, or with none.")));
		v->data = VARDATA_ANY(flat);
		v->len = (uint32) len;
	}
}

/* The datum of stored bytes, a by-reference one copied into cxt. */
static Datum
lion_store_datum(const LionStoreCol *col, const char *data, uint32 len,
				 MemoryContext cxt)
{
	if (col->typlen > 0)
	{
		if (col->typbyval)
		{
			union
			{
				Datum		d;
				char		c[sizeof(Datum)];
			}			u;

			u.d = 0;
			memcpy(u.c, data, col->typlen);
			return fetch_att(u.c, true, col->typlen);
		}
		else
		{
			char	   *p = MemoryContextAlloc(cxt, col->typlen);

			memcpy(p, data, col->typlen);
			return PointerGetDatum(p);
		}
	}
	else
	{
		struct varlena *v = (struct varlena *) MemoryContextAlloc(cxt, len + VARHDRSZ);

		SET_VARSIZE(v, len + VARHDRSZ);
		memcpy(VARDATA(v), data, len);
		return PointerGetDatum(v);
	}
}

static inline bool
lion_store_val_equal(const LionStoreVal *a, const char *data, uint32 len)
{
	return a->len == len && memcmp(a->data, data, len) == 0;
}

/* ---------------------------------------------------------------------
 * 2. Checking a store page
 * --------------------------------------------------------------------- */

bool
lion_store_page_owned(Page page, uint32 ckey, uint16 ord)
{
	LionPageOpaque opaque;

	if (PageIsNew(page) || PageGetSpecialSize(page) != LION_SPECIAL_SIZE)
		return false;
	opaque = LionPageGetOpaque(page);

	return opaque->page_id == LION_PAGE_ID &&
		(opaque->flags & (LION_PAGE_KINDS | LION_PAGE_DELETED)) ==
		LION_PAGE_STORE &&
		opaque->owner_head == ckey &&
		opaque->owner_hash == (uint32) ord;
}

/*
 * The part of lion_store_page_check() every reader makes on every lock: the
 * header, the item count, and the dictionary's size.  A reader checks each
 * sub-array as it decodes it (lion_store_check_sub()) and every dictionary
 * offset as it follows it, which together keep every read inside the page.
 */
static char *
lion_store_check_header(Page page, const LionStoreCol *col)
{
	OffsetNumber maxoff = PageGetMaxOffsetNumber(page);
	LionPageOpaque opaque = LionPageGetOpaque(page);
	ItemId		iid;
	LionStoreHeader h;
	LionStoreDict d;
	OffsetNumber off;

	if (maxoff < LION_STORE_SUB_FIRST || (Size) maxoff > MaxIndexTuplesPerPage)
		return psprintf("has %u items", (unsigned) maxoff);
	for (off = FirstOffsetNumber; off <= maxoff; off++)
	{
		iid = PageGetItemId(page, off);
		if (!ItemIdIsNormal(iid) || !ItemIdHasStorage(iid) ||
			ItemIdGetOffset(iid) < ((PageHeader) page)->pd_upper ||
			ItemIdGetOffset(iid) + ItemIdGetLength(iid) >
			((PageHeader) page)->pd_special)
			return psprintf("has a bad line pointer at item %u", (unsigned) off);
	}

	iid = PageGetItemId(page, LION_STORE_HDR_OFF);
	if (ItemIdGetLength(iid) != sizeof(LionStoreHeader))
		return psprintf("has a header of %u bytes", ItemIdGetLength(iid));
	memcpy(&h, PageGetItem(page, iid), sizeof(LionStoreHeader));

	if (h.ckey != opaque->owner_head || h.ord != opaque->owner_hash)
		return psprintf("has a header for window %u column %u under the stamp of window %u column %u",
						h.ckey, (unsigned) h.ord, opaque->owner_head,
						opaque->owner_hash);
	if (h.lo > h.hi || h.hi >= lion_store_col_pages(col))
		return psprintf("covers heap pages %u to %u of its window",
						(unsigned) h.lo, (unsigned) h.hi);
	if ((int) maxoff != (int) LION_STORE_SUB_FIRST - 1 + (h.hi - h.lo + 1))
		return psprintf("has %u items for heap pages %u to %u",
						(unsigned) maxoff, (unsigned) h.lo, (unsigned) h.hi);
	if (h.typlen != col->typlen ||
		(col->kind == LION_STORE_KIND_HEAP && h.flags != 0) ||
		(col->kind == LION_STORE_KIND_ORDERED &&
		 (h.flags & LION_STORE_F_KINDS) != LION_STORE_F_VIRTUAL) ||
		(col->kind == LION_STORE_KIND_PERM &&
		 (h.flags & LION_STORE_F_KINDS) != LION_STORE_F_PERM))
		return psprintf("has typlen %d and flags %u, expected typlen %d",
						(int) h.typlen, (unsigned) h.flags, (int) col->typlen);

	iid = PageGetItemId(page, LION_STORE_DICT_OFF);
	if (ItemIdGetLength(iid) < sizeof(LionStoreDict))
		return psprintf("has a dictionary of %u bytes", ItemIdGetLength(iid));
	memcpy(&d, PageGetItem(page, iid), sizeof(LionStoreDict));

	/*
	 * A permutation page is RAW, and the dictionary item of its chain's head
	 * is the window header (§41): LionStoreWinHdr and its directory.
	 */
	if (col->kind == LION_STORE_KIND_PERM)
	{
		LionStoreWinHdr wh;

		if (h.mode != LION_STORE_RAW || h.width != col->rawwidth || h.ndict != 0 ||
			d.ndict != 0 ||
			ItemIdGetLength(iid) != sizeof(LionStoreDict) + (Size) d.nbytes)
			return psprintf("is a permutation page of mode %u, width %u, and a dictionary of %u bytes",
							(unsigned) h.mode, (unsigned) h.width,
							ItemIdGetLength(iid));
		if (h.lo != 0)
		{
			if (d.nbytes != 0)
				return psprintf("is a permutation page past its head with a window header of %u bytes",
								(unsigned) d.nbytes);
			return NULL;
		}
		if (d.nbytes < sizeof(LionStoreWinHdr))
			return psprintf("is the head of a permutation with a window header of %u bytes",
							(unsigned) d.nbytes);
		memcpy(&wh, (const char *) PageGetItem(page, iid) + sizeof(LionStoreDict),
			   sizeof(LionStoreWinHdr));
		if ((Size) d.nbytes != sizeof(LionStoreWinHdr) +
			(Size) wh.ndir * sizeof(LionStoreDirEnt) ||
			(wh.flags & ~LION_STORE_WH_THIN) != 0 || wh.vwidth == 0 || wh.vwidth > LION_MAX_OFFSET ||
			(uint32) wh.nsorted > (uint32) wh.vwidth * LION_BLOCKS_PER_CONTAINER)
			return psprintf("has a window header of %u bytes for %u directory entries, %u rows sorted %u to a page",
							(unsigned) d.nbytes, (unsigned) wh.ndir,
							(unsigned) wh.nsorted, (unsigned) wh.vwidth);
		return NULL;
	}

	if (h.mode == LION_STORE_DICT)
	{
		if (!lion_store_width_valid(h.width) ||
			(uint32) h.ndict > lion_store_width_max(h.width))
			return psprintf("is DICT with %u entries of width %u",
							(unsigned) h.ndict, (unsigned) h.width);
		if (d.ndict != h.ndict ||
			(col->typlen > 0 && (Size) d.nbytes != (Size) d.ndict * col->typlen) ||
			ItemIdGetLength(iid) != lion_store_dict_len(col->typlen, d.ndict,
														d.nbytes))
			return psprintf("has a dictionary of %u bytes for %u entries of %u bytes",
							ItemIdGetLength(iid), (unsigned) d.ndict,
							(unsigned) d.nbytes);
	}
	else if (h.mode == LION_STORE_RAW)
	{
		if (col->rawwidth <= 0 || h.width != col->rawwidth || h.ndict != 0)
			return psprintf("is RAW of width %u, for a column whose slots are %d bytes",
							(unsigned) h.width, col->rawwidth);
		if (d.ndict != 0 || d.nbytes != 0 ||
			ItemIdGetLength(iid) != sizeof(LionStoreDict))
			return psprintf("is RAW with a dictionary of %u bytes",
							ItemIdGetLength(iid));
	}
	else
		return psprintf("has mode %u", (unsigned) h.mode);

	return NULL;
}

/* Is sub-array item k (heap page k of the window) consistent with h? */
static char *
lion_store_check_sub(Page page, const LionStoreHeader *h, int k)
{
	ItemId		iid = PageGetItemId(page,
									(OffsetNumber) (LION_STORE_SUB_FIRST + k - h->lo));
	LionStoreSub s;
	Size		len = ItemIdGetLength(iid);

	if (len < sizeof(LionStoreSub))
		return psprintf("has a sub-array of %zu bytes for heap page %d", len, k);
	memcpy(&s, PageGetItem(page, iid), sizeof(LionStoreSub));
	if ((s.flags & ~LION_STORE_ABSENT) != 0 ||
		s.nslots > LION_STORE_MAX_SLOTS ||
		((s.flags & LION_STORE_ABSENT) != 0 && s.nslots != 0) ||
		len != lion_store_sub_len(h->mode, h->width, s.nslots,
								  (s.flags & LION_STORE_ABSENT) != 0))
		return psprintf("has a sub-array of %zu bytes with %u slots and flags %u for heap page %d",
						len, (unsigned) s.nslots, (unsigned) s.flags, k);
	return NULL;
}

char *
lion_store_page_check(Page page, const LionStoreCol *col)
{
	char	   *msg = lion_store_check_header(page, col);
	LionStoreHeader *h;
	int			k;

	if (msg != NULL)
		return msg;
	h = lion_store_page_header(page);
	for (k = h->lo; k <= h->hi; k++)
		if ((msg = lion_store_check_sub(page, h, k)) != NULL)
			return msg;
	if (h->mode == LION_STORE_DICT && col->typlen == -1)
	{
		ItemId		iid = PageGetItemId(page, LION_STORE_DICT_OFF);

		if (!lion_store_vdict_check(PageGetItem(page, iid), ItemIdGetLength(iid)))
			return pstrdup("has a dictionary whose offsets are out of order");
	}
	return NULL;
}

char *
lion_store_page_check_codes(Page page)
{
	LionStoreHeader *h = lion_store_page_header(page);
	int			k;

	for (k = h->lo; k <= h->hi; k++)
	{
		Size		len;
		LionStoreSub *s = lion_store_page_sub(page, k, &len);
		const uint8 *body = (const uint8 *) s + sizeof(LionStoreSub);
		uint32		i;

		if ((s->flags & LION_STORE_ABSENT) != 0)
			continue;
		for (i = 0; i < s->nslots; i++)
		{
			if (h->mode == LION_STORE_DICT)
			{
				uint32		c = lion_store_code_get(body, h->width, i);

				if (c > h->ndict)
					return psprintf("has code %u of a dictionary of %u entries at heap page %d offset %u",
									c, (unsigned) h->ndict, k, i + 1);
			}
			else if (h->typlen == -1 &&
					 !lion_store_null_get(body + (Size) s->nslots * h->width, i))
			{
				uint16		vlen;

				memcpy(&vlen, body + (Size) i * h->width, sizeof(uint16));
				if ((int) vlen > (int) h->width - (int) sizeof(uint16))
					return psprintf("has a value of %u bytes in a slot of %u at heap page %d offset %u",
									(unsigned) vlen, (unsigned) h->width, k, i + 1);
			}
		}
	}
	return NULL;
}

/* ERROR for a page that claims to be the store of (ckey, ord) and is not sound. */
static void
lion_store_check_or_error(Relation index, Page page, BlockNumber blk,
						  const LionStoreCol *col, bool full)
{
	char	   *msg = full ? lion_store_page_check(page, col) :
		lion_store_check_header(page, col);

	if (msg != NULL)
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg("lion index \"%s\": store page %u %s",
						RelationGetRelationName(index), blk, msg),
				 errhint("REINDEX the index.")));
}

/*
 * Dictionary entry c (1-based) of a checked DICT page, its bounds checked:
 * a varlena dictionary's offsets are not all checked on every lock.
 */
static const char *
lion_store_dict_entry(Relation index, const LionStoreCol *col,
					  const char *ditem, Size dlen, uint32 ndict, uint32 c,
					  uint32 *len)
{
	Assert(c >= 1 && c <= ndict);
	if (col->typlen > 0)
	{
		*len = (uint32) col->typlen;
		return ditem + sizeof(LionStoreDict) + (Size) (c - 1) * col->typlen;
	}
	else
	{
		LionStoreDict d;
		uint32		start = lion_store_vdict_off(ditem, dlen, ndict, c - 1);
		uint32		end = lion_store_vdict_off(ditem, dlen, ndict, c);

		memcpy(&d, ditem, sizeof(LionStoreDict));
		if (start > end || end > d.nbytes)
			ereport(ERROR,
					(errcode(ERRCODE_INDEX_CORRUPTED),
					 errmsg("lion index \"%s\": a store dictionary has entry %u at bytes %u to %u of %u",
							RelationGetRelationName(index), c, start, end,
							(unsigned) d.nbytes),
					 errhint("REINDEX the index.")));
		*len = end - start;
		return ditem + sizeof(LionStoreDict) + start;
	}
}

/* The code of value v in a DICT page's dictionary, 0 when it has none. */
static uint32
lion_store_dict_find(Relation index, const LionStoreCol *col, const char *ditem,
					 Size dlen, uint32 ndict, const LionStoreVal *v)
{
	uint32		c;

	for (c = 1; c <= ndict; c++)
	{
		uint32		len;
		const char *e = lion_store_dict_entry(index, col, ditem, dlen, ndict,
											  c, &len);

		if (lion_store_val_equal(v, e, len))
			return c;
	}
	return 0;
}

/* ---------------------------------------------------------------------
 * 3. The page model
 *
 * A store page decoded: for each heap page of its range, the value of every
 * slot (pointing into a copy of the page, or into the caller's data), or the
 * mark that the heap page is ABSENT.  The build fills one from the rows it
 * sees; a structural change decodes the page, changes the model, and encodes
 * it again, choosing the dictionary afresh - which is what drops the values
 * nothing references any more ("compacts", DESIGN.md §40).
 * --------------------------------------------------------------------- */

typedef struct LionStoreModel
{
	uint32		ckey;
	uint16		ord;
	int			lo;
	int			hi;
	uint8		flags;			/* the header's: kind and generation (§41) */

	/*
	 * A permutation head's window header (§41), which the encoder puts in
	 * the dictionary item of the page whose range starts at 0; NULL else.
	 */
	const char *extra;
	uint16		extralen;

	/* per heap page, or virtual page of an ordered index (§41) */
	uint16		nslots[LION_STORE_MAX_VPAGES];
	bool		absent[LION_STORE_MAX_VPAGES];
	LionStoreVal *slots[LION_STORE_MAX_VPAGES];	/* [nslots[k]] */
} LionStoreModel;

/* A page image the encoder made: its items, and what they take on a page. */
typedef struct LionStoreImage
{
	LionStoreHeader hdr;
	int			nitems;
	char	   *items[2 + LION_STORE_MAX_VPAGES];
	Size		lens[2 + LION_STORE_MAX_VPAGES];
	Size		need;
} LionStoreImage;

/* The window header's bytes on a page whose range starts at from (§41). */
static inline Size
lion_store_extra_len(const LionStoreModel *m, int from)
{
	return (from == 0 && m->extra != NULL) ? m->extralen : 0;
}

/*
 * The distinct values of a run of slots, in the order they were first seen,
 * through an open-addressing hash table: what a DICT page's dictionary is
 * built from.  The values are referenced, not copied.
 */
typedef struct LionStoreDictSet
{
	uint32		n;				/* entries */
	uint32		cap;			/* hash slots, a power of two, > 2n */
	uint32	   *table;			/* entry + 1, 0 = empty */
	uint32	   *hashes;			/* per entry */
	LionStoreVal *vals;			/* per entry */
	uint32		vcap;
	Size		bytes;			/* the values' bytes */
	MemoryContext cxt;
} LionStoreDictSet;

static void
lion_store_dset_init(LionStoreDictSet *ds, MemoryContext cxt)
{
	ds->cxt = cxt;
	ds->n = 0;
	ds->cap = 256;
	ds->table = (uint32 *) MemoryContextAllocZero(cxt, sizeof(uint32) * ds->cap);
	ds->vcap = 128;
	ds->hashes = (uint32 *) MemoryContextAlloc(cxt, sizeof(uint32) * ds->vcap);
	ds->vals = (LionStoreVal *) MemoryContextAlloc(cxt, sizeof(LionStoreVal) * ds->vcap);
	ds->bytes = 0;
}

static void
lion_store_dset_reset(LionStoreDictSet *ds)
{
	memset(ds->table, 0, sizeof(uint32) * ds->cap);
	ds->n = 0;
	ds->bytes = 0;
}

static void
lion_store_dset_grow(LionStoreDictSet *ds)
{
	uint32		newcap = ds->cap * 2;
	uint32	   *t = (uint32 *) MemoryContextAllocZero(ds->cxt, sizeof(uint32) * newcap);
	uint32		e;

	for (e = 0; e < ds->n; e++)
	{
		uint32		i = ds->hashes[e] & (newcap - 1);

		while (t[i] != 0)
			i = (i + 1) & (newcap - 1);
		t[i] = e + 1;
	}
	pfree(ds->table);
	ds->table = t;
	ds->cap = newcap;
}

/*
 * The 1-based code of v in the set, adding it when `add` and it is not there
 * (*added says so); 0 when it is not there and not added.
 */
static uint32
lion_store_dset_code(LionStoreDictSet *ds, const LionStoreVal *v, bool add,
					 bool *added)
{
	uint32		h = hash_bytes((const unsigned char *) v->data, (int) v->len) ^ v->len;
	uint32		i = h & (ds->cap - 1);
	uint32		e;

	if (added != NULL)
		*added = false;
	while (ds->table[i] != 0)
	{
		e = ds->table[i] - 1;
		if (ds->hashes[e] == h &&
			lion_store_val_equal(&ds->vals[e], v->data, v->len))
			return e + 1;
		i = (i + 1) & (ds->cap - 1);
	}
	if (!add)
		return 0;

	if (ds->n >= ds->vcap)
	{
		ds->vcap *= 2;
		ds->hashes = (uint32 *) repalloc(ds->hashes, sizeof(uint32) * ds->vcap);
		ds->vals = (LionStoreVal *) repalloc(ds->vals, sizeof(LionStoreVal) * ds->vcap);
	}
	e = ds->n++;
	ds->hashes[e] = h;
	ds->vals[e] = *v;
	ds->table[i] = e + 1;
	ds->bytes += v->len;
	if ((Size) ds->n * 2 >= ds->cap)
		lion_store_dset_grow(ds);
	if (added != NULL)
		*added = true;
	return e + 1;
}

/* What the set's dictionary item takes, its own header excluded. */
static Size
lion_store_dset_bytes(const LionStoreDictSet *ds, const LionStoreCol *col)
{
	return lion_store_dict_len(col->typlen, ds->n, ds->bytes) - sizeof(LionStoreDict);
}

/*
 * DICT or RAW for heap pages [from .. to] of m (DESIGN.md §40, "DICT or
 * RAW"): RAW when the column can be RAW and its dictionary would not pay.
 */
static int
lion_store_choose_mode(const LionStoreCol *col, const LionStoreModel *m,
					   int from, int to, LionStoreDictSet *ds)
{
	Size		rows = 0;
	int			width;
	int			k;

	if (col->rawwidth <= 0)
		return LION_STORE_DICT;
	if (col->kind == LION_STORE_KIND_PERM)
		return LION_STORE_RAW;	/* its dictionary item is the window header */

	lion_store_dset_reset(ds);
	for (k = from; k <= to; k++)
	{
		uint32		i;

		if (m->absent[k])
			continue;
		rows += m->nslots[k];
		for (i = 0; i < m->nslots[k]; i++)
			if (m->slots[k][i].data != NULL)
				(void) lion_store_dset_code(ds, &m->slots[k][i], true, NULL);
	}
	width = lion_store_dict_width(ds->n);
	if (width == 0)
		return LION_STORE_RAW;
	return lion_store_prefer_raw(lion_store_dset_bytes(ds, col), rows, width,
								 col->rawwidth) ? LION_STORE_RAW : LION_STORE_DICT;
}

/*
 * Encode heap pages [from .. to] of m as one page in `mode` (the header says
 * from and to are its range).  false, with img unusable, when it would not
 * fit a page; the items are palloc'd in the current memory context.
 */
static bool
lion_store_encode(const LionStoreCol *col, const LionStoreModel *m, int from,
				  int to, int mode, LionStoreDictSet *ds, LionStoreImage *img)
{
	uint16	   *codes[LION_STORE_MAX_VPAGES];
	int			width;
	Size		dictlen;
	Size		need;
	Size		extralen = lion_store_extra_len(m, from);
	int			k;
	int			n = 0;

	Assert(from >= m->lo && to <= m->hi && from <= to);
	Assert(extralen == 0 || mode == LION_STORE_RAW);
	if (extralen > 0 && mode != LION_STORE_RAW)
		return false;

	if (mode == LION_STORE_DICT)
	{
		lion_store_dset_reset(ds);
		for (k = from; k <= to; k++)
		{
			uint32		i;

			codes[k] = NULL;
			if (m->absent[k] || m->nslots[k] == 0)
				continue;
			codes[k] = (uint16 *) palloc(sizeof(uint16) * m->nslots[k]);
			for (i = 0; i < m->nslots[k]; i++)
			{
				const LionStoreVal *v = &m->slots[k][i];
				uint32		c = 0;

				if (v->data != NULL)
				{
					c = lion_store_dset_code(ds, v, true, NULL);
					if (c > LION_STORE_MAX_DICT)
						return false;
				}
				codes[k][i] = (uint16) c;
			}
		}
		width = lion_store_dict_width(ds->n);
		dictlen = lion_store_dict_len(col->typlen, ds->n, ds->bytes);
	}
	else
	{
		Assert(mode == LION_STORE_RAW && col->rawwidth > 0);
		width = col->rawwidth;
		dictlen = sizeof(LionStoreDict) + extralen;
	}

	need = LION_STORE_ITEM_COST(sizeof(LionStoreHeader)) +
		LION_STORE_ITEM_COST(dictlen);
	for (k = from; k <= to; k++)
		need += LION_STORE_ITEM_COST(lion_store_sub_len(mode, width,
														m->nslots[k],
														m->absent[k]));
	if (need > LION_PAGE_CAPACITY)
		return false;

	/* It fits: build the items. */
	memset(&img->hdr, 0, sizeof(LionStoreHeader));
	img->hdr.ckey = m->ckey;
	img->hdr.ord = m->ord;
	img->hdr.lo = (uint8) from;
	img->hdr.hi = (uint8) to;
	img->hdr.mode = (uint8) mode;
	img->hdr.flags = m->flags;
	img->hdr.width = (uint16) width;
	img->hdr.ndict = (mode == LION_STORE_DICT) ? (uint16) ds->n : 0;
	img->hdr.typlen = col->typlen;
	img->need = need;

	img->items[n] = (char *) palloc(sizeof(LionStoreHeader));
	memcpy(img->items[n], &img->hdr, sizeof(LionStoreHeader));
	img->lens[n++] = sizeof(LionStoreHeader);

	{
		char	   *d = (char *) palloc0(dictlen);
		LionStoreDict dh;

		dh.ndict = img->hdr.ndict;
		dh.nbytes = 0;
		if (mode == LION_STORE_DICT)
		{
			uint32		e;
			Size		pos = 0;

			for (e = 0; e < ds->n; e++)
			{
				const LionStoreVal *v = &ds->vals[e];

				if (col->typlen > 0)
				{
					Assert(v->len == (uint32) col->typlen);
					memcpy(d + sizeof(LionStoreDict) + pos, v->data, v->len);
					pos += v->len;
				}
				else
				{
					uint16		o;

					memcpy(d + sizeof(LionStoreDict) + pos, v->data, v->len);
					if (e == 0)
					{
						o = 0;
						memcpy(d + dictlen - sizeof(uint16) * (ds->n + 1), &o,
							   sizeof(uint16));
					}
					pos += v->len;
					o = (uint16) pos;
					memcpy(d + dictlen - sizeof(uint16) * (ds->n + 1) +
						   sizeof(uint16) * (e + 1), &o, sizeof(uint16));
				}
			}
			dh.nbytes = (uint16) pos;
		}
		else if (extralen > 0)
		{
			memcpy(d + sizeof(LionStoreDict), m->extra, extralen);
			dh.nbytes = (uint16) extralen;
		}
		memcpy(d, &dh, sizeof(LionStoreDict));
		img->items[n] = d;
		img->lens[n++] = dictlen;
	}

	for (k = from; k <= to; k++)
	{
		Size		len = lion_store_sub_len(mode, width, m->nslots[k],
											 m->absent[k]);
		char	   *s = (char *) palloc0(len);
		uint8	   *body = (uint8 *) s + sizeof(LionStoreSub);
		LionStoreSub sh;
		uint32		i;

		sh.nslots = m->absent[k] ? 0 : m->nslots[k];
		sh.flags = m->absent[k] ? LION_STORE_ABSENT : 0;
		memcpy(s, &sh, sizeof(LionStoreSub));
		if (!m->absent[k])
		{
			for (i = 0; i < m->nslots[k]; i++)
			{
				const LionStoreVal *v = &m->slots[k][i];

				if (mode == LION_STORE_DICT)
					lion_store_code_set(body, width, i, codes[k][i]);
				else if (v->data == NULL)
					lion_store_null_set(body + (Size) m->nslots[k] * width, i,
										true);
				else if (col->typlen > 0)
				{
					Assert(v->len == (uint32) col->typlen);
					memcpy(body + (Size) i * width, v->data, v->len);
				}
				else
				{
					uint16		vlen = (uint16) v->len;

					if ((int) v->len > width - (int) sizeof(uint16))
						elog(ERROR, "lion store: a value of %u bytes in a slot of %d",
							 v->len, width);
					memcpy(body + (Size) i * width, &vlen, sizeof(uint16));
					memcpy(body + (Size) i * width + sizeof(uint16), v->data,
						   v->len);
				}
			}
		}
		img->items[n] = s;
		img->lens[n++] = len;
	}
	img->nitems = n;

	return true;
}

/*
 * Decode a store page lion_store_page_check() accepted into m.  The values
 * point into `page`, which must stay as it is while m is used.
 */
static void
lion_store_decode(Relation index, const LionStoreCol *col, Page page,
				  BlockNumber blk, LionStoreModel *m)
{
	LionStoreHeader *h = lion_store_page_header(page);
	ItemId		diid = PageGetItemId(page, LION_STORE_DICT_OFF);
	const char *ditem = (const char *) PageGetItem(page, diid);
	Size		dlen = ItemIdGetLength(diid);
	int			k;

	memset(m, 0, sizeof(LionStoreModel));
	m->ckey = h->ckey;
	m->ord = h->ord;
	m->lo = h->lo;
	m->hi = h->hi;
	m->flags = h->flags;
	if (col->kind == LION_STORE_KIND_PERM && h->lo == 0)
	{
		LionStoreDict d;

		memcpy(&d, ditem, sizeof(LionStoreDict));
		m->extra = ditem + sizeof(LionStoreDict);
		m->extralen = d.nbytes;
	}

	for (k = h->lo; k <= h->hi; k++)
	{
		Size		len;
		LionStoreSub *s = lion_store_page_sub(page, k, &len);
		const uint8 *body = (const uint8 *) s + sizeof(LionStoreSub);
		uint32		i;

		if ((s->flags & LION_STORE_ABSENT) != 0)
		{
			m->absent[k] = true;
			continue;
		}
		m->nslots[k] = s->nslots;
		if (s->nslots == 0)
			continue;
		m->slots[k] = (LionStoreVal *) palloc0(sizeof(LionStoreVal) * s->nslots);
		for (i = 0; i < s->nslots; i++)
		{
			LionStoreVal *v = &m->slots[k][i];

			if (h->mode == LION_STORE_DICT)
			{
				uint32		c = lion_store_code_get(body, h->width, i);

				if (c == 0)
					continue;
				if (c > h->ndict)
					ereport(ERROR,
							(errcode(ERRCODE_INDEX_CORRUPTED),
							 errmsg("lion index \"%s\": store page %u has code %u of a dictionary of %u entries",
									RelationGetRelationName(index), blk, c,
									(unsigned) h->ndict),
							 errhint("REINDEX the index.")));
				v->data = lion_store_dict_entry(index, col, ditem, dlen,
												h->ndict, c, &v->len);
			}
			else
			{
				const uint8 *slot = body + (Size) i * h->width;

				if (lion_store_null_get(body + (Size) s->nslots * h->width, i))
					continue;
				if (col->typlen > 0)
				{
					v->data = (const char *) slot;
					v->len = (uint32) col->typlen;
				}
				else
				{
					uint16		vlen;

					memcpy(&vlen, slot, sizeof(uint16));
					if ((int) vlen > (int) h->width - (int) sizeof(uint16))
						ereport(ERROR,
								(errcode(ERRCODE_INDEX_CORRUPTED),
								 errmsg("lion index \"%s\": store page %u has a value of %u bytes in a slot of %u",
										RelationGetRelationName(index), blk,
										(unsigned) vlen, (unsigned) h->width),
								 errhint("REINDEX the index.")));
					v->data = (const char *) slot + sizeof(uint16);
					v->len = vlen;
				}
			}
		}
	}
}

/* Set slot `off` of heap page k of m to v, growing the page's slots. */
static void
lion_store_model_set(LionStoreModel *m, int k, OffsetNumber off,
					 const LionStoreVal *v)
{
	Assert(k >= m->lo && k <= m->hi && !m->absent[k]);
	if (off > m->nslots[k])
	{
		LionStoreVal *n = (LionStoreVal *) palloc0(sizeof(LionStoreVal) * off);

		if (m->nslots[k] > 0)
			memcpy(n, m->slots[k], sizeof(LionStoreVal) * m->nslots[k]);
		m->slots[k] = n;
		m->nslots[k] = off;
	}
	m->slots[k][off - 1] = *v;
}

/* Put an image on a page nothing else can see (the build's). */
static void
lion_store_place(Page page, const LionStoreImage *img, BlockNumber rightlink)
{
	LionPageOpaque opaque;
	int			i;

	lion_init_page(page, LION_PAGE_STORE);
	opaque = LionPageGetOpaque(page);
	opaque->rightlink = rightlink;
	lion_page_set_owner(page, (uint32) img->hdr.ord, (BlockNumber) img->hdr.ckey);
	for (i = 0; i < img->nitems; i++)
		if (PageAddItemExtended(page, img->items[i], img->lens[i],
								InvalidOffsetNumber, 0) !=
			(OffsetNumber) (i + 1))
			elog(ERROR, "lion store: could not place item %d of a page image", i + 1);
}

/*
 * The same inside an open record, on a page it registered as one it
 * initialises: INIT, then every item in one ADDMANY, then the special area
 * (DESIGN.md §40, "WAL").  The image's size was checked, so nothing here can
 * fail.
 */
static void
lion_store_log_image(LionWalState *xs, Page page, const LionStoreImage *img,
					 BlockNumber rightlink)
{
	int			i;

	lion_init_page(page, LION_PAGE_STORE);
	lion_wal_op(xs, page, LION_OP_INIT, 0, LION_PAGE_STORE, NULL, 0);
	LionPageGetOpaque(page)->rightlink = rightlink;
	lion_page_set_owner(page, (uint32) img->hdr.ord, (BlockNumber) img->hdr.ckey);
	lion_wal_op(xs, page, LION_OP_ADDMANY, FirstOffsetNumber,
				(uint16) img->nitems, NULL, 0);
	for (i = 0; i < img->nitems; i++)
	{
		uint16		isz = (uint16) img->lens[i];

		if (PageAddItemExtended(page, img->items[i], img->lens[i],
								InvalidOffsetNumber, 0) !=
			(OffsetNumber) (i + 1))
			elog(ERROR, "lion store: could not place item %d of a page image", i + 1);
		lion_wal_op_append(xs, page, &isz, sizeof(uint16));
		lion_wal_op_append(xs, page, img->items[i], img->lens[i]);
	}
	lion_wal_log_special(xs, page);
}

/* ---------------------------------------------------------------------
 * 4. The window map
 * --------------------------------------------------------------------- */

static const PGAlignedBlock lion_store_zero;

void
lion_storemap_init_page(Page page, uint16 level, uint32 number)
{
	LionPageOpaque opaque;

	lion_init_page(page, LION_PAGE_STOREMAP);
	opaque = LionPageGetOpaque(page);
	opaque->level = level;
	opaque->owner_head = (BlockNumber) number;
	opaque->owner_hash = 0;
	if (PageAddItemExtended(page, lion_store_zero.data, LION_STOREMAP_ITEM_SIZE,
							FirstOffsetNumber, 0) != FirstOffsetNumber)
		elog(ERROR, "lion store: could not place the item of a map page");
}

char *
lion_storemap_page_check(Page page, uint16 level, uint64 number)
{
	LionPageOpaque opaque;
	ItemId		iid;

	if (PageIsNew(page) || PageGetSpecialSize(page) != LION_SPECIAL_SIZE)
		return pstrdup("is not a page of the index");
	opaque = LionPageGetOpaque(page);
	if (opaque->page_id != LION_PAGE_ID ||
		(opaque->flags & (LION_PAGE_KINDS | LION_PAGE_DELETED)) !=
		LION_PAGE_STOREMAP)
		return psprintf("has flags 0x%04X", opaque->flags);
	if (opaque->level != level || (uint64) opaque->owner_head != number)
		return psprintf("is page %u of level %u", opaque->owner_head,
						(unsigned) opaque->level);
	if (PageGetMaxOffsetNumber(page) != FirstOffsetNumber)
		return psprintf("has %u items", (unsigned) PageGetMaxOffsetNumber(page));
	iid = PageGetItemId(page, FirstOffsetNumber);
	if (!ItemIdIsNormal(iid) || !ItemIdHasStorage(iid) ||
		ItemIdGetLength(iid) != LION_STOREMAP_ITEM_SIZE ||
		ItemIdGetOffset(iid) < ((PageHeader) page)->pd_upper ||
		ItemIdGetOffset(iid) + ItemIdGetLength(iid) >
		((PageHeader) page)->pd_special)
		return pstrdup("has a bad line pointer");
	return NULL;
}

/*
 * Is this page number `number` of `level` of the window map?  Map pages never
 * move and are never freed, so anything else is damage.
 */
static void
lion_storemap_check(Relation index, Page page, BlockNumber blk, uint16 level,
					uint64 number)
{
	char	   *msg = lion_storemap_page_check(page, level, number);

	if (msg != NULL)
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg("lion index \"%s\": block %u, page " UINT64_FORMAT " of level %u of the window map, %s",
						RelationGetRelationName(index), blk, number,
						(unsigned) level, msg),
				 errhint("REINDEX the index.")));
}

static inline BlockNumber *
lion_storemap_entries(Page page)
{
	return lion_storemap_page_entries(page);
}

/* Entry i of the map page blk, read under a SHARE lock; 0 reads as none. */
static BlockNumber
lion_storemap_read(Relation index, BlockNumber blk, uint16 level,
				   uint64 number, uint32 i)
{
	Buffer		buf = ReadBuffer(index, blk);
	Page		page;
	BlockNumber v;

	LockBuffer(buf, BUFFER_LOCK_SHARE);
	page = BufferGetPage(buf);
	lion_storemap_check(index, page, blk, level, number);
	v = lion_storemap_entries(page)[i];
	UnlockReleaseBuffer(buf);

	return (v == LION_METAPAGE_BLKNO) ? InvalidBlockNumber : v;
}

/*
 * The leaf page of the map that holds leaf number leafno, or InvalidBlockNumber
 * when the map has none yet.  cache, when given, remembers the inner and leaf
 * pages found: they never move.
 */
static BlockNumber
lion_storemap_leaf(Relation index, BlockNumber root, uint64 leafno,
				   LionStoreCache *cache)
{
	uint64		innerno = leafno / LION_STOREMAP_FANOUT;
	BlockNumber inner;
	BlockNumber leaf;

	if (innerno >= LION_STOREMAP_FANOUT)
		return InvalidBlockNumber;	/* past every window there can be */
	if (cache != NULL && BlockNumberIsValid(cache->leafblk) &&
		cache->leafno == leafno)
		return cache->leafblk;

	if (cache != NULL && BlockNumberIsValid(cache->innerblk) &&
		cache->innerno == innerno)
		inner = cache->innerblk;
	else
	{
		inner = lion_storemap_read(index, root, LION_STOREMAP_ROOT, 0,
								   (uint32) innerno);
		if (!BlockNumberIsValid(inner))
			return InvalidBlockNumber;
		if (cache != NULL)
		{
			cache->innerno = innerno;
			cache->innerblk = inner;
		}
	}

	leaf = lion_storemap_read(index, inner, LION_STOREMAP_INNER, innerno,
							  (uint32) (leafno % LION_STOREMAP_FANOUT));
	if (BlockNumberIsValid(leaf) && cache != NULL)
	{
		cache->leafno = leafno;
		cache->leafblk = leaf;
	}
	return leaf;
}

/* The head of the store chain of map slot `slot`, or InvalidBlockNumber. */
static BlockNumber
lion_storemap_head(Relation index, BlockNumber root, uint64 slot,
				   LionStoreCache *cache)
{
	uint64		leafno = slot / LION_STOREMAP_FANOUT;
	BlockNumber leaf = lion_storemap_leaf(index, root, leafno, cache);

	if (!BlockNumberIsValid(leaf))
		return InvalidBlockNumber;
	return lion_storemap_read(index, leaf, LION_STOREMAP_LEAF, leafno,
							  (uint32) (slot % LION_STOREMAP_FANOUT));
}

/*
 * The images a chain is written as: the build's, and a sort's (§41).  The
 * pages of heap (or virtual) pages 0 .. hi of m: DICT or RAW chosen once for
 * them all, then as many pages as the content needs, page by page.
 */
typedef struct LionStoreImages
{
	int			n;
	int			cap;
	LionStoreImage *imgs;
} LionStoreImages;

static void
lion_store_images_add(LionStoreImages *li, const LionStoreImage *img)
{
	if (li->n >= li->cap)
	{
		li->cap = Max(8, li->cap * 2);
		if (li->imgs == NULL)
			li->imgs = (LionStoreImage *) palloc(sizeof(LionStoreImage) * li->cap);
		else
			li->imgs = (LionStoreImage *) repalloc(li->imgs,
												   sizeof(LionStoreImage) * li->cap);
	}
	li->imgs[li->n++] = *img;
}

/*
 * What heap pages [start .. end] of m take on one page in `mode`, the
 * dictionary being what ds holds (DICT): the greedy fill's test.
 */
static Size
lion_store_group_need(const LionStoreCol *col, const LionStoreModel *m,
					  int start, int end, int mode, const LionStoreDictSet *ds)
{
	int			width;
	Size		need;
	int			k;

	if (mode == LION_STORE_DICT)
	{
		width = lion_store_dict_width(ds->n);
		if (width == 0)
			return SIZE_MAX;
		need = LION_STORE_ITEM_COST(lion_store_dict_len(col->typlen, ds->n,
														ds->bytes));
	}
	else
	{
		width = col->rawwidth;
		need = LION_STORE_ITEM_COST(sizeof(LionStoreDict) +
									lion_store_extra_len(m, start));
	}
	need += LION_STORE_ITEM_COST(sizeof(LionStoreHeader));
	for (k = start; k <= end; k++)
		need += LION_STORE_ITEM_COST(lion_store_sub_len(mode, width,
														m->nslots[k],
														m->absent[k]));
	return need;
}

/* Add heap page k's values to the dictionary set (DICT). */
static void
lion_store_dset_add_page(LionStoreDictSet *ds, const LionStoreModel *m, int k)
{
	uint32		i;

	for (i = 0; i < m->nslots[k]; i++)
		if (m->slots[k][i].data != NULL)
			(void) lion_store_dset_code(ds, &m->slots[k][i], true, NULL);
}

/*
 * Heap page k alone does not fit a page in `mode`: the other mode if the
 * column has it and it fits, else the page is ABSENT (DESIGN.md §40,
 * "Growth").
 */
static void
lion_store_layout_single(const LionStoreCol *col, LionStoreModel *m, int k,
						 int mode, LionStoreDictSet *ds, LionStoreImages *out)
{
	LionStoreImage img;
	int			other = (mode == LION_STORE_DICT) ? LION_STORE_RAW : LION_STORE_DICT;

	if (col->kind != LION_STORE_KIND_PERM &&
		(other == LION_STORE_DICT || col->rawwidth > 0) &&
		lion_store_encode(col, m, k, k, other, ds, &img))
	{
		lion_store_images_add(out, &img);
		return;
	}
	m->absent[k] = true;
	if (!lion_store_encode(col, m, k, k, mode, ds, &img))
		elog(ERROR, "lion store: an ABSENT heap page does not fit a page");
	lion_store_images_add(out, &img);
}

/* Lay heap (or virtual) pages 0 .. hi of m out as page images. */
static void
lion_store_layout(const LionStoreCol *col, LionStoreModel *m, int hi,
				  LionStoreDictSet *ds, LionStoreImages *out)
{
	LionStoreImage img;
	int			mode;
	int			start = 0;
	int			k = 0;

	m->lo = 0;
	m->hi = hi;
	mode = lion_store_choose_mode(col, m, 0, hi, ds);

	lion_store_dset_reset(ds);
	while (k <= hi)
	{
		if (mode == LION_STORE_DICT)
			lion_store_dset_add_page(ds, m, k);
		if (lion_store_group_need(col, m, start, k, mode, ds) <=
			LION_PAGE_CAPACITY)
		{
			k++;
			continue;
		}
		if (k > start)
		{
			/* close the page before k, and try k again on a page of its own */
			if (!lion_store_encode(col, m, start, k - 1, mode, ds, &img))
				elog(ERROR, "lion store: a build page does not fit");
			lion_store_images_add(out, &img);
			start = k;
			lion_store_dset_reset(ds);
			continue;
		}
		lion_store_layout_single(col, m, k, mode, ds, out);
		k++;
		start = k;
		lion_store_dset_reset(ds);
	}
	if (start <= hi)
	{
		if (!lion_store_encode(col, m, start, hi, mode, ds, &img))
			elog(ERROR, "lion store: a build page does not fit");
		lion_store_images_add(out, &img);
	}
}

/*
 * The comparator of an ordered index's order column (§41): its stored type's
 * default B-tree ordering, under the index column's collation, NULLs last.
 */
static void
lion_store_order_ssup(Relation index, LionIndexState *ix, SortSupport ssup,
					  MemoryContext cxt)
{
	const LionStoreCol *col = &ix->stored[ix->store_order];
	TypeCacheEntry *tce = lookup_type_cache(col->typid, TYPECACHE_LT_OPR);
	Oid			coll = TupleDescAttr(RelationGetDescr(index), col->attno - 1)->attcollation;

	if (!OidIsValid(tce->lt_opr))
		elog(ERROR, "lion index \"%s\": its order column has no ordering",
			 RelationGetRelationName(index));
	if (!OidIsValid(coll) && type_is_collatable(col->typid))
		coll = DEFAULT_COLLATION_OID;
	memset(ssup, 0, sizeof(SortSupportData));
	ssup->ssup_cxt = cxt;
	ssup->ssup_collation = coll;
	ssup->ssup_nulls_first = false;
	PrepareSortSupportFromOrderingOp(tce->lt_opr, ssup);
}

/* A row being sorted: the order column's value, and its heap lo. */
typedef struct LionStoreSortRow
{
	Datum		d;
	bool		isnull;
	uint16		lo;
} LionStoreSortRow;

static int
lion_store_sortrow_cmp(const void *a, const void *b, void *arg)
{
	const LionStoreSortRow *ra = (const LionStoreSortRow *) a;
	const LionStoreSortRow *rb = (const LionStoreSortRow *) b;
	int			c = ApplySortComparator(ra->d, ra->isnull, rb->d, rb->isnull,
										(SortSupport) arg);

	if (c != 0)
		return c;
	return (ra->lo < rb->lo) ? -1 : (ra->lo > rb->lo) ? 1 : 0;
}

/*
 * A sorted window (§41): its columns over virtual pages, its permutation, and
 * the window header being put together for the permutation's head.
 */
typedef struct LionStoreSorted
{
	int			n;				/* rows */
	int			vwidth;			/* V */
	int			nvpages;		/* virtual pages the sorted region uses */
	LionStoreModel *vm;			/* [nstored] */
	LionStoreModel pm;			/* the permutation, heap coordinates */
	int			ndir;
	int			maxdir;
	LionStoreDirEnt *dir;
} LionStoreSorted;

/*
 * Sort a window's rows (§41): hm[ord] holds each column's values in heap
 * coordinates, rows[0 .. n - 1] the heap lo of every row, ascending.  Fills
 * so with the virtual models and the permutation, generation gen, all in the
 * current memory context; the values are hm's, referenced.
 */
static void
lion_store_sort_window(LionIndexState *ix, SortSupport ssup, uint32 ckey,
					   LionStoreModel *hm, const uint16 *rows, int n, int gen,
					   LionStoreSorted *so)
{
	const LionStoreCol *ocol = &ix->stored[ix->store_order];
	LionStoreModel *om = &hm[ix->store_order];
	LionStoreSortRow *sr = (LionStoreSortRow *) palloc(sizeof(LionStoreSortRow) * n);
	uint16	   *permv;
	int			maxpage = -1;
	int			vw;
	int			i;
	int			ord;

	Assert(n > 0 && n <= LION_BLOCKS_PER_CONTAINER * MaxHeapTuplesPerPage);
	for (i = 0; i < n; i++)
	{
		uint16		k;
		OffsetNumber off;
		const LionStoreVal *v = NULL;

		lion_lo_split(rows[i], &k, &off);
		if (off <= om->nslots[k] && om->slots[k] != NULL)
			v = &om->slots[k][off - 1];
		sr[i].lo = rows[i];
		sr[i].isnull = (v == NULL || v->data == NULL);
		sr[i].d = sr[i].isnull ? (Datum) 0 :
			lion_store_datum(ocol, v->data, v->len, CurrentMemoryContext);
		maxpage = Max(maxpage, (int) k);
	}
	qsort_arg(sr, n, sizeof(LionStoreSortRow), lion_store_sortrow_cmp, ssup);

	vw = (n + LION_BLOCKS_PER_CONTAINER - 1) / LION_BLOCKS_PER_CONTAINER;
	so->n = n;
	so->vwidth = vw;
	so->nvpages = (n + vw - 1) / vw;
	so->vm = (LionStoreModel *) palloc0(sizeof(LionStoreModel) * ix->nstored);
	for (ord = 0; ord < ix->nstored; ord++)
	{
		LionStoreModel *vm = &so->vm[ord];
		int			p;

		vm->ckey = ckey;
		vm->ord = (uint16) ord;
		vm->flags = lion_store_make_flags(LION_STORE_F_VIRTUAL, gen);
		for (p = 0; p < so->nvpages; p++)
		{
			vm->nslots[p] = (uint16) Min(vw, n - p * vw);
			vm->slots[p] = (LionStoreVal *) palloc0(sizeof(LionStoreVal) * vm->nslots[p]);
		}
	}

	memset(&so->pm, 0, sizeof(LionStoreModel));
	so->pm.ckey = ckey;
	so->pm.ord = (uint16) ix->nstored;
	so->pm.flags = lion_store_make_flags(LION_STORE_F_PERM, gen);
	permv = (uint16 *) palloc(sizeof(uint16) * n);
	for (i = 0; i < n; i++)
	{
		uint16		k;
		OffsetNumber off;

		lion_lo_split(rows[i], &k, &off);
		if (off > so->pm.nslots[k])
			so->pm.nslots[k] = off;
	}
	for (i = 0; i <= maxpage; i++)
		if (so->pm.nslots[i] > 0)
			so->pm.slots[i] = (LionStoreVal *)
				palloc0(sizeof(LionStoreVal) * so->pm.nslots[i]);

	for (i = 0; i < n; i++)
	{
		uint16		k;
		OffsetNumber off;
		uint16		vlo = lion_store_sorted_vlo((uint32) i, (uint32) vw);
		int			p = lion_store_vlo_page(vlo);
		int			o = lion_store_vlo_off(vlo);

		lion_lo_split(sr[i].lo, &k, &off);
		for (ord = 0; ord < ix->nstored; ord++)
		{
			LionStoreModel *m = &hm[ord];

			if (off <= m->nslots[k] && m->slots[k] != NULL)
				so->vm[ord].slots[p][o - 1] = m->slots[k][off - 1];
		}
		permv[i] = vlo;
		so->pm.slots[k][off - 1].data = (const char *) &permv[i];
		so->pm.slots[k][off - 1].len = sizeof(uint16);
	}
	so->pm.lo = 0;
	so->pm.hi = maxpage;

	so->ndir = 0;
	so->maxdir = 64;
	so->dir = (LionStoreDirEnt *) palloc(sizeof(LionStoreDirEnt) * so->maxdir);
}

/* Name page image img, written at blk, in the directory if it is in the sorted region. */
static void
lion_store_sorted_dir(LionStoreSorted *so, const LionStoreImage *img,
					  BlockNumber blk)
{
	if (img->hdr.lo >= LION_STORE_APPEND_VPAGE)
		return;
	if (so->ndir >= so->maxdir)
	{
		so->maxdir *= 2;
		so->dir = (LionStoreDirEnt *) repalloc(so->dir,
											   sizeof(LionStoreDirEnt) * so->maxdir);
	}
	so->dir[so->ndir].ord = img->hdr.ord;
	so->dir[so->ndir].lo = img->hdr.lo;
	so->dir[so->ndir].blk = blk;
	so->ndir++;
}

/*
 * The window header (§41) the permutation's head carries, once every column's
 * pages are placed: the directory, thinned to every other entry of a column
 * until the head can hold it with the first heap page's permutation - a
 * directory entry is a shortcut, and a reader walks right from the one before
 * a page it lacks.
 */
static void
lion_store_sorted_header(LionStoreSorted *so)
{
	LionStoreWinHdr wh;
	char	   *buf;
	Size		budget = LION_PAGE_CAPACITY / 2;
	bool		thin = false;

	while (sizeof(LionStoreWinHdr) + (Size) so->ndir * sizeof(LionStoreDirEnt) > budget)
	{
		int			i;
		int			j = 0;

		/* keep each column's first page and every other one after it */
		for (i = 0; i < so->ndir; i++)
			if (i == 0 || so->dir[i].ord != so->dir[i - 1].ord || (i % 2) == 0)
				so->dir[j++] = so->dir[i];
		if (j == so->ndir)
			break;
		so->ndir = j;
		thin = true;
	}

	wh.nsorted = (uint16) so->n;
	wh.vwidth = (uint16) so->vwidth;
	wh.ndir = (uint16) so->ndir;
	wh.flags = thin ? LION_STORE_WH_THIN : 0;
	buf = (char *) palloc(sizeof(LionStoreWinHdr) + sizeof(LionStoreDirEnt) * so->ndir);
	memcpy(buf, &wh, sizeof(LionStoreWinHdr));
	memcpy(buf + sizeof(LionStoreWinHdr), so->dir,
		   sizeof(LionStoreDirEnt) * so->ndir);
	so->pm.extra = buf;
	so->pm.extralen = (uint16) (sizeof(LionStoreWinHdr) +
								sizeof(LionStoreDirEnt) * so->ndir);
}

/* ---------------------------------------------------------------------
 * 5. The build (DESIGN.md §40, "Build")
 *
 * Rows come in heap order, so a window's rows all arrive before the next
 * window's first: the current window's values are kept per stored column,
 * per heap page, per offset (offsets of one page may come in any order: HOT
 * chains are reported by their root), and when the first row of the next
 * window arrives each column's pages are written - DICT or RAW chosen once
 * from the whole window, then as many pages as the content needs, heap page
 * by heap page.  The map is written as it fills, leaf by leaf, and its inner
 * pages and root at the end.  An ordered index (§41) sorts the window's rows
 * at its close, lays each column out over the sorted region, and writes the
 * permutation after the columns, with the directory of their pages.
 * --------------------------------------------------------------------- */

struct LionStoreBuild
{
	Relation	index;
	LionIndexState *ix;
	BulkWriteState *bulk;
	BlockNumber *nblocks;		/* the build's block counter */
	MemoryContext cxt;			/* the build's own */
	MemoryContext wincxt;		/* the open window's values */
	MemoryContext tmpcxt;		/* per row, and per page image */

	bool		open;			/* a window is open */
	uint32		ckey;			/* ... this one */
	int			maxpage;		/* its last heap page with a row */
	LionStoreModel *models;		/* [nstored], the open window's */
	LionStoreDictSet ds;
	int			stride;			/* map slots a window takes */

	/* an ordered index (§41): its comparator, and the open window's rows */
	bool		ordered;
	SortSupportData ssup;
	uint8		present[LION_BLOCKS_PER_CONTAINER][(MaxHeapTuplesPerPage + 7) / 8];

	/* the map */
	bool		leafused;
	uint64		leafno;			/* the leaf being filled */
	BlockNumber *leaf;			/* ... its entries */
	BlockNumber *leafblks;		/* the leaves written, by number */
	uint64		nleafblks;
	uint64		maxleafblks;

	int64		pages;			/* store and map pages written */
	int64		sortedwindows;	/* windows written in key order */
};

LionStoreBuild *
lion_store_build_begin(Relation index, LionIndexState *ix, BulkWriteState *bulk,
					   BlockNumber *nblocks)
{
	MemoryContext cxt = AllocSetContextCreate(CurrentMemoryContext,
											  "lion store build",
											  ALLOCSET_DEFAULT_SIZES);
	LionStoreBuild *sb = (LionStoreBuild *)
		MemoryContextAllocZero(cxt, sizeof(LionStoreBuild));

	Assert(ix->nstored > 0);
	sb->index = index;
	sb->ix = ix;
	sb->bulk = bulk;
	sb->nblocks = nblocks;
	sb->cxt = cxt;
	sb->wincxt = AllocSetContextCreate(cxt, "lion store build window",
									   ALLOCSET_DEFAULT_SIZES);
	sb->tmpcxt = AllocSetContextCreate(cxt, "lion store build temporary",
									   ALLOCSET_DEFAULT_SIZES);
	sb->models = (LionStoreModel *)
		MemoryContextAllocZero(cxt, sizeof(LionStoreModel) * ix->nstored);
	lion_store_dset_init(&sb->ds, cxt);
	sb->stride = lion_store_stride(ix);
	sb->ordered = (ix->store_order >= 0);
	if (sb->ordered)
		lion_store_order_ssup(index, ix, &sb->ssup, cxt);
	sb->leaf = (BlockNumber *) MemoryContextAlloc(cxt, LION_STOREMAP_ITEM_SIZE);
	sb->maxleafblks = 16;
	sb->leafblks = (BlockNumber *)
		MemoryContextAlloc(cxt, sizeof(BlockNumber) * sb->maxleafblks);

	return sb;
}

/* A map page image of (level, number) with these entries, written now. */
static BlockNumber
lion_store_build_map_page(LionStoreBuild *sb, uint16 level, uint64 number,
						  const BlockNumber *entries)
{
	BlockNumber blk = (*sb->nblocks)++;
	BulkWriteBuffer buf = smgr_bulk_get_buf(sb->bulk);
	Page		page = (Page) buf->data;

	lion_storemap_init_page(page, level, (uint32) number);
	memcpy(lion_storemap_entries(page), entries, LION_STOREMAP_ITEM_SIZE);
	smgr_bulk_write(sb->bulk, blk, buf, true);
	sb->pages++;
	return blk;
}

static void
lion_store_build_flush_leaf(LionStoreBuild *sb)
{
	BlockNumber blk;

	if (!sb->leafused)
		return;
	blk = lion_store_build_map_page(sb, LION_STOREMAP_LEAF, sb->leafno, sb->leaf);
	while (sb->leafno >= sb->maxleafblks)
	{
		sb->maxleafblks *= 2;
		sb->leafblks = (BlockNumber *)
			repalloc(sb->leafblks, sizeof(BlockNumber) * sb->maxleafblks);
	}
	while (sb->nleafblks < sb->leafno)
		sb->leafblks[sb->nleafblks++] = InvalidBlockNumber;
	sb->leafblks[sb->nleafblks++] = blk;
	sb->leafused = false;
}

/* Record a chain's head in the map. */
static void
lion_store_build_map_set(LionStoreBuild *sb, uint64 slot, BlockNumber head)
{
	uint64		leafno = slot / LION_STOREMAP_FANOUT;

	Assert(!sb->leafused || leafno >= sb->leafno);
	if (leafno / LION_STOREMAP_FANOUT >= LION_STOREMAP_FANOUT)
		elog(ERROR, "lion store: window map slot " UINT64_FORMAT " is out of range", slot);
	if (sb->leafused && leafno != sb->leafno)
		lion_store_build_flush_leaf(sb);
	if (!sb->leafused)
	{
		memset(sb->leaf, 0, LION_STOREMAP_ITEM_SIZE);
		sb->leafno = leafno;
		sb->leafused = true;
	}
	sb->leaf[slot % LION_STOREMAP_FANOUT] = head;
}

/*
 * Write one window's chain for col, the pages of m's heap (or virtual) pages
 * 0 .. hi, through the bulk writer on consecutive blocks, each linked to the
 * next, and point the map at its head.  A sorted window's pages go into its
 * directory (so).
 */
static void
lion_store_build_emit(LionStoreBuild *sb, const LionStoreCol *col,
					  LionStoreModel *m, int hi, LionStoreSorted *so)
{
	MemoryContext old = MemoryContextSwitchTo(sb->tmpcxt);
	LionStoreImages li;
	BlockNumber first;
	int			i;

	memset(&li, 0, sizeof(li));
	m->ckey = sb->ckey;
	m->ord = (uint16) col->ord;
	lion_store_layout(col, m, hi, &sb->ds, &li);
	Assert(li.n > 0);

	first = *sb->nblocks;
	*sb->nblocks += (BlockNumber) li.n;
	for (i = 0; i < li.n; i++)
	{
		BulkWriteBuffer buf = smgr_bulk_get_buf(sb->bulk);

		lion_store_place((Page) buf->data, &li.imgs[i],
						 (i + 1 < li.n) ? first + i + 1 : InvalidBlockNumber);
		smgr_bulk_write(sb->bulk, first + i, buf, true);
		if (so != NULL && col->kind == LION_STORE_KIND_ORDERED)
			lion_store_sorted_dir(so, &li.imgs[i], first + i);
		sb->pages++;
	}
	lion_store_build_map_set(sb, lion_storemap_slot(sb->ckey, sb->stride, col->ord),
							 first);

	MemoryContextSwitchTo(old);
	MemoryContextReset(sb->tmpcxt);
}

/* Close an ordered index's window: sort it, then write it (§41). */
static void
lion_store_build_close_sorted(LionStoreBuild *sb)
{
	LionIndexState *ix = sb->ix;
	MemoryContext old = MemoryContextSwitchTo(sb->wincxt);
	uint16	   *rows;
	int			n = 0;
	int			k;
	int			ord;
	LionStoreSorted so;

	rows = (uint16 *) palloc(sizeof(uint16) * (sb->maxpage + 1) * MaxHeapTuplesPerPage);
	for (k = 0; k <= sb->maxpage; k++)
	{
		int			off;

		for (off = FirstOffsetNumber; off <= MaxHeapTuplesPerPage; off++)
			if ((sb->present[k][(off - 1) / 8] & (1 << ((off - 1) % 8))) != 0)
				rows[n++] = lion_store_vlo(k, off);
	}
	lion_store_sort_window(ix, &sb->ssup, sb->ckey, sb->models, rows, n, 1, &so);

	for (ord = 0; ord < ix->nstored; ord++)
		lion_store_build_emit(sb, &ix->stored[ord], &so.vm[ord],
							  so.nvpages - 1, &so);
	MemoryContextSwitchTo(sb->wincxt);
	lion_store_sorted_header(&so);
	lion_store_build_emit(sb, ix->storeperm, &so.pm, so.pm.hi, NULL);
	sb->sortedwindows++;
	MemoryContextSwitchTo(old);
}

static void
lion_store_build_close_window(LionStoreBuild *sb)
{
	int			ord;

	if (!sb->open)
		return;
	if (sb->ordered)
		lion_store_build_close_sorted(sb);
	else
		for (ord = 0; ord < sb->ix->nstored; ord++)
			lion_store_build_emit(sb, &sb->ix->stored[ord], &sb->models[ord],
								  sb->maxpage, NULL);
	MemoryContextReset(sb->wincxt);
	sb->open = false;
}

void
lion_store_build_add(LionStoreBuild *sb, ItemPointer tid, const Datum *values,
					 const bool *isnull)
{
	uint64		code;
	uint32		ckey;
	uint16		k;
	OffsetNumber off;
	MemoryContext old;
	int			ord;

	lion_check_key_offset(tid);
	code = lion_tid_to_code(tid);
	ckey = lion_code_ckey(code);
	lion_lo_split(lion_code_lo(code), &k, &off);
	if (off < FirstOffsetNumber || off > MaxHeapTuplesPerPage)
		elog(ERROR, "lion store: heap offset %u out of range", off);

	if (sb->open && ckey != sb->ckey)
	{
		if (ckey < sb->ckey)
			elog(ERROR, "lion store: build rows out of heap order (window %u after %u)",
				 ckey, sb->ckey);
		lion_store_build_close_window(sb);
	}
	if (!sb->open)
	{
		memset(sb->models, 0, sizeof(LionStoreModel) * sb->ix->nstored);
		if (sb->ordered)
			memset(sb->present, 0, sizeof(sb->present));
		sb->open = true;
		sb->ckey = ckey;
		sb->maxpage = -1;
	}

	MemoryContextReset(sb->tmpcxt);
	old = MemoryContextSwitchTo(sb->tmpcxt);
	for (ord = 0; ord < sb->ix->nstored; ord++)
	{
		const LionStoreCol *col = &sb->ix->stored[ord];
		LionStoreModel *m = &sb->models[ord];
		LionStoreVal v;
		Datum		fix;

		lion_store_value(sb->index, col, values[col->attno - 1],
						 isnull[col->attno - 1], &v, &fix);
		if (v.data != NULL)
		{
			char	   *copy = MemoryContextAlloc(sb->wincxt, Max(v.len, 1));

			memcpy(copy, v.data, v.len);
			v.data = copy;
		}
		if (m->slots[k] == NULL)
			m->slots[k] = (LionStoreVal *)
				MemoryContextAllocZero(sb->wincxt,
									   sizeof(LionStoreVal) * MaxHeapTuplesPerPage);
		m->slots[k][off - 1] = v;
		if (off > m->nslots[k])
			m->nslots[k] = off;
	}
	if (sb->ordered)
		sb->present[k][(off - 1) / 8] |= (uint8) (1 << ((off - 1) % 8));
	MemoryContextSwitchTo(old);

	if ((int) k > sb->maxpage)
		sb->maxpage = k;
}

void
lion_store_build_finish(LionStoreBuild *sb, LionMetaStore *store)
{
	BlockNumber *entries;
	BlockNumber root;
	uint64		ninner;
	uint64		innerno;

	lion_store_build_close_window(sb);
	lion_store_build_flush_leaf(sb);

	/* the inner pages, then the root, which is always written */
	entries = (BlockNumber *) MemoryContextAllocZero(sb->cxt, LION_STOREMAP_ITEM_SIZE);
	{
		BlockNumber *rootents = (BlockNumber *)
			MemoryContextAllocZero(sb->cxt, LION_STOREMAP_ITEM_SIZE);

		ninner = (sb->nleafblks + LION_STOREMAP_FANOUT - 1) / LION_STOREMAP_FANOUT;
		for (innerno = 0; innerno < ninner; innerno++)
		{
			bool		any = false;
			uint32		i;

			memset(entries, 0, LION_STOREMAP_ITEM_SIZE);
			for (i = 0; i < LION_STOREMAP_FANOUT; i++)
			{
				uint64		leafno = innerno * LION_STOREMAP_FANOUT + i;

				if (leafno < sb->nleafblks &&
					BlockNumberIsValid(sb->leafblks[leafno]))
				{
					entries[i] = sb->leafblks[leafno];
					any = true;
				}
			}
			if (any)
				rootents[innerno] = lion_store_build_map_page(sb, LION_STOREMAP_INNER,
															  innerno, entries);
		}
		root = lion_store_build_map_page(sb, LION_STOREMAP_ROOT, 0, rootents);
	}

	*store = sb->ix->store;
	store->store_root = root;
	store->store_pages = (uint32) Min(sb->pages, (int64) PG_UINT32_MAX);
	sb->ix->store.store_root = root;

	MemoryContextDelete(sb->cxt);
}

/* ---------------------------------------------------------------------
 * 6. The insert path (DESIGN.md §40, "Writes")
 *
 * For each stored column: find the window's chain through the map (creating
 * its head when there is none), walk it to the page whose range holds the
 * heap page, and write the slot.  The write is in place when the page has
 * room - SETBYTES for a slot inside a sub-array, DELTA for a sub-array or a
 * dictionary that grew, ADD for the sub-arrays of heap pages past the range
 * of the chain's last page - and otherwise structural: the page is decoded,
 * the write applied to the model and the model encoded again, which compacts
 * the dictionary and widens the codes; a DICT page that then does not fit
 * goes RAW when that fits; a page that still does not fit is split (or, past
 * the chain's end, a page is appended) and the write starts again; and a page
 * of one heap page that fits in no mode marks that heap page ABSENT.
 * --------------------------------------------------------------------- */

/* The meta page, EXCLUSIVE, for a record that changes the store's count. */
static Buffer
lion_store_lock_meta(Relation index)
{
	Buffer		buf = ReadBuffer(index, LION_METAPAGE_BLKNO);
	Page		page;

	LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
	page = BufferGetPage(buf);
	if (PageIsNew(page) || PageGetSpecialSize(page) != LION_SPECIAL_SIZE ||
		!LionPageIsMeta(page) || !LionMetaHasStoreArea(page) ||
		((PageHeader) page)->pd_lower > ((PageHeader) page)->pd_upper)
	{
		UnlockReleaseBuffer(buf);
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg("lion index \"%s\": the meta page has no store record",
						RelationGetRelationName(index)),
				 errhint("REINDEX the index.")));
	}
	return buf;
}

/*
 * Count delta store or map pages on the meta page, in the open record
 * (LION_OP_STORE_META): store_pages is kept exact by the records that add
 * and free the pages, so that nothing has to count them.
 */
static void
lion_store_meta_count(LionWalState *xs, Buffer metabuf, int delta)
{
	Page		p = lion_wal_register_buffer(xs, metabuf, LION_WALBUF_STD);
	LionMetaStore *ms = LionPageGetMetaStore(p);
	LionMetaStore copy;

	if (delta < 0 && ms->store_pages < (uint32) -delta)
		ms->store_pages = 0;
	else
		ms->store_pages = (uint32) ((int64) ms->store_pages + delta);
	memcpy(&copy, ms, sizeof(LionMetaStore));
	lion_wal_op(xs, p, LION_OP_STORE_META, 0, 0, &copy, sizeof(LionMetaStore));
}

/*
 * Add map page (level, number) under entry i of its parent, unless another
 * backend has meanwhile: one record of three blocks - the new page, the
 * parent's SETBYTES, the meta page's count.
 */
static void
lion_storemap_add(Relation index, Relation heaprel, BlockNumber parent,
				  uint16 plevel, uint64 pnumber, uint32 i, uint16 level,
				  uint64 number)
{
	Buffer		nbuf = lion_alloc_page(index, heaprel, true);
	BlockNumber nblk = BufferGetBlockNumber(nbuf);
	Buffer		pbuf = ReadBuffer(index, parent);
	Buffer		metabuf;
	Page		page;
	LionWalState *xs;
	Page		pN;
	Page		pP;

	LockBuffer(pbuf, BUFFER_LOCK_EXCLUSIVE);
	page = BufferGetPage(pbuf);
	lion_storemap_check(index, page, parent, plevel, pnumber);
	if (lion_storemap_entries(page)[i] != 0)
	{
		UnlockReleaseBuffer(pbuf);
		lion_release_unused_page(index, nbuf);
		return;
	}
	metabuf = lion_store_lock_meta(index);

	xs = lion_wal_begin(index);
	pN = lion_wal_register_buffer(xs, nbuf, LION_WALBUF_INIT);
	lion_storemap_init_page(pN, level, (uint32) number);
	lion_wal_op(xs, pN, LION_OP_INIT, 0, LION_PAGE_STOREMAP, NULL, 0);
	lion_wal_op(xs, pN, LION_OP_ADD, FirstOffsetNumber, 0, lion_store_zero.data,
				LION_STOREMAP_ITEM_SIZE);
	lion_wal_log_special(xs, pN);

	pP = lion_wal_register_buffer(xs, pbuf, LION_WALBUF_STD);
	lion_storemap_entries(pP)[i] = nblk;
	lion_wal_op(xs, pP, LION_OP_SETBYTES, FirstOffsetNumber,
				(uint16) (i * sizeof(BlockNumber)), &nblk, sizeof(BlockNumber));

	lion_store_meta_count(xs, metabuf, 1);
	lion_wal_finish(xs, LION_XLOG_STORE);

	UnlockReleaseBuffer(nbuf);
	UnlockReleaseBuffer(pbuf);
	UnlockReleaseBuffer(metabuf);
}

/* The leaf of leafno, adding it and its inner page as needed. */
static BlockNumber
lion_storemap_extend(Relation index, Relation heaprel, LionIndexState *ix,
					 uint64 leafno)
{
	BlockNumber root = ix->store.store_root;
	uint64		innerno = leafno / LION_STOREMAP_FANOUT;
	int			attempts = 0;

	if (innerno >= LION_STOREMAP_FANOUT)
		elog(ERROR, "lion store: window map leaf " UINT64_FORMAT " is out of range",
			 leafno);
	for (;;)
	{
		BlockNumber leaf = lion_storemap_leaf(index, root, leafno, ix->storecache);
		BlockNumber inner;

		if (BlockNumberIsValid(leaf))
			return leaf;
		if (++attempts > 3)
			elog(ERROR, "lion store: could not extend the window map");

		inner = lion_storemap_read(index, root, LION_STOREMAP_ROOT, 0,
								   (uint32) innerno);
		if (!BlockNumberIsValid(inner))
			lion_storemap_add(index, heaprel, root, LION_STOREMAP_ROOT, 0,
							  (uint32) innerno, LION_STOREMAP_INNER, innerno);
		else
			lion_storemap_add(index, heaprel, inner, LION_STOREMAP_INNER,
							  innerno, (uint32) (leafno % LION_STOREMAP_FANOUT),
							  LION_STOREMAP_LEAF, leafno);
	}
}

/*
 * Create the head of window ckey's chain for col, holding the one value being
 * written, and point the map slot at it, in one record (the page, the leaf,
 * the meta page).  false when another backend created it first.
 */
static bool
lion_store_create_head(Relation index, Relation heaprel, LionIndexState *ix,
					   const LionStoreCol *col, uint32 ckey, int k,
					   OffsetNumber off, const LionStoreVal *v, uint64 slot,
					   uint8 flags)
{
	uint64		leafno = slot / LION_STOREMAP_FANOUT;
	uint32		i = (uint32) (slot % LION_STOREMAP_FANOUT);
	BlockNumber leafblk = lion_storemap_extend(index, heaprel, ix, leafno);
	LionStoreModel m;
	LionStoreDictSet ds;
	LionStoreImage img;
	Buffer		sbuf;
	BlockNumber sblk;
	Buffer		lbuf;
	Buffer		metabuf;
	Page		page;
	LionWalState *xs;
	Page		pS;
	Page		pL;

	memset(&m, 0, sizeof(m));
	m.ckey = ckey;
	m.ord = (uint16) col->ord;
	m.lo = 0;
	m.hi = k;
	m.flags = flags;
	lion_store_model_set(&m, k, off, v);
	lion_store_dset_init(&ds, CurrentMemoryContext);
	if (!lion_store_encode(col, &m, 0, k, LION_STORE_DICT, &ds, &img))
		elog(ERROR, "lion store: a new window's first value does not fit a page");

	sbuf = lion_alloc_page(index, heaprel, true);
	sblk = BufferGetBlockNumber(sbuf);
	lbuf = ReadBuffer(index, leafblk);
	LockBuffer(lbuf, BUFFER_LOCK_EXCLUSIVE);
	page = BufferGetPage(lbuf);
	lion_storemap_check(index, page, leafblk, LION_STOREMAP_LEAF, leafno);
	if (lion_storemap_entries(page)[i] != 0)
	{
		UnlockReleaseBuffer(lbuf);
		lion_release_unused_page(index, sbuf);
		return false;
	}
	metabuf = lion_store_lock_meta(index);

	xs = lion_wal_begin(index);
	pS = lion_wal_register_buffer(xs, sbuf, LION_WALBUF_INIT);
	lion_store_log_image(xs, pS, &img, InvalidBlockNumber);
	pL = lion_wal_register_buffer(xs, lbuf, LION_WALBUF_STD);
	lion_storemap_entries(pL)[i] = sblk;
	lion_wal_op(xs, pL, LION_OP_SETBYTES, FirstOffsetNumber,
				(uint16) (i * sizeof(BlockNumber)), &sblk, sizeof(BlockNumber));
	lion_store_meta_count(xs, metabuf, 1);
	lion_wal_finish(xs, LION_XLOG_STORE);

	UnlockReleaseBuffer(sbuf);
	UnlockReleaseBuffer(lbuf);
	UnlockReleaseBuffer(metabuf);

	ix->storecache->slot = slot;
	ix->storecache->head = sblk;
	return true;
}

/*
 * Step from blk to its right link next, at most a window's worth of pages:
 * every page of a chain covers at least one of the window's heap pages.
 */
static void
lion_store_chain_step(Relation index, BlockNumber blk, BlockNumber next,
					  int *steps)
{
	if (next == blk || next == LION_METAPAGE_BLKNO ||
		++(*steps) >= LION_STORE_MAX_VPAGES)
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg("lion index \"%s\": store page %u links to %u after %d pages of one window",
						RelationGetRelationName(index), blk, next, *steps),
				 errhint("REINDEX the index.")));
}

/*
 * The page of the chain at head whose range holds heap page k, or the chain's
 * last page when k is past every range, EXCLUSIVE; InvalidBuffer when the
 * chain is not (any longer) window ckey's, which sends the caller back to the
 * map.
 */
static Buffer
lion_store_find_page(Relation index, const LionStoreCol *col, uint32 ckey,
					 BlockNumber head, int k)
{
	BlockNumber blk = head;
	int			steps = 0;

	for (;;)
	{
		Buffer		buf = ReadBuffer(index, blk);
		Page		page;
		LionStoreHeader *h;
		BlockNumber next;

		LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
		page = BufferGetPage(buf);
		if (!lion_store_page_owned(page, ckey, (uint16) col->ord))
		{
			UnlockReleaseBuffer(buf);
			return InvalidBuffer;
		}
		lion_store_check_or_error(index, page, blk, col, false);
		h = lion_store_page_header(page);

		/*
		 * A head's range starts at the window's first heap page; a page that
		 * does not is one of the window's later pages, reused as such after
		 * the chain the map named was freed (§18).  Walking right never
		 * overshoots, because ranges only ever move right.
		 */
		if ((blk == head && h->lo != 0) || k < h->lo)
		{
			UnlockReleaseBuffer(buf);
			return InvalidBuffer;
		}
		next = LionPageGetOpaque(page)->rightlink;
		if (k <= h->hi || !BlockNumberIsValid(next))
		{
			lion_store_check_or_error(index, page, blk, col, true);
			return buf;
		}
		UnlockReleaseBuffer(buf);
		lion_store_chain_step(index, blk, next, &steps);
		blk = next;
	}
}

/* Rewrite the page in buf (EXCLUSIVE) as img, keeping its right link. */
static void
lion_store_rewrite(Relation index, Buffer buf, const LionStoreImage *img)
{
	BlockNumber rightlink = LionPageGetOpaque(BufferGetPage(buf))->rightlink;
	LionWalState *xs = lion_wal_begin(index);
	Page		p = lion_wal_register_buffer(xs, buf, LION_WALBUF_INIT);

	lion_store_log_image(xs, p, img, rightlink);
	lion_wal_finish(xs, LION_XLOG_STORE);
}

/*
 * Split the page in buf (EXCLUSIVE), whose content is m: heap pages from `at`
 * on move to a new page that takes its right link, both halves encoded in
 * `mode` (two blocks and the meta page, one record).  Each half is part of a
 * page that fits, so each fits.
 */
static void
lion_store_split(Relation index, Relation heaprel, Buffer buf,
				 const LionStoreCol *col, const LionStoreModel *m, int at,
				 int mode)
{
	BlockNumber rightlink = LionPageGetOpaque(BufferGetPage(buf))->rightlink;
	LionStoreDictSet ds;
	LionStoreImage left;
	LionStoreImage right;
	Buffer		nbuf;
	BlockNumber nblk;
	Buffer		metabuf;
	LionWalState *xs;
	Page		p;

	Assert(at > m->lo && at <= m->hi);
	lion_store_dset_init(&ds, CurrentMemoryContext);
	if (!lion_store_encode(col, m, m->lo, at - 1, mode, &ds, &left) ||
		!lion_store_encode(col, m, at, m->hi, mode, &ds, &right))
		elog(ERROR, "lion store: half of a store page does not fit a page");

	nbuf = lion_alloc_page(index, heaprel, true);
	nblk = BufferGetBlockNumber(nbuf);
	metabuf = lion_store_lock_meta(index);

	xs = lion_wal_begin(index);
	p = lion_wal_register_buffer(xs, buf, LION_WALBUF_INIT);
	lion_store_log_image(xs, p, &left, nblk);
	p = lion_wal_register_buffer(xs, nbuf, LION_WALBUF_INIT);
	lion_store_log_image(xs, p, &right, rightlink);
	lion_store_meta_count(xs, metabuf, 1);
	lion_wal_finish(xs, LION_XLOG_STORE);

	UnlockReleaseBuffer(nbuf);
	UnlockReleaseBuffer(metabuf);
}

/*
 * The chain's last page, in buf (EXCLUSIVE), has no room for heap pages
 * [from .. to] past its range: they go to a new last page of their own,
 * empty, in `mode`.
 */
static void
lion_store_append(Relation index, Relation heaprel, Buffer buf,
				  const LionStoreCol *col, uint32 ckey, int from, int to,
				  int mode)
{
	LionStoreModel e;
	LionStoreDictSet ds;
	LionStoreImage img;
	Buffer		nbuf;
	BlockNumber nblk;
	Buffer		metabuf;
	LionWalState *xs;
	Page		p;

	Assert(!BlockNumberIsValid(LionPageGetOpaque(BufferGetPage(buf))->rightlink));
	memset(&e, 0, sizeof(e));
	e.ckey = ckey;
	e.ord = (uint16) col->ord;
	e.lo = from;
	e.hi = to;
	e.flags = lion_store_page_header(BufferGetPage(buf))->flags;
	lion_store_dset_init(&ds, CurrentMemoryContext);
	if (!lion_store_encode(col, &e, from, to, mode, &ds, &img))
		elog(ERROR, "lion store: an empty store page does not fit a page");

	nbuf = lion_alloc_page(index, heaprel, true);
	nblk = BufferGetBlockNumber(nbuf);
	metabuf = lion_store_lock_meta(index);

	xs = lion_wal_begin(index);
	p = lion_wal_register_buffer(xs, buf, LION_WALBUF_STD);
	LionPageGetOpaque(p)->rightlink = nblk;
	lion_wal_log_special(xs, p);
	p = lion_wal_register_buffer(xs, nbuf, LION_WALBUF_INIT);
	lion_store_log_image(xs, p, &img, InvalidBlockNumber);
	lion_store_meta_count(xs, metabuf, 1);
	lion_wal_finish(xs, LION_XLOG_STORE);

	UnlockReleaseBuffer(nbuf);
	UnlockReleaseBuffer(metabuf);
}

/*
 * Write v at (k, off) in place on the page in buf (EXCLUSIVE, checked), if it
 * has the room and needs no structural change; false, with nothing changed,
 * when it does.
 */
static bool
lion_store_write_inplace(Relation index, Buffer buf, const LionStoreCol *col,
						 int k, OffsetNumber off, const LionStoreVal *v)
{
	Page		page = BufferGetPage(buf);
	LionStoreHeader h;
	ItemId		diid = PageGetItemId(page, LION_STORE_DICT_OFF);
	const char *ditem = (const char *) PageGetItem(page, diid);
	Size		dlen = ItemIdGetLength(diid);
	bool		extend;
	OffsetNumber subno = InvalidOffsetNumber;
	const char *oldsub = NULL;
	Size		oldsublen = 0;
	uint16		oldnslots = 0;
	uint16		newnslots;
	Size		newsublen;
	char	   *newsub;
	uint8	   *body;
	char	   *newdict = NULL;
	Size		newdictlen = 0;
	uint32		code = 0;
	Size		need = 0;
	LionWalState *xs;
	Page		p;
	int			j;

	memcpy(&h, lion_store_page_header(page), sizeof(LionStoreHeader));
	extend = (k > h.hi);
	if (!extend)
	{
		LionStoreSub s;

		subno = (OffsetNumber) (LION_STORE_SUB_FIRST + k - h.lo);
		oldsub = (const char *) PageGetItem(page, PageGetItemId(page, subno));
		oldsublen = ItemIdGetLength(PageGetItemId(page, subno));
		memcpy(&s, oldsub, sizeof(LionStoreSub));
		if ((s.flags & LION_STORE_ABSENT) != 0)
			return true;		/* readers take this heap page from the heap */
		oldnslots = s.nslots;
	}
	newnslots = Max(oldnslots, off);

	if (h.mode == LION_STORE_DICT && v->data != NULL)
	{
		code = lion_store_dict_find(index, col, ditem, dlen, h.ndict, v);
		if (code == 0)
		{
			uint32		nd = (uint32) h.ndict + 1;
			LionStoreDict d;

			/* a wider code is a rewrite of every sub-array */
			if (lion_store_dict_width(nd) != h.width)
				return false;
			memcpy(&d, ditem, sizeof(LionStoreDict));
			if ((Size) d.nbytes + v->len > PG_UINT16_MAX)
				return false;
			newdictlen = lion_store_dict_len(col->typlen, nd, d.nbytes + v->len);
			need += MAXALIGN(newdictlen) - MAXALIGN(dlen);
			code = nd;

			/* the dictionary with v appended: values first, offsets last */
			newdict = (char *) palloc0(newdictlen);
			d.ndict = (uint16) nd;
			if (col->typlen > 0)
			{
				memcpy(newdict, ditem, dlen);
				memcpy(newdict + dlen, v->data, v->len);
				d.nbytes = (uint16) (d.nbytes + v->len);
			}
			else
			{
				uint32		e;
				uint16		o;

				memcpy(newdict + sizeof(LionStoreDict),
					   ditem + sizeof(LionStoreDict), d.nbytes);
				memcpy(newdict + sizeof(LionStoreDict) + d.nbytes, v->data,
					   v->len);
				for (e = 0; e < nd; e++)
				{
					o = (uint16) lion_store_vdict_off(ditem, dlen, h.ndict, e);
					memcpy(newdict + newdictlen - sizeof(uint16) * (nd + 1) +
						   sizeof(uint16) * e, &o, sizeof(uint16));
				}
				o = (uint16) (d.nbytes + v->len);
				memcpy(newdict + newdictlen - sizeof(uint16), &o, sizeof(uint16));
				d.nbytes = o;
			}
			memcpy(newdict, &d, sizeof(LionStoreDict));
		}
	}

	newsublen = lion_store_sub_len(h.mode, h.width, newnslots, false);
	if (extend)
		need += (Size) (k - h.hi - 1) * LION_STORE_ITEM_COST(sizeof(LionStoreSub)) +
			LION_STORE_ITEM_COST(newsublen);
	else
		need += MAXALIGN(newsublen) - MAXALIGN(oldsublen);
	if (need > PageGetExactFreeSpace(page))
		return false;

	/* the sub-array as it will be */
	newsub = (char *) palloc0(newsublen);
	body = (uint8 *) newsub + sizeof(LionStoreSub);
	{
		LionStoreSub s;

		s.nslots = newnslots;
		s.flags = 0;
		memcpy(newsub, &s, sizeof(LionStoreSub));
	}
	if (h.mode == LION_STORE_DICT)
	{
		if (oldnslots > 0)
			memcpy(body, oldsub + sizeof(LionStoreSub),
				   lion_store_codes_bytes(h.width, oldnslots));
		lion_store_code_set(body, h.width, off - 1, code);
	}
	else
	{
		uint8	   *bitmap = body + (Size) newnslots * h.width;
		uint8	   *slot = body + (Size) (off - 1) * h.width;
		uint32		i;

		if (oldnslots > 0)
		{
			memcpy(body, oldsub + sizeof(LionStoreSub), (Size) oldnslots * h.width);
			memcpy(bitmap, oldsub + sizeof(LionStoreSub) + (Size) oldnslots * h.width,
				   lion_store_bitmap_bytes(oldnslots));
		}
		for (i = oldnslots; i < newnslots; i++)
			lion_store_null_set(bitmap, i, true);
		memset(slot, 0, h.width);
		if (v->data == NULL)
			lion_store_null_set(bitmap, off - 1, true);
		else
		{
			lion_store_null_set(bitmap, off - 1, false);
			if (col->typlen > 0)
				memcpy(slot, v->data, v->len);
			else
			{
				uint16		vlen = (uint16) v->len;

				Assert((int) v->len <= (int) h.width - (int) sizeof(uint16));
				memcpy(slot, &vlen, sizeof(uint16));
				memcpy(slot + sizeof(uint16), v->data, v->len);
			}
		}
	}

	/* the same value written again changes nothing */
	if (!extend && newdict == NULL && newsublen == oldsublen &&
		memcmp(newsub, oldsub, oldsublen) == 0)
		return true;

	xs = lion_wal_begin(index);
	p = lion_wal_register_buffer(xs, buf, LION_WALBUF_STD);
	if (newdict != NULL || extend)
	{
		LionStoreHeader *ph = lion_store_page_header(p);

		if (newdict != NULL)
			ph->ndict = (uint16) code;
		if (extend)
			ph->hi = (uint8) k;
		lion_wal_op(xs, p, LION_OP_SETBYTES, LION_STORE_HDR_OFF, 0, ph,
					sizeof(LionStoreHeader));
	}
	if (newdict != NULL)
	{
		lion_wal_save_item(xs, p, LION_STORE_DICT_OFF);
		if (!PageIndexTupleOverwrite(p, LION_STORE_DICT_OFF, newdict, newdictlen))
			elog(ERROR, "lion store: could not grow a dictionary");
		lion_wal_op_replace(xs, p, LION_STORE_DICT_OFF, newdict, newdictlen);
	}
	if (extend)
	{
		LionStoreSub empty;

		empty.nslots = 0;
		empty.flags = 0;
		for (j = h.hi + 1; j <= k; j++)
		{
			OffsetNumber o = OffsetNumberNext(PageGetMaxOffsetNumber(p));
			const char *item = (j == k) ? newsub : (const char *) &empty;
			Size		len = (j == k) ? newsublen : sizeof(LionStoreSub);

			if (PageAddItemExtended(p, item, len, o, 0) != o)
				elog(ERROR, "lion store: could not add a sub-array");
			lion_wal_op(xs, p, LION_OP_ADD, o, 0, item, len);
		}
	}
	else if (newsublen == oldsublen)
	{
		char	   *target = (char *) PageGetItem(p, PageGetItemId(p, subno));
		Size		first = 0;
		Size		last = newsublen;

		while (first < newsublen && target[first] == newsub[first])
			first++;
		while (last > first && target[last - 1] == newsub[last - 1])
			last--;
		if (last > first)
		{
			memcpy(target + first, newsub + first, last - first);
			lion_wal_op(xs, p, LION_OP_SETBYTES, subno, (uint16) first,
						newsub + first, last - first);
		}
	}
	else
	{
		lion_wal_save_item(xs, p, subno);
		if (!PageIndexTupleOverwrite(p, subno, newsub, newsublen))
			elog(ERROR, "lion store: could not grow a sub-array");
		lion_wal_op_replace(xs, p, subno, newsub, newsublen);
	}
	lion_wal_finish(xs, LION_XLOG_STORE);

	return true;
}

/*
 * The structural write: decode, write, encode.  Returns true when the value
 * is written (or its heap page is ABSENT), false when the page was split or a
 * page appended and the write has to find its page again.  buf is released.
 */
static bool
lion_store_write_rebuild(Relation index, Relation heaprel,
						 const LionStoreCol *col, Buffer buf, int k,
						 OffsetNumber off, const LionStoreVal *v)
{
	Page		page = BufferGetPage(buf);
	char	   *copy = (char *) palloc(BLCKSZ);
	LionStoreHeader h;
	LionStoreModel m;
	LionStoreModel w;
	LionStoreDictSet ds;
	LionStoreImage img;
	int			j;

	memcpy(copy, page, BLCKSZ);
	lion_store_decode(index, col, (Page) copy, BufferGetBlockNumber(buf), &m);
	memcpy(&h, lion_store_page_header((Page) copy), sizeof(LionStoreHeader));
	lion_store_dset_init(&ds, CurrentMemoryContext);

	/* w is m with the write, past the range when k is */
	w = m;
	if (k > h.hi)
	{
		for (j = h.hi + 1; j <= k; j++)
		{
			w.nslots[j] = 0;
			w.slots[j] = NULL;
			w.absent[j] = false;
		}
		w.hi = k;
	}
	if (w.absent[k])
	{
		UnlockReleaseBuffer(buf);
		return true;
	}
	w.slots[k] = (LionStoreVal *)
		palloc0(sizeof(LionStoreVal) * Max(w.nslots[k], off));
	if (w.nslots[k] > 0)
		memcpy(w.slots[k], m.slots[k], sizeof(LionStoreVal) * w.nslots[k]);
	w.nslots[k] = Max(w.nslots[k], off);
	w.slots[k][off - 1] = *v;

	/* compacted, and wider if need be, in the page's mode ... */
	if (lion_store_encode(col, &w, w.lo, w.hi, h.mode, &ds, &img) ||
	/* ... or RAW, once the dictionary has stopped paying (§40) */
		(h.mode == LION_STORE_DICT && col->rawwidth > 0 &&
		 lion_store_encode(col, &w, w.lo, w.hi, LION_STORE_RAW, &ds, &img)))
	{
		lion_store_rewrite(index, buf, &img);
		UnlockReleaseBuffer(buf);
		return true;
	}

	if (w.lo < w.hi)
	{
		if (k > h.hi)
			lion_store_append(index, heaprel, buf, col, h.ckey, h.hi + 1, k,
							  h.mode);
		else
		{
			/*
			 * The upper half moves right - or, for a write to the page's last
			 * heap page, that page alone, which is where a heap filling page
			 * by page keeps writing: the left page is then full and stays so,
			 * where a cut in the middle would leave two half-full pages behind
			 * every heap page of the window.
			 */
			int			at = (k == h.hi) ? h.hi : (h.lo + h.hi + 1) / 2;

			/*
			 * A page of an ordered index that spans the sorted and the append
			 * region splits at the boundary, so that the sorted region's pages
			 * keep the ranges the directory names (§41): only the append
			 * region is ever written after a sort.
			 */
			if ((h.flags & LION_STORE_F_VIRTUAL) != 0 &&
				h.lo < LION_STORE_APPEND_VPAGE && h.hi >= LION_STORE_APPEND_VPAGE)
				at = LION_STORE_APPEND_VPAGE;

			lion_store_split(index, heaprel, buf, col, &m, at, h.mode);
		}
		UnlockReleaseBuffer(buf);
		return false;
	}

	/*
	 * One heap page whose values fit no page in the page's mode, nor RAW: it
	 * is ABSENT, and its rows are read from the heap (§40, "Growth").
	 */
	Assert(w.lo == k && w.hi == k);
	w.absent[k] = true;
	w.nslots[k] = 0;
	w.slots[k] = NULL;
	if (!lion_store_encode(col, &w, k, k, h.mode, &ds, &img))
		elog(ERROR, "lion store: an ABSENT heap page does not fit a page");
	lion_store_rewrite(index, buf, &img);
	UnlockReleaseBuffer(buf);
	return true;
}

/*
 * Write v as column col's value of (ckey, k, off): k a heap page, or a
 * virtual page of an ordered index (§41).  A chain this has to start gets
 * header flags `flags`, the window's kind and generation.
 */
static void
lion_store_write(Relation index, Relation heaprel, LionIndexState *ix,
				 const LionStoreCol *col, uint32 ckey, int k, OffsetNumber off,
				 const LionStoreVal *v, uint8 flags)
{
	uint64		slot = lion_storemap_slot(ckey, lion_store_stride(ix), col->ord);
	LionStoreCache *cache = ix->storecache;
	int			attempts = 0;

	for (;;)
	{
		BlockNumber head;
		Buffer		buf;

		if (++attempts > LION_STORE_MAX_ATTEMPTS)
			ereport(ERROR,
					(errcode(ERRCODE_INDEX_CORRUPTED),
					 errmsg("lion index \"%s\": could not find the store page of window %u column %d",
							RelationGetRelationName(index), ckey, col->attno),
					 errhint("REINDEX the index.")));
		CHECK_FOR_INTERRUPTS();

		if (BlockNumberIsValid(cache->head) && cache->slot == slot)
			head = cache->head;
		else
		{
			head = lion_storemap_head(index, ix->store.store_root, slot, cache);
			cache->slot = slot;
			cache->head = head;
		}
		if (!BlockNumberIsValid(head))
		{
			if (lion_store_create_head(index, heaprel, ix, col, ckey, k, off,
									   v, slot, flags))
				return;
			cache->head = InvalidBlockNumber;
			continue;
		}

		buf = lion_store_find_page(index, col, ckey, head, k);
		if (!BufferIsValid(buf))
		{
			/* the chain was freed under the cached head: ask the map */
			cache->head = InvalidBlockNumber;
			continue;
		}

		if (lion_store_write_inplace(index, buf, col, k, off, v))
		{
			UnlockReleaseBuffer(buf);
			return;
		}
		if (lion_store_write_rebuild(index, heaprel, col, buf, k, off, v))
			return;
	}
}

/*
 * The insert's look at an ordered window's permutation (§41): its generation
 * (0 when the window has none) and whether heap position (k, off) still has
 * an entry, which a new row there must clear.  Pages are read SHARE, one at a
 * time; the caller holds the window lock, so no sort replaces the chain
 * meanwhile, and a VACUUM that clears the entry first only makes the clearing
 * a no-op.
 */
static void
lion_store_perm_probe(Relation index, LionIndexState *ix, uint32 ckey, int k,
					  OffsetNumber off, int *gen, bool *stale)
{
	const LionStoreCol *pc = ix->storeperm;
	uint64		slot = lion_storemap_slot(ckey, lion_store_stride(ix), pc->ord);
	BlockNumber head = lion_storemap_head(index, ix->store.store_root, slot,
										  ix->storecache);
	BlockNumber blk = head;
	int			steps = 0;

	*gen = 0;
	*stale = false;
	while (BlockNumberIsValid(blk))
	{
		Buffer		buf = ReadBuffer(index, blk);
		Page		page;
		LionStoreHeader *h;
		BlockNumber next;

		LockBuffer(buf, BUFFER_LOCK_SHARE);
		page = BufferGetPage(buf);
		if (!lion_store_page_owned(page, ckey, (uint16) pc->ord))
		{
			UnlockReleaseBuffer(buf);
			ereport(ERROR,
					(errcode(ERRCODE_INDEX_CORRUPTED),
					 errmsg("lion index \"%s\": block %u on the permutation of window %u is not one of its pages",
							RelationGetRelationName(index), blk, ckey),
					 errhint("REINDEX the index.")));
		}
		lion_store_check_or_error(index, page, blk, pc, false);
		h = lion_store_page_header(page);
		if (blk == head)
			*gen = lion_store_flags_gen(h->flags);
		next = LionPageGetOpaque(page)->rightlink;
		if (k >= h->lo && k <= h->hi)
		{
			char	   *msg = lion_store_check_sub(page, h, k);
			LionStoreSub *sub;
			Size		len;

			if (msg != NULL)
			{
				UnlockReleaseBuffer(buf);
				ereport(ERROR,
						(errcode(ERRCODE_INDEX_CORRUPTED),
						 errmsg("lion index \"%s\": store page %u %s",
								RelationGetRelationName(index), blk, msg),
						 errhint("REINDEX the index.")));
			}
			sub = lion_store_page_sub(page, k, &len);
			if ((sub->flags & LION_STORE_ABSENT) == 0 && off <= sub->nslots)
			{
				const uint8 *body = (const uint8 *) sub + sizeof(LionStoreSub);

				*stale = !lion_store_null_get(body + (Size) sub->nslots * h->width,
											  off - 1);
			}
			UnlockReleaseBuffer(buf);
			return;
		}
		UnlockReleaseBuffer(buf);
		if (k < h->lo || !BlockNumberIsValid(next))
			return;
		lion_store_chain_step(index, blk, next, &steps);
		blk = next;
	}
}

void
lion_store_insert(Relation index, Relation heaprel, LionIndexState *ix,
				  ItemPointer tid, const Datum *values, const bool *isnull)
{
	uint64		code = lion_tid_to_code(tid);
	uint32		ckey = lion_code_ckey(code);
	uint16		k;
	OffsetNumber off;
	LionStoreVal *vals;
	Datum	   *fix;
	int			ord;

	Assert(ix->nstored > 0);
	lion_lo_split(lion_code_lo(code), &k, &off);

	/* every value first, so that a value too long is refused before a write */
	vals = (LionStoreVal *) palloc(sizeof(LionStoreVal) * ix->nstored);
	fix = (Datum *) palloc(sizeof(Datum) * ix->nstored);
	for (ord = 0; ord < ix->nstored; ord++)
	{
		const LionStoreCol *col = &ix->stored[ord];

		lion_store_value(index, col, values[col->attno - 1],
						 isnull[col->attno - 1], &vals[ord], &fix[ord]);
	}
	if (ix->store_order < 0)
	{
		for (ord = 0; ord < ix->nstored; ord++)
			lion_store_write(index, heaprel, ix, &ix->stored[ord], ckey, k, off,
							 &vals[ord], 0);
		return;
	}

	/*
	 * An ordered index (§41): the row goes to the append region, under the
	 * window lock in share mode, which keeps VACUUM's sort out of the window
	 * until every column is written.  A permutation entry left at the
	 * position by a row that died is cleared first, so that the new row is
	 * read from the append region.
	 */
	LockPage(index, LION_STORE_WINLOCK(ckey), ShareLock);
	{
		int			gen = 0;
		bool		stale = false;
		LionStoreVal none = {NULL, 0};
		uint8		flags;

		lion_store_perm_probe(index, ix, ckey, k, off, &gen, &stale);
		if (stale)
			lion_store_write(index, heaprel, ix, ix->storeperm, ckey, k, off,
							 &none, lion_store_make_flags(LION_STORE_F_PERM, gen));
		flags = lion_store_make_flags(LION_STORE_F_VIRTUAL, gen);
		for (ord = 0; ord < ix->nstored; ord++)
			lion_store_write(index, heaprel, ix, &ix->stored[ord], ckey,
							 LION_STORE_APPEND_VPAGE + k, off, &vals[ord], flags);
	}
	UnlockPage(index, LION_STORE_WINLOCK(ckey), ShareLock);
}

/* ---------------------------------------------------------------------
 * 7. VACUUM (DESIGN.md §40, "VACUUM")
 * --------------------------------------------------------------------- */

/*
 * Mark the store page in buf (EXCLUSIVE) DELETED and take it off the meta
 * page's count, in one record; buf is released.  The free space map is the
 * caller's, so that a caller holding the heap's extension lock can defer it.
 */
static void
lion_store_delete_page(Relation index, Buffer buf)
{
	FullTransactionId safexid = ReadNextFullTransactionId();
	Buffer		metabuf = lion_store_lock_meta(index);
	LionWalState *xs;
	Page		p;

	Assert(LionPageIsStore(BufferGetPage(buf)) &&
		   !LionPageIsDeleted(BufferGetPage(buf)));
	xs = lion_wal_begin(index);
	p = lion_wal_register_buffer(xs, buf, LION_WALBUF_STD);
	lion_page_set_deleted(p, safexid);
	lion_wal_op(xs, p, LION_OP_DELETED, 0, 0, &safexid, sizeof(FullTransactionId));
	lion_store_meta_count(xs, metabuf, -1);
	lion_wal_finish(xs, LION_XLOG_STORE);

	UnlockReleaseBuffer(buf);
	UnlockReleaseBuffer(metabuf);
}

void
lion_store_free_page(Relation index, Buffer buf)
{
	BlockNumber blk = BufferGetBlockNumber(buf);

	lion_store_delete_page(index, buf);
	RecordFreeIndexPage(index, blk);
}

/*
 * Which slots VACUUM clears.  A heap-ordered page's, and an ordered window's
 * append region's, by the callback on the slot's TID; an ordered window's
 * sorted slots and permutation entries by the dead positions the
 * permutation's walk found (§41), so that a row of the sorted region is asked
 * about once, not once per column.
 */
typedef struct LionStoreDead
{
	IndexBulkDeleteCallback callback;
	void	   *callback_state;
	uint32		ckey;			/* the window deadvlo and deadpos are of */
	bool		valid;
	uint8		deadvlo[(1 << 16) / 8];	/* sorted slots of dead rows, by vlo */
	uint8		deadpos[LION_CONTAINER_RANGE / 8];	/* dead positions, by lo */
} LionStoreDead;

static inline bool
lion_store_bit(const uint8 *map, uint32 i)
{
	return (map[i / 8] & (1 << (i % 8))) != 0;
}

static inline void
lion_store_bit_set(uint8 *map, uint32 i)
{
	map[i / 8] |= (uint8) (1 << (i % 8));
}

/* Is slot i of (heap or virtual) page k of a page with header h dead? */
static bool
lion_store_slot_dead(LionStoreDead *dd, const LionStoreHeader *h, int k,
					 uint32 i)
{
	BlockNumber first = lion_ckey_first_block(h->ckey);
	ItemPointerData tid;

	if ((h->flags & LION_STORE_F_PERM) != 0)
		return dd->valid && dd->ckey == h->ckey &&
			lion_store_bit(dd->deadpos, lion_store_vlo(k, (int) i + 1));
	if ((h->flags & LION_STORE_F_VIRTUAL) != 0)
	{
		if (k < LION_STORE_APPEND_VPAGE)
			return dd->valid && dd->ckey == h->ckey &&
				lion_store_bit(dd->deadvlo, lion_store_vlo(k, (int) i + 1));
		k -= LION_STORE_APPEND_VPAGE;
	}
	ItemPointerSet(&tid, first + k, (OffsetNumber) (i + 1));
	return dd->callback(&tid, dd->callback_state);
}

/*
 * Clear the dead slots of the store page in buf (EXCLUSIVE, checked), in
 * place or by a rewrite.
 */
static void
lion_store_vacuum_page(Relation index, const LionStoreCol *col, Buffer buf,
					   LionStoreDead *dd, LionStoreVacStats *st)
{
	Page		page = BufferGetPage(buf);
	LionStoreHeader h;
	bool	   *dead[LION_STORE_MAX_VPAGES];
	int64		written = 0;
	int64		ndead = 0;
	int			k;

	memcpy(&h, lion_store_page_header(page), sizeof(LionStoreHeader));

	/* which written slots are dead: one callback per written slot */
	for (k = h.lo; k <= h.hi; k++)
	{
		Size		len;
		LionStoreSub *s = lion_store_page_sub(page, k, &len);
		const uint8 *body = (const uint8 *) s + sizeof(LionStoreSub);
		uint32		i;

		dead[k] = NULL;
		if ((s->flags & LION_STORE_ABSENT) != 0)
			continue;
		for (i = 0; i < s->nslots; i++)
		{
			bool		isnull;

			if (h.mode == LION_STORE_DICT)
				isnull = (lion_store_code_get(body, h.width, i) == 0);
			else
				isnull = lion_store_null_get(body + (Size) s->nslots * h.width, i);
			if (isnull)
				continue;		/* NULL, never written, or cleared */
			written++;
			if (lion_store_slot_dead(dd, &h, k, i))
			{
				if (dead[k] == NULL)
					dead[k] = (bool *) palloc0(sizeof(bool) * s->nslots);
				dead[k][i] = true;
				ndead++;
			}
		}
	}
	if (ndead == 0)
		return;
	st->cleared += ndead;

	if (ndead * 4 >= written)
	{
		/*
		 * A quarter of the page is dead: write it again from its live
		 * values, which drops the dictionary entries nothing names any more,
		 * in whichever mode is smaller now (§40).
		 */
		char	   *copy = (char *) palloc(BLCKSZ);
		LionStoreModel m;
		LionStoreDictSet ds;
		LionStoreImage img;
		LionStoreImage other;
		bool		have;
		bool		haveother = false;

		memcpy(copy, page, BLCKSZ);
		lion_store_decode(index, col, (Page) copy, BufferGetBlockNumber(buf), &m);
		for (k = h.lo; k <= h.hi; k++)
		{
			uint32		i;

			if (dead[k] == NULL)
				continue;
			for (i = 0; i < m.nslots[k]; i++)
				if (dead[k][i])
				{
					m.slots[k][i].data = NULL;
					m.slots[k][i].len = 0;
				}
		}
		lion_store_dset_init(&ds, CurrentMemoryContext);
		have = lion_store_encode(col, &m, h.lo, h.hi, h.mode, &ds, &img);
		if (col->kind != LION_STORE_KIND_PERM &&
			(h.mode == LION_STORE_RAW || col->rawwidth > 0))
			haveother = lion_store_encode(col, &m, h.lo, h.hi,
										  h.mode == LION_STORE_RAW ?
										  LION_STORE_DICT : LION_STORE_RAW,
										  &ds, &other);
		if (haveother && (!have || other.need < img.need))
		{
			img = other;
			have = true;
		}
		if (!have)
			elog(ERROR, "lion store: a page with its dead slots cleared does not fit a page");
		lion_store_rewrite(index, buf, &img);
		st->rewritten++;
		st->records++;
		return;
	}

	/* Fewer: clear each in place, a SETBYTES per sub-array. */
	{
		LionWalState *xs = lion_wal_begin(index);
		Page		p = lion_wal_register_buffer(xs, buf, LION_WALBUF_STD);

		for (k = h.lo; k <= h.hi; k++)
		{
			Size		len;
			LionStoreSub *s;
			uint8	   *body;
			Size		lowbyte = PG_UINT16_MAX;
			Size		highbyte = 0;
			uint32		i;

			if (dead[k] == NULL)
				continue;
			s = lion_store_page_sub(p, k, &len);
			body = (uint8 *) s + sizeof(LionStoreSub);
			for (i = 0; i < s->nslots; i++)
			{
				Size		b;

				if (!dead[k][i])
					continue;
				if (h.mode == LION_STORE_DICT)
				{
					lion_store_code_set(body, h.width, i, 0);
					b = lion_store_code_byte(h.width, i);
					lowbyte = Min(lowbyte, b);
					highbyte = Max(highbyte, b + (h.width == 16 ? 1 : 0));
				}
				else
				{
					uint8	   *slot = body + (Size) i * h.width;

					/* the bytes too, so that nothing of a dead row stays */
					memset(slot, 0, h.width);
					lowbyte = Min(lowbyte, (Size) i * h.width);
					lion_store_null_set(body + (Size) s->nslots * h.width, i, true);
					b = (Size) s->nslots * h.width + i / 8;
					highbyte = Max(highbyte, b);
				}
			}
			lion_wal_op(xs, p, LION_OP_SETBYTES,
						(OffsetNumber) (LION_STORE_SUB_FIRST + k - h.lo),
						(uint16) (sizeof(LionStoreSub) + lowbyte),
						body + lowbyte, highbyte - lowbyte + 1);
		}
		lion_wal_finish(xs, LION_XLOG_STORE);
		st->records++;
	}
}

/* Copy the one item of map page blk, checked, into entries. */
static void
lion_storemap_copy(Relation index, BlockNumber blk, uint16 level,
				   uint64 number, BlockNumber *entries)
{
	Buffer		buf = ReadBuffer(index, blk);
	Page		page;

	LockBuffer(buf, BUFFER_LOCK_SHARE);
	page = BufferGetPage(buf);
	lion_storemap_check(index, page, blk, level, number);
	memcpy(entries, lion_storemap_entries(page), LION_STOREMAP_ITEM_SIZE);
	UnlockReleaseBuffer(buf);
}

/*
 * Walk the whole map, calling fn for every map page and every nonzero leaf
 * slot from slot `from` on.  The pages' items are copied under a SHARE lock
 * and the slots acted on with nothing held.
 */
typedef void (*LionStoreSlotFn) (void *arg, uint64 slot, BlockNumber head);

static void
lion_storemap_walk(Relation index, LionIndexState *ix, uint64 from,
				   LionStoreVisit visit, void *visitarg,
				   LionStoreSlotFn fn, void *fnarg, int64 *mappages)
{
	BlockNumber root = ix->store.store_root;
	BlockNumber *rootents = (BlockNumber *) palloc(LION_STOREMAP_ITEM_SIZE);
	BlockNumber *innerents = (BlockNumber *) palloc(LION_STOREMAP_ITEM_SIZE);
	BlockNumber *leafents = (BlockNumber *) palloc(LION_STOREMAP_ITEM_SIZE);
	uint64		fromleaf = from / LION_STOREMAP_FANOUT;
	uint32		a;

	lion_storemap_copy(index, root, LION_STOREMAP_ROOT, 0, rootents);
	if (visit != NULL)
		visit(visitarg, root);
	if (mappages != NULL)
		(*mappages)++;

	for (a = (uint32) (fromleaf / LION_STOREMAP_FANOUT); a < LION_STOREMAP_FANOUT; a++)
	{
		uint32		b;

		if (rootents[a] == 0)
			continue;
		lion_storemap_copy(index, rootents[a], LION_STOREMAP_INNER, a, innerents);
		if (visit != NULL)
			visit(visitarg, rootents[a]);
		if (mappages != NULL)
			(*mappages)++;

		for (b = 0; b < LION_STOREMAP_FANOUT; b++)
		{
			uint64		leafno = (uint64) a * LION_STOREMAP_FANOUT + b;
			uint32		c;

			if (innerents[b] == 0 || leafno < fromleaf)
				continue;
			lion_storemap_copy(index, innerents[b], LION_STOREMAP_LEAF, leafno,
							   leafents);
			if (visit != NULL)
				visit(visitarg, innerents[b]);
			if (mappages != NULL)
				(*mappages)++;

			for (c = 0; c < LION_STOREMAP_FANOUT; c++)
			{
				uint64		slot = leafno * LION_STOREMAP_FANOUT + c;

				if (leafents[c] == 0 || leafents[c] == LION_METAPAGE_BLKNO ||
					slot < from)
					continue;
				if (fn != NULL)
					fn(fnarg, slot, leafents[c]);
			}
			CHECK_FOR_INTERRUPTS();
		}
	}

	pfree(leafents);
	pfree(innerents);
	pfree(rootents);
}

typedef struct LionStoreVacArg
{
	Relation	index;
	LionIndexState *ix;
	bool		write;
	LionStoreVisit visit;
	void	   *visitarg;
	LionStoreVacStats *st;
	MemoryContext pagecxt;
	LionStoreDead *dead;
} LionStoreVacArg;

/* The column of ordinal ord: a stored column, or an ordered index's permutation. */
static inline const LionStoreCol *
lion_store_ord_col(const LionIndexState *ix, int ord)
{
	return (ord < ix->nstored) ? &ix->stored[ord] : ix->storeperm;
}

/*
 * The dead rows of an ordered window's sorted region (§41): every position
 * the permutation names is asked of the callback, once, and the dead ones'
 * positions and sorted slots marked.  The pages are read SHARE, one at a
 * time: an insert may clear an entry meanwhile, never set one.
 */
static void
lion_store_perm_dead(Relation index, LionIndexState *ix, uint32 ckey,
					 LionStoreDead *dd, LionStoreVacStats *st)
{
	const LionStoreCol *pc = ix->storeperm;
	uint64		slot = lion_storemap_slot(ckey, lion_store_stride(ix), pc->ord);
	BlockNumber blk = lion_storemap_head(index, ix->store.store_root, slot, NULL);
	BlockNumber first = lion_ckey_first_block(ckey);
	int			steps = 0;

	memset(dd->deadvlo, 0, sizeof(dd->deadvlo));
	memset(dd->deadpos, 0, sizeof(dd->deadpos));
	dd->ckey = ckey;
	dd->valid = true;

	while (BlockNumberIsValid(blk))
	{
		Buffer		buf = ReadBuffer(index, blk);
		Page		page;
		LionStoreHeader h;
		BlockNumber next;
		int			k;

		LockBuffer(buf, BUFFER_LOCK_SHARE);
		page = BufferGetPage(buf);
		if (!lion_store_page_owned(page, ckey, (uint16) pc->ord))
		{
			UnlockReleaseBuffer(buf);
			ereport(ERROR,
					(errcode(ERRCODE_INDEX_CORRUPTED),
					 errmsg("lion index \"%s\": block %u on the permutation of window %u is not one of its pages",
							RelationGetRelationName(index), blk, ckey),
					 errhint("REINDEX the index.")));
		}
		lion_store_check_or_error(index, page, blk, pc, true);
		memcpy(&h, lion_store_page_header(page), sizeof(LionStoreHeader));
		for (k = h.lo; k <= h.hi; k++)
		{
			Size		len;
			LionStoreSub *sub = lion_store_page_sub(page, k, &len);
			const uint8 *body = (const uint8 *) sub + sizeof(LionStoreSub);
			uint32		i;

			if ((sub->flags & LION_STORE_ABSENT) != 0)
				continue;
			for (i = 0; i < sub->nslots; i++)
			{
				ItemPointerData tid;
				uint16		vlo;

				if (lion_store_null_get(body + (Size) sub->nslots * h.width, i))
					continue;
				memcpy(&vlo, body + (Size) i * h.width, sizeof(uint16));
				ItemPointerSet(&tid, first + k, (OffsetNumber) (i + 1));
				if (dd->callback(&tid, dd->callback_state))
				{
					lion_store_bit_set(dd->deadpos, lion_store_vlo(k, (int) i + 1));
					lion_store_bit_set(dd->deadvlo, vlo);
				}
			}
		}
		next = LionPageGetOpaque(page)->rightlink;
		UnlockReleaseBuffer(buf);
		if (BlockNumberIsValid(next))
			lion_store_chain_step(index, blk, next, &steps);
		blk = next;
	}
}

/* One window's chain for one column: visit each page, clear its dead slots. */
static void
lion_store_vacuum_chain(void *arg, uint64 slot, BlockNumber head)
{
	LionStoreVacArg *va = (LionStoreVacArg *) arg;
	uint64		stride = (uint64) lion_store_stride(va->ix);
	uint32		ckey = (uint32) (slot / stride);
	int			ord = (int) (slot % stride);
	const LionStoreCol *col = lion_store_ord_col(va->ix, ord);
	BlockNumber blk = head;
	int			steps = 0;

	/*
	 * An ordered window's chains are walked in ordinal order, the
	 * permutation's last: its dead rows are found first, cleared from every
	 * column, and only then from the permutation (§41, "VACUUM's bulk
	 * delete").
	 */
	if (va->write && va->ix->store_order >= 0 &&
		(!va->dead->valid || va->dead->ckey != ckey))
		lion_store_perm_dead(va->index, va->ix, ckey, va->dead, va->st);

	while (BlockNumberIsValid(blk))
	{
		Buffer		buf = ReadBuffer(va->index, blk);
		Page		page;
		BlockNumber next;
		MemoryContext old;

		LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
		page = BufferGetPage(buf);

		/*
		 * A chain is freed only by amvacuumcleanup, which never runs beside
		 * this, so every page of it is this window's.
		 */
		if (!lion_store_page_owned(page, ckey, (uint16) ord) ||
			(blk == head && lion_store_check_header(page, col) == NULL &&
			 lion_store_page_header(page)->lo != 0))
		{
			UnlockReleaseBuffer(buf);
			ereport(ERROR,
					(errcode(ERRCODE_INDEX_CORRUPTED),
					 errmsg("lion index \"%s\": block %u on the store chain of window %u column %d is not one of its pages",
							RelationGetRelationName(va->index), blk, ckey,
							col->attno),
					 errhint("REINDEX the index.")));
		}
		lion_store_check_or_error(va->index, page, blk, col, true);
		va->visit(va->visitarg, blk);
		va->st->pages++;

		old = MemoryContextSwitchTo(va->pagecxt);
		if (va->write)
			lion_store_vacuum_page(va->index, col, buf, va->dead, va->st);
		MemoryContextSwitchTo(old);
		MemoryContextReset(va->pagecxt);

		next = LionPageGetOpaque(BufferGetPage(buf))->rightlink;
		UnlockReleaseBuffer(buf);
		if (BlockNumberIsValid(next))
			lion_store_chain_step(va->index, blk, next, &steps);
		blk = next;
		lion_vacuum_delay_point();
	}
}

void
lion_store_bulkdelete(Relation index, LionIndexState *ix,
					  IndexBulkDeleteCallback callback, void *callback_state,
					  bool write, LionStoreVisit visit, void *visitarg,
					  LionStoreVacStats *st)
{
	LionStoreVacArg va;

	Assert(ix->nstored > 0);
	va.index = index;
	va.ix = ix;
	va.dead = (LionStoreDead *) palloc(sizeof(LionStoreDead));
	va.dead->callback = callback;
	va.dead->callback_state = callback_state;
	va.dead->valid = false;
	va.write = write;
	va.visit = visit;
	va.visitarg = visitarg;
	va.st = st;
	va.pagecxt = AllocSetContextCreate(CurrentMemoryContext,
									   "lion store vacuum page",
									   ALLOCSET_DEFAULT_SIZES);
	lion_storemap_walk(index, ix, 0, visit, visitarg,
					   lion_store_vacuum_chain, &va, &st->mappages);
	MemoryContextDelete(va.pagecxt);
	pfree(va.dead);
}

/* The slots and heads amvacuumcleanup found past the heap's end. */
typedef struct LionStoreDoomed
{
	int			n;
	int			cap;
	uint64	   *slots;
	BlockNumber *heads;
} LionStoreDoomed;

static void
lion_store_doom(void *arg, uint64 slot, BlockNumber head)
{
	LionStoreDoomed *d = (LionStoreDoomed *) arg;

	if (d->n >= d->cap)
	{
		d->cap = Max(16, d->cap * 2);
		d->slots = (uint64 *) repalloc_array(d->slots, uint64, d->cap);
		d->heads = (BlockNumber *) repalloc_array(d->heads, BlockNumber, d->cap);
	}
	d->slots[d->n] = slot;
	d->heads[d->n] = head;
	d->n++;
}

/*
 * Free a chain from its head on, which nothing can reach any more: the map
 * slot is cleared and the head, when deleted is set, is DELETED already.
 */
static int64
lion_store_free_chain(Relation index, uint32 ckey, uint16 ord,
					  BlockNumber blk, int steps)
{
	int64		freed = 0;

	while (BlockNumberIsValid(blk))
	{
		Buffer		buf = ReadBuffer(index, blk);
		Page		page;
		BlockNumber next;

		LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
		page = BufferGetPage(buf);
		if (!lion_store_page_owned(page, ckey, ord))
		{
			/* gone already: a cleanup before this one got this far */
			UnlockReleaseBuffer(buf);
			break;
		}
		next = LionPageGetOpaque(page)->rightlink;
		lion_store_free_page(index, buf);
		freed++;
		if (BlockNumberIsValid(next))
			lion_store_chain_step(index, blk, next, &steps);
		blk = next;
	}
	return freed;
}

int64
lion_store_vacuum_cleanup(Relation index, Relation heaprel, LionIndexState *ix)
{
	LionStoreDoomed d;
	BlockNumber nblocks;
	uint32		firstwin;
	BlockNumber *nexts;
	int64		freed = 0;
	int			i;

	Assert(ix->nstored > 0);

	/*
	 * The windows past the heap's end now.  The heap does not shrink while
	 * VACUUM runs (it truncates after this), so these are a superset of the
	 * ones past it under the lock below, and an index with none - every index
	 * whose heap was not truncated - takes no lock at all.
	 */
	nblocks = RelationGetNumberOfBlocks(heaprel);
	firstwin = (uint32) ((nblocks + LION_BLOCKS_PER_CONTAINER - 1) /
						 LION_BLOCKS_PER_CONTAINER);
	memset(&d, 0, sizeof(d));
	d.cap = 16;
	d.slots = palloc_array(uint64, d.cap);
	d.heads = palloc_array(BlockNumber, d.cap);
	lion_storemap_walk(index, ix,
					   lion_storemap_slot(firstwin, lion_store_stride(ix), 0),
					   NULL, NULL, lion_store_doom, &d, NULL);
	if (d.n == 0)
		return 0;

	/*
	 * Under the heap's extension lock no heap page can be added, so no row
	 * can be inserted into a window that has no heap page, and no insert
	 * into one can be under way: its heap tuple would be on a page that does
	 * not exist.  While it is held, clear the slots and delete the heads; an
	 * insert into such a window afterwards finds the slot clear, or a cached
	 * head DELETED, and starts a new chain (§40, "VACUUM").  Without the lock
	 * an insert could write into a chain this is about to free, and a later
	 * insert would start a new chain whose other slots read as NULL, which
	 * for that first row would be wrong.  Nothing in here takes a heavyweight
	 * lock, which the extension lock forbids: the free space map is told
	 * about the heads after it is released.
	 */
	nexts = palloc_array(BlockNumber, d.n);
	LockRelationForExtension(heaprel, ExclusiveLock);
	nblocks = RelationGetNumberOfBlocks(heaprel);
	firstwin = (uint32) ((nblocks + LION_BLOCKS_PER_CONTAINER - 1) /
						 LION_BLOCKS_PER_CONTAINER);
	for (i = 0; i < d.n; i++)
	{
		uint64		slot = d.slots[i];
		uint32		ckey = (uint32) (slot / (uint64) lion_store_stride(ix));
		uint16		ord = (uint16) (slot % (uint64) lion_store_stride(ix));
		uint64		leafno = slot / LION_STOREMAP_FANOUT;
		uint32		e = (uint32) (slot % LION_STOREMAP_FANOUT);
		BlockNumber leafblk;
		Buffer		buf;
		Page		page;
		LionWalState *xs;
		Page		p;
		BlockNumber zero = 0;

		nexts[i] = InvalidBlockNumber;
		if (ckey < firstwin)
		{
			d.heads[i] = InvalidBlockNumber;
			continue;
		}

		/* the slot: one record on the map leaf */
		leafblk = lion_storemap_leaf(index, ix->store.store_root, leafno, NULL);
		Assert(BlockNumberIsValid(leafblk));
		buf = ReadBuffer(index, leafblk);
		LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
		page = BufferGetPage(buf);
		lion_storemap_check(index, page, leafblk, LION_STOREMAP_LEAF, leafno);
		if (lion_storemap_entries(page)[e] != d.heads[i])
		{
			UnlockReleaseBuffer(buf);
			d.heads[i] = InvalidBlockNumber;
			continue;
		}
		xs = lion_wal_begin(index);
		p = lion_wal_register_buffer(xs, buf, LION_WALBUF_STD);
		lion_storemap_entries(p)[e] = 0;
		lion_wal_op(xs, p, LION_OP_SETBYTES, FirstOffsetNumber,
					(uint16) (e * sizeof(BlockNumber)), &zero, sizeof(BlockNumber));
		lion_wal_finish(xs, LION_XLOG_STORE);
		UnlockReleaseBuffer(buf);

		/* the head, DELETED, so that a cached copy of the slot finds nothing */
		buf = ReadBuffer(index, d.heads[i]);
		LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
		page = BufferGetPage(buf);
		if (!lion_store_page_owned(page, ckey, ord))
		{
			UnlockReleaseBuffer(buf);
			d.heads[i] = InvalidBlockNumber;
			continue;
		}
		nexts[i] = LionPageGetOpaque(page)->rightlink;
		lion_store_delete_page(index, buf);
		freed++;
	}
	UnlockRelationForExtension(heaprel, ExclusiveLock);

	/* The rest of each chain is out of everybody's reach now. */
	for (i = 0; i < d.n; i++)
	{
		uint32		ckey = (uint32) (d.slots[i] / (uint64) lion_store_stride(ix));
		uint16		ord = (uint16) (d.slots[i] % (uint64) lion_store_stride(ix));

		if (!BlockNumberIsValid(d.heads[i]))
			continue;
		RecordFreeIndexPage(index, d.heads[i]);
		if (BlockNumberIsValid(nexts[i]))
			freed += lion_store_free_chain(index, ckey, ord, nexts[i], 1);
		CHECK_FOR_INTERRUPTS();
	}

	return freed;
}

/* ---------------------------------------------------------------------
 * VACUUM's sort of an ordered index's windows (DESIGN.md §41, "The sort")
 * --------------------------------------------------------------------- */

/*
 * Read the chain at head, of window ckey and col, into m: every page checked,
 * of generation gen, copied into the current memory context and decoded.
 * The caller holds the window lock exclusively, so nothing but a reader is
 * on the chain: a page that is not the window's is damage.
 */
static void
lion_store_read_chain(Relation index, const LionStoreCol *col, uint32 ckey,
					  BlockNumber head, int gen, LionStoreModel *m)
{
	BlockNumber blk = head;
	int			steps = 0;

	memset(m, 0, sizeof(LionStoreModel));
	m->ckey = ckey;
	m->ord = (uint16) col->ord;
	m->hi = -1;
	while (BlockNumberIsValid(blk))
	{
		Buffer		buf = ReadBuffer(index, blk);
		Page		page;
		char	   *copy;
		LionStoreModel pm;
		BlockNumber next;
		int			k;

		LockBuffer(buf, BUFFER_LOCK_SHARE);
		page = BufferGetPage(buf);
		if (!lion_store_page_owned(page, ckey, (uint16) col->ord) ||
			(blk == head && lion_store_check_header(page, col) == NULL &&
			 lion_store_page_header(page)->lo != 0))
		{
			UnlockReleaseBuffer(buf);
			ereport(ERROR,
					(errcode(ERRCODE_INDEX_CORRUPTED),
					 errmsg("lion index \"%s\": block %u on the store chain of window %u ordinal %d is not one of its pages",
							RelationGetRelationName(index), blk, ckey, col->ord),
					 errhint("REINDEX the index.")));
		}
		lion_store_check_or_error(index, page, blk, col, true);
		if (lion_store_flags_gen(lion_store_page_header(page)->flags) != gen)
		{
			UnlockReleaseBuffer(buf);
			ereport(ERROR,
					(errcode(ERRCODE_INDEX_CORRUPTED),
					 errmsg("lion index \"%s\": store page %u of window %u is of generation %d, its permutation of %d",
							RelationGetRelationName(index), blk, ckey,
							lion_store_flags_gen(lion_store_page_header(page)->flags),
							gen),
					 errhint("REINDEX the index.")));
		}
		copy = (char *) palloc(BLCKSZ);
		memcpy(copy, page, BLCKSZ);
		next = LionPageGetOpaque(page)->rightlink;
		UnlockReleaseBuffer(buf);

		lion_store_decode(index, col, (Page) copy, blk, &pm);
		if (pm.lo != m->hi + 1)
			ereport(ERROR,
					(errcode(ERRCODE_INDEX_CORRUPTED),
					 errmsg("lion index \"%s\": store page %u of window %u covers pages %d to %d after %d",
							RelationGetRelationName(index), blk, ckey, pm.lo,
							pm.hi, m->hi),
					 errhint("REINDEX the index.")));
		for (k = pm.lo; k <= pm.hi; k++)
		{
			m->nslots[k] = pm.nslots[k];
			m->slots[k] = pm.slots[k];
			m->absent[k] = pm.absent[k];
		}
		m->hi = pm.hi;
		if (pm.extra != NULL)
		{
			m->extra = pm.extra;
			m->extralen = pm.extralen;
		}
		m->flags = pm.flags;
		if (BlockNumberIsValid(next))
			lion_store_chain_step(index, blk, next, &steps);
		blk = next;
	}
}

/* A permutation model's entry for heap position (k, off), or false. */
static inline bool
lion_store_perm_entry(const LionStoreModel *pm, int k, OffsetNumber off,
					  uint16 *vlo)
{
	if (k > pm->hi || pm->absent[k] || off > pm->nslots[k] ||
		pm->slots[k][off - 1].data == NULL)
		return false;
	memcpy(vlo, pm->slots[k][off - 1].data, sizeof(uint16));
	return true;
}

/* A model's value at (page k, offset off), or NULL past what it holds. */
static inline const LionStoreVal *
lion_store_model_get(const LionStoreModel *m, int k, OffsetNumber off)
{
	if (k > m->hi || m->absent[k] || off > m->nslots[k] || m->slots[k] == NULL)
		return NULL;
	return &m->slots[k][off - 1];
}

/*
 * Write a page image nothing can reach yet as a new page linked to
 * rightlink, in a record of its own with the meta page's count.
 */
static BlockNumber
lion_store_write_new_page(Relation index, Relation heaprel,
						  const LionStoreImage *img, BlockNumber rightlink)
{
	Buffer		nbuf = lion_alloc_page(index, heaprel, true);
	BlockNumber blk = BufferGetBlockNumber(nbuf);
	Buffer		metabuf = lion_store_lock_meta(index);
	LionWalState *xs;
	Page		p;

	xs = lion_wal_begin(index);
	p = lion_wal_register_buffer(xs, nbuf, LION_WALBUF_INIT);
	lion_store_log_image(xs, p, img, rightlink);
	lion_store_meta_count(xs, metabuf, 1);
	lion_wal_finish(xs, LION_XLOG_STORE);
	UnlockReleaseBuffer(nbuf);
	UnlockReleaseBuffer(metabuf);
	return blk;
}

/*
 * Write a chain's images as new pages, the last first so that each page is
 * written with the block of the one after it; returns the head.  The pages
 * go into the sorted window's directory as they are placed.
 */
static BlockNumber
lion_store_write_new_chain(Relation index, Relation heaprel,
						   const LionStoreImages *li, LionStoreSorted *so,
						   bool dir)
{
	BlockNumber next = InvalidBlockNumber;
	BlockNumber *blks = (BlockNumber *) palloc(sizeof(BlockNumber) * li->n);
	int			i;

	for (i = li->n - 1; i >= 0; i--)
	{
		next = lion_store_write_new_page(index, heaprel, &li->imgs[i], next);
		blks[i] = next;
		CHECK_FOR_INTERRUPTS();
	}
	if (dir)
		for (i = 0; i < li->n; i++)
			lion_store_sorted_dir(so, &li->imgs[i], blks[i]);
	return next;
}

/*
 * Point every map slot of window ckey at its new head, in one record: the
 * slots of one window are consecutive, so one map leaf or two.
 */
static void
lion_store_switch_heads(Relation index, Relation heaprel, LionIndexState *ix,
						uint32 ckey, const BlockNumber *heads)
{
	int			stride = lion_store_stride(ix);
	uint64		s0 = lion_storemap_slot(ckey, stride, 0);
	uint64		s1 = s0 + (uint64) stride - 1;
	uint64		leafno[2];
	BlockNumber leafblk[2];
	Buffer		buf[2];
	int			nleaf = 1;
	LionWalState *xs;
	int			j;

	leafno[0] = s0 / LION_STOREMAP_FANOUT;
	leafno[1] = s1 / LION_STOREMAP_FANOUT;
	if (leafno[1] != leafno[0])
		nleaf = 2;
	for (j = 0; j < nleaf; j++)
		leafblk[j] = lion_storemap_extend(index, heaprel, ix, leafno[j]);
	for (j = 0; j < nleaf; j++)
	{
		buf[j] = ReadBuffer(index, leafblk[j]);
		LockBuffer(buf[j], BUFFER_LOCK_EXCLUSIVE);
		lion_storemap_check(index, BufferGetPage(buf[j]), leafblk[j],
							LION_STOREMAP_LEAF, leafno[j]);
	}

	xs = lion_wal_begin(index);
	for (j = 0; j < nleaf; j++)
	{
		Page		p = lion_wal_register_buffer(xs, buf[j], LION_WALBUF_STD);
		BlockNumber *ents = lion_storemap_entries(p);
		uint64		from = Max(s0, leafno[j] * LION_STOREMAP_FANOUT);
		uint64		to = Min(s1, (leafno[j] + 1) * LION_STOREMAP_FANOUT - 1);
		uint64		sl;

		for (sl = from; sl <= to; sl++)
			ents[sl % LION_STOREMAP_FANOUT] = heads[sl - s0];
		lion_wal_op(xs, p, LION_OP_SETBYTES, FirstOffsetNumber,
					(uint16) ((from % LION_STOREMAP_FANOUT) * sizeof(BlockNumber)),
					&ents[from % LION_STOREMAP_FANOUT],
					(to - from + 1) * sizeof(BlockNumber));
	}
	lion_wal_finish(xs, LION_XLOG_STORE);
	for (j = 0; j < nleaf; j++)
		UnlockReleaseBuffer(buf[j]);
}

/*
 * The line pointers in use on heap block blk, as bits by offset: a row's
 * (normal, or the root of a HOT chain) or one being pruned.  None for a
 * block past the heap's end.
 */
static void
lion_store_heap_used(Relation heaprel, BlockNumber blk, uint8 *used)
{
	Buffer		buf;
	Page		page;
	OffsetNumber maxoff;
	OffsetNumber off;

	memset(used, 0, (MaxHeapTuplesPerPage + 1 + 7) / 8);
	if (blk >= RelationGetNumberOfBlocks(heaprel))
		return;
	buf = ReadBufferExtended(heaprel, MAIN_FORKNUM, blk, RBM_NORMAL, NULL);
	LockBuffer(buf, BUFFER_LOCK_SHARE);
	page = BufferGetPage(buf);
	if (!PageIsNew(page))
	{
		maxoff = PageGetMaxOffsetNumber(page);
		for (off = FirstOffsetNumber; off <= Min(maxoff, MaxHeapTuplesPerPage); off++)
			if (ItemIdIsUsed(PageGetItemId(page, off)))
				lion_store_bit_set(used, off);
	}
	UnlockReleaseBuffer(buf);
}

/*
 * Sort window ckey again, under the window lock the caller holds
 * exclusively: read its chains, sort its rows, write the new chains and
 * permutation, switch the map, free the old pages.  false, with nothing
 * written, for a window it leaves alone (no rows, or an ABSENT page whose
 * values only the heap has).
 */
static bool
lion_store_sort_window_now(Relation index, Relation heaprel, LionIndexState *ix,
						   SortSupport ssup, uint32 ckey, int64 *freed)
{
	int			stride = lion_store_stride(ix);
	BlockNumber *oldheads = (BlockNumber *) palloc(sizeof(BlockNumber) * stride);
	BlockNumber *newheads = (BlockNumber *) palloc(sizeof(BlockNumber) * stride);
	LionStoreModel *vm = (LionStoreModel *) palloc(sizeof(LionStoreModel) * ix->nstored);
	LionStoreModel *hm = (LionStoreModel *) palloc0(sizeof(LionStoreModel) * ix->nstored);
	LionStoreModel pm;
	LionStoreDictSet ds;
	LionStoreSorted so;
	uint16	   *rows;
	const LionStoreVal **vals;
	uint8		used[(MaxHeapTuplesPerPage + 1 + 7) / 8];
	int			n = 0;
	int			gen = 0;
	int			ord;
	int			k;
	bool		any = false;

	for (ord = 0; ord < stride; ord++)
	{
		oldheads[ord] = lion_storemap_head(index, ix->store.store_root,
										   lion_storemap_slot(ckey, stride, ord),
										   NULL);
		if (ord < ix->nstored && BlockNumberIsValid(oldheads[ord]))
			any = true;
	}
	if (!any)
		return false;

	memset(&pm, 0, sizeof(pm));
	pm.hi = -1;
	if (BlockNumberIsValid(oldheads[ix->nstored]))
	{
		Buffer		buf = ReadBuffer(index, oldheads[ix->nstored]);

		LockBuffer(buf, BUFFER_LOCK_SHARE);
		if (lion_store_page_owned(BufferGetPage(buf), ckey, (uint16) ix->nstored))
			gen = lion_store_flags_gen(lion_store_page_header(BufferGetPage(buf))->flags);
		UnlockReleaseBuffer(buf);
		lion_store_read_chain(index, ix->storeperm, ckey, oldheads[ix->nstored],
							  gen, &pm);
	}
	for (ord = 0; ord < ix->nstored; ord++)
	{
		if (!BlockNumberIsValid(oldheads[ord]))
		{
			memset(&vm[ord], 0, sizeof(LionStoreModel));
			vm[ord].hi = -1;
			continue;
		}
		lion_store_read_chain(index, &ix->stored[ord], ckey, oldheads[ord], gen,
							  &vm[ord]);
		for (k = 0; k <= vm[ord].hi; k++)
			if (vm[ord].absent[k])
				return false;	/* values only the heap has: leave it be */
	}

	/*
	 * The window's rows, in heap coordinates: a position with a permutation
	 * entry reads its sorted slot, any other its append slot - what a reader
	 * reads.  A slot does not tell a NULL from no row, so a position whose
	 * every value is NULL is a row only if the heap has a line pointer in use
	 * there: a slot no row wrote, below one an insert did, or an entry a
	 * crash left behind, takes no slot in the new order.  An insert that
	 * puts a row at such a position meanwhile waits for the window lock, and
	 * writes its slots in the append region after the sort.
	 */
	rows = (uint16 *) palloc(sizeof(uint16) * LION_BLOCKS_PER_CONTAINER * MaxHeapTuplesPerPage);
	vals = (const LionStoreVal **) palloc(sizeof(LionStoreVal *) * ix->nstored);
	for (k = 0; k < LION_BLOCKS_PER_CONTAINER; k++)
	{
		int			vp = LION_STORE_APPEND_VPAGE + k;
		int			maxoff = 0;
		bool		heapread = false;
		OffsetNumber off;

		for (ord = 0; ord < ix->nstored; ord++)
			if (vp <= vm[ord].hi)
				maxoff = Max(maxoff, (int) vm[ord].nslots[vp]);
		if (k <= pm.hi && !pm.absent[k])
			maxoff = Max(maxoff, (int) pm.nslots[k]);
		maxoff = Min(maxoff, MaxHeapTuplesPerPage);

		for (off = FirstOffsetNumber; off <= maxoff; off++)
		{
			uint16		vlo;
			bool		sorted = lion_store_perm_entry(&pm, k, off, &vlo);
			bool		present = sorted;
			bool		anyvalue = false;

			if (!sorted)
				for (ord = 0; ord < ix->nstored && !present; ord++)
					present = (vp <= vm[ord].hi && off <= vm[ord].nslots[vp]);
			if (!present)
				continue;
			for (ord = 0; ord < ix->nstored; ord++)
			{
				vals[ord] = sorted ?
					lion_store_model_get(&vm[ord], lion_store_vlo_page(vlo),
										 (OffsetNumber) lion_store_vlo_off(vlo)) :
					lion_store_model_get(&vm[ord], vp, off);
				if (vals[ord] != NULL && vals[ord]->data != NULL)
					anyvalue = true;
			}
			if (!anyvalue)
			{
				if (!heapread)
				{
					lion_store_heap_used(heaprel,
										 (BlockNumber) ckey * LION_BLOCKS_PER_CONTAINER + k,
										 used);
					heapread = true;
				}
				if (!lion_store_bit(used, off))
					continue;
			}
			for (ord = 0; ord < ix->nstored; ord++)
			{
				if (hm[ord].slots[k] == NULL)
					hm[ord].slots[k] = (LionStoreVal *)
						palloc0(sizeof(LionStoreVal) * MaxHeapTuplesPerPage);
				if (vals[ord] != NULL)
					hm[ord].slots[k][off - 1] = *vals[ord];
				hm[ord].nslots[k] = Max(hm[ord].nslots[k], off);
				hm[ord].hi = Max(hm[ord].hi, k);
			}
			rows[n++] = lion_store_vlo(k, off);
		}
	}
	if (n == 0)
		return false;

	lion_store_sort_window(ix, ssup, ckey, hm, rows, n, lion_store_next_gen(gen),
						   &so);
	lion_store_dset_init(&ds, CurrentMemoryContext);
	for (ord = 0; ord < ix->nstored; ord++)
	{
		LionStoreImages li;

		memset(&li, 0, sizeof(li));
		lion_store_layout(&ix->stored[ord], &so.vm[ord], so.nvpages - 1, &ds, &li);
		newheads[ord] = lion_store_write_new_chain(index, heaprel, &li, &so, true);
	}
	lion_store_sorted_header(&so);
	{
		LionStoreImages li;

		memset(&li, 0, sizeof(li));
		lion_store_layout(ix->storeperm, &so.pm, so.pm.hi, &ds, &li);
		newheads[ix->nstored] = lion_store_write_new_chain(index, heaprel, &li,
														   &so, false);
	}

	/* the switch, after which the old pages are nobody's */
	lion_store_switch_heads(index, heaprel, ix, ckey, newheads);
	for (ord = 0; ord < stride; ord++)
		if (BlockNumberIsValid(oldheads[ord]))
			*freed += lion_store_free_chain(index, ckey, (uint16) ord,
											oldheads[ord], 0);
	return true;
}

/*
 * The non-NULL slots of sub-array sub of a checked page with header h: the
 * rows a page holds, as far as a slot can tell (a NULL is not told apart
 * from a slot no row wrote).
 */
static int
lion_store_sub_values(const LionStoreHeader *h, const LionStoreSub *sub)
{
	const uint8 *body = (const uint8 *) sub + sizeof(LionStoreSub);
	int			n = 0;
	uint32		i;

	if ((sub->flags & LION_STORE_ABSENT) != 0)
		return 0;
	for (i = 0; i < sub->nslots; i++)
	{
		if (h->mode == LION_STORE_DICT)
			n += (lion_store_code_get(body, h->width, i) != 0);
		else
			n += !lion_store_null_get(body + (Size) sub->nslots * h->width, i);
	}
	return n;
}

/*
 * Does window ckey want sorting (§41)?  Its permutation says how many rows
 * the last sort ordered and how many of them still have an entry; the order
 * column's chain how many values its append region has (a row whose order
 * value is NULL is not counted: a slot does not tell it from no row).  A
 * quarter of the window's rows outside the sorted order, dead or appended,
 * and it does.
 */
static bool
lion_store_window_wants_sort(Relation index, LionIndexState *ix, uint32 ckey)
{
	int			stride = lion_store_stride(ix);
	const LionStoreCol *ocol = &ix->stored[ix->store_order];
	BlockNumber blk;
	int			steps = 0;
	int64		nsorted = 0;
	int64		live = 0;
	int64		appended = 0;
	int64		outside;

	blk = lion_storemap_head(index, ix->store.store_root,
							 lion_storemap_slot(ckey, stride, ix->nstored), NULL);
	while (BlockNumberIsValid(blk))
	{
		Buffer		buf = ReadBuffer(index, blk);
		Page		page;
		LionStoreHeader *h;
		BlockNumber next;
		int			k;

		LockBuffer(buf, BUFFER_LOCK_SHARE);
		page = BufferGetPage(buf);
		if (!lion_store_page_owned(page, ckey, (uint16) ix->nstored) ||
			lion_store_page_check(page, ix->storeperm) != NULL)
		{
			UnlockReleaseBuffer(buf);
			return false;		/* the sort itself will say what is wrong */
		}
		h = lion_store_page_header(page);
		if (h->lo == 0)
		{
			LionStoreWinHdr wh;

			memcpy(&wh, (const char *) PageGetItem(page, PageGetItemId(page, LION_STORE_DICT_OFF)) +
				   sizeof(LionStoreDict), sizeof(LionStoreWinHdr));
			nsorted = wh.nsorted;
		}
		for (k = h->lo; k <= h->hi; k++)
		{
			Size		len;
			LionStoreSub *sub = lion_store_page_sub(page, k, &len);
			const uint8 *body = (const uint8 *) sub + sizeof(LionStoreSub);
			uint32		i;

			for (i = 0; i < sub->nslots; i++)
				if (!lion_store_null_get(body + (Size) sub->nslots * h->width, i))
					live++;
		}
		next = LionPageGetOpaque(page)->rightlink;
		UnlockReleaseBuffer(buf);
		if (BlockNumberIsValid(next))
			lion_store_chain_step(index, blk, next, &steps);
		blk = next;
	}

	steps = 0;
	blk = lion_storemap_head(index, ix->store.store_root,
							 lion_storemap_slot(ckey, stride, ocol->ord), NULL);
	while (BlockNumberIsValid(blk))
	{
		Buffer		buf = ReadBuffer(index, blk);
		Page		page;
		LionStoreHeader *h;
		BlockNumber next;
		int			k;

		LockBuffer(buf, BUFFER_LOCK_SHARE);
		page = BufferGetPage(buf);
		if (!lion_store_page_owned(page, ckey, (uint16) ocol->ord) ||
			lion_store_page_check(page, ocol) != NULL)
		{
			UnlockReleaseBuffer(buf);
			return false;
		}
		h = lion_store_page_header(page);
		for (k = Max(h->lo, LION_STORE_APPEND_VPAGE); k <= h->hi; k++)
		{
			Size		len;

			appended += lion_store_sub_values(h, lion_store_page_sub(page, k, &len));
		}
		next = LionPageGetOpaque(page)->rightlink;
		UnlockReleaseBuffer(buf);
		if (BlockNumberIsValid(next))
			lion_store_chain_step(index, blk, next, &steps);
		blk = next;
	}

	outside = Max(nsorted - live, 0) + appended;
	return outside > 0 && outside * 4 > live + appended;
}

/* The windows a map walk found, for the sort. */
typedef struct LionStoreWindows
{
	int			n;
	int			cap;
	uint32	   *ckeys;
	int			stride;
} LionStoreWindows;

static void
lion_store_note_window(void *arg, uint64 slot, BlockNumber head)
{
	LionStoreWindows *w = (LionStoreWindows *) arg;
	uint32		ckey = (uint32) (slot / (uint64) w->stride);

	if (w->n > 0 && w->ckeys[w->n - 1] == ckey)
		return;
	if (w->n >= w->cap)
	{
		w->cap = Max(64, w->cap * 2);
		w->ckeys = (uint32 *) repalloc_array(w->ckeys, uint32, w->cap);
	}
	w->ckeys[w->n++] = ckey;
}

int64
lion_store_vacuum_sort(Relation index, Relation heaprel, LionIndexState *ix,
					   int64 *freed)
{
	LionStoreWindows w;
	MemoryContext wcxt;
	MemoryContext old;
	SortSupportData ssup;
	BlockNumber nblocks;
	uint32		firstwin;
	int64		sorted = 0;
	int			i;

	*freed = 0;
	if (ix->store_order < 0)
		return 0;

	memset(&w, 0, sizeof(w));
	w.stride = lion_store_stride(ix);
	w.cap = 64;
	w.ckeys = palloc_array(uint32, w.cap);
	lion_storemap_walk(index, ix, 0, NULL, NULL, lion_store_note_window, &w, NULL);

	/* the windows past the heap's end are lion_store_vacuum_cleanup()'s */
	nblocks = RelationGetNumberOfBlocks(heaprel);
	firstwin = (uint32) ((nblocks + LION_BLOCKS_PER_CONTAINER - 1) /
						 LION_BLOCKS_PER_CONTAINER);
	wcxt = AllocSetContextCreate(CurrentMemoryContext, "lion store sort",
								 ALLOCSET_DEFAULT_SIZES);
	lion_store_order_ssup(index, ix, &ssup, CurrentMemoryContext);

	for (i = 0; i < w.n; i++)
	{
		uint32		ckey = w.ckeys[i];

		if (ckey >= firstwin)
			break;
		old = MemoryContextSwitchTo(wcxt);
		if (lion_store_window_wants_sort(index, ix, ckey) &&
			ConditionalLockPage(index, LION_STORE_WINLOCK(ckey), ExclusiveLock))
		{
			if (lion_store_sort_window_now(index, heaprel, ix, &ssup, ckey, freed))
				sorted++;
			UnlockPage(index, LION_STORE_WINLOCK(ckey), ExclusiveLock);
		}
		MemoryContextSwitchTo(old);
		MemoryContextReset(wcxt);
		lion_vacuum_delay_point();
	}
	MemoryContextDelete(wcxt);
	pfree(w.ckeys);
	return sorted;
}

/*
 * verify()'s check of an ordered window as a whole (DESIGN.md §41,
 * "verify()"), after each of its chains has passed on its own: every page
 * of the generation of the permutation's head (0 without one), no sorted
 * slot without a permutation, the permutation a bijection from the
 * positions with an entry onto distinct sorted slots below nsorted, every
 * sorted slot holding a value named by an entry, and the directory naming
 * the pages that hold sorted slots - all of them unless it was thinned.
 * ERRORs on damage.  The caller holds what keeps VACUUM out.
 */
void
lion_store_verify_window(Relation index, LionIndexState *ix, uint32 ckey)
{
	int			stride = lion_store_stride(ix);
	BlockNumber *heads = (BlockNumber *) palloc(sizeof(BlockNumber) * stride);
	LionStoreModel *vm = (LionStoreModel *) palloc(sizeof(LionStoreModel));
	LionStoreModel *pm = (LionStoreModel *) palloc(sizeof(LionStoreModel));
	LionStoreWinHdr wh;
	LionStoreDirEnt *dir = NULL;
	uint8	   *named;
	int			gen = 0;
	int			ndirseen = 0;
	int			nvpages;
	int			ord;
	int			k;

	if (ix->store_order < 0)
		return;
	for (ord = 0; ord < stride; ord++)
		heads[ord] = lion_storemap_head(index, ix->store.store_root,
										lion_storemap_slot(ckey, stride, ord),
										NULL);

	memset(&wh, 0, sizeof(wh));
	memset(pm, 0, sizeof(LionStoreModel));
	pm->hi = -1;
	if (BlockNumberIsValid(heads[ix->nstored]))
	{
		Buffer		buf = ReadBuffer(index, heads[ix->nstored]);

		LockBuffer(buf, BUFFER_LOCK_SHARE);
		if (lion_store_page_owned(BufferGetPage(buf), ckey, (uint16) ix->nstored))
			gen = lion_store_flags_gen(lion_store_page_header(BufferGetPage(buf))->flags);
		UnlockReleaseBuffer(buf);
		lion_store_read_chain(index, ix->storeperm, ckey, heads[ix->nstored],
							  gen, pm);
		if (pm->extra == NULL || gen == 0)
			ereport(ERROR,
					(errcode(ERRCODE_INDEX_CORRUPTED),
					 errmsg("lion index \"%s\": the permutation of window %u has no window header, or generation 0",
							RelationGetRelationName(index), ckey)));
		memcpy(&wh, pm->extra, sizeof(LionStoreWinHdr));
		dir = (LionStoreDirEnt *) palloc(sizeof(LionStoreDirEnt) * Max(wh.ndir, 1));
		memcpy(dir, pm->extra + sizeof(LionStoreWinHdr),
			   sizeof(LionStoreDirEnt) * wh.ndir);
		for (k = 1; k < wh.ndir; k++)
			if (dir[k].ord < dir[k - 1].ord ||
				(dir[k].ord == dir[k - 1].ord && dir[k].lo <= dir[k - 1].lo))
				ereport(ERROR,
						(errcode(ERRCODE_INDEX_CORRUPTED),
						 errmsg("lion index \"%s\": the directory of window %u is out of order at entry %d",
								RelationGetRelationName(index), ckey, k)));
	}

	/*
	 * The virtual pages the sorted region uses: every page starting below
	 * them is in the directory unless it was thinned.  A page past them holds
	 * no sorted slot (an append after a sort starts where the last page
	 * ended), and needs no entry.
	 */
	nvpages = (wh.vwidth > 0) ? (wh.nsorted + wh.vwidth - 1) / wh.vwidth : 0;

	/* the permutation: distinct sorted slots below nsorted */
	named = (uint8 *) palloc0((Max(wh.nsorted, 1) + 7) / 8);
	for (k = 0; k <= pm->hi; k++)
	{
		OffsetNumber off;

		if (pm->absent[k])
			ereport(ERROR,
					(errcode(ERRCODE_INDEX_CORRUPTED),
					 errmsg("lion index \"%s\": the permutation of window %u is ABSENT for heap page %d",
							RelationGetRelationName(index), ckey, k)));
		for (off = FirstOffsetNumber; off <= pm->nslots[k]; off++)
		{
			uint16		vlo;
			int			p;
			int			o;
			uint32		s;

			if (!lion_store_perm_entry(pm, k, off, &vlo))
				continue;
			p = lion_store_vlo_page(vlo);
			o = lion_store_vlo_off(vlo);
			s = (uint32) p * wh.vwidth + (uint32) (o - 1);
			if (p >= LION_STORE_APPEND_VPAGE || o < FirstOffsetNumber ||
				o > wh.vwidth || s >= wh.nsorted)
				ereport(ERROR,
						(errcode(ERRCODE_INDEX_CORRUPTED),
						 errmsg("lion index \"%s\": the permutation of window %u gives heap page %d offset %u the slot %u, outside the %u rows sorted %u to a page",
								RelationGetRelationName(index), ckey, k,
								(unsigned) off, (unsigned) vlo,
								(unsigned) wh.nsorted, (unsigned) wh.vwidth)));
			if (lion_store_bit(named, s))
				ereport(ERROR,
						(errcode(ERRCODE_INDEX_CORRUPTED),
						 errmsg("lion index \"%s\": the permutation of window %u gives slot %u to two rows",
								RelationGetRelationName(index), ckey,
								(unsigned) vlo)));
			lion_store_bit_set(named, s);
		}
	}

	/* every column: its generation, its sorted values named, its directory */
	for (ord = 0; ord < ix->nstored; ord++)
	{
		const LionStoreCol *col = &ix->stored[ord];
		BlockNumber blk = heads[ord];
		int			steps = 0;
		int			p;

		if (!BlockNumberIsValid(blk))
			continue;
		lion_store_read_chain(index, col, ckey, blk, gen, vm);
		for (p = 0; p < LION_STORE_APPEND_VPAGE && p <= vm->hi; p++)
		{
			OffsetNumber off;

			if (vm->absent[p] || vm->slots[p] == NULL)
				continue;
			for (off = FirstOffsetNumber; off <= vm->nslots[p]; off++)
			{
				uint32		s = (uint32) p * wh.vwidth + (uint32) (off - 1);

				if (vm->slots[p][off - 1].data == NULL)
					continue;
				if (off > wh.vwidth || s >= wh.nsorted || !lion_store_bit(named, s))
					ereport(ERROR,
							(errcode(ERRCODE_INDEX_CORRUPTED),
							 errmsg("lion index \"%s\": window %u column %d has a value at virtual page %d offset %u that no permutation entry names",
									RelationGetRelationName(index), ckey,
									col->attno, p, (unsigned) off)));
			}
		}

		/* the directory, page by page */
		while (BlockNumberIsValid(blk))
		{
			Buffer		buf = ReadBuffer(index, blk);
			Page		page;
			int			lo;
			BlockNumber next;
			bool		found = false;
			int			d;

			LockBuffer(buf, BUFFER_LOCK_SHARE);
			page = BufferGetPage(buf);
			lo = lion_store_page_header(page)->lo;
			next = LionPageGetOpaque(page)->rightlink;
			UnlockReleaseBuffer(buf);

			for (d = 0; d < wh.ndir; d++)
				if (dir[d].ord == ord && dir[d].lo == lo)
				{
					if (dir[d].blk != blk)
						ereport(ERROR,
								(errcode(ERRCODE_INDEX_CORRUPTED),
								 errmsg("lion index \"%s\": the directory of window %u names block %u for column %d virtual page %d, which is at block %u",
										RelationGetRelationName(index), ckey,
										dir[d].blk, col->attno, lo, blk)));
					found = true;
					ndirseen++;
				}
			if (!found && lo > 0 && lo < nvpages &&
				(wh.flags & LION_STORE_WH_THIN) == 0)
				ereport(ERROR,
						(errcode(ERRCODE_INDEX_CORRUPTED),
						 errmsg("lion index \"%s\": the directory of window %u does not name block %u, column %d from virtual page %d",
								RelationGetRelationName(index), ckey, blk,
								col->attno, lo)));
			if (BlockNumberIsValid(next))
				lion_store_chain_step(index, blk, next, &steps);
			blk = next;
		}
	}
	if (ndirseen != wh.ndir)
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg("lion index \"%s\": the directory of window %u has %u entries, %d of them pages of its chains",
						RelationGetRelationName(index), ckey,
						(unsigned) wh.ndir, ndirseen)));
}

/*
 * What lion_index_store_window() shows of window ckey of an ordered index
 * (§41): its permutation's generation and window header, the positions with
 * an entry, the order column's append-region slots, and the pages of its
 * chains.  The chains are read as the sort reads them, and a damaged one is
 * an ERROR.
 */
void
lion_store_window_info(Relation index, LionIndexState *ix, uint32 ckey,
					   LionStoreWindowInfo *wi)
{
	int			stride = lion_store_stride(ix);
	LionStoreModel *m = (LionStoreModel *) palloc(sizeof(LionStoreModel));
	int			ord;
	int			k;

	memset(wi, 0, sizeof(LionStoreWindowInfo));
	if (ix->store_order < 0)
		return;
	for (ord = 0; ord < stride; ord++)
	{
		BlockNumber blk = lion_storemap_head(index, ix->store.store_root,
											 lion_storemap_slot(ckey, stride, ord),
											 NULL);
		int			steps = 0;

		if (BlockNumberIsValid(blk))
			wi->exists = true;
		while (BlockNumberIsValid(blk))
		{
			Buffer		buf = ReadBuffer(index, blk);
			BlockNumber next;

			LockBuffer(buf, BUFFER_LOCK_SHARE);
			if (!lion_store_page_owned(BufferGetPage(buf), ckey, (uint16) ord))
			{
				UnlockReleaseBuffer(buf);
				ereport(ERROR,
						(errcode(ERRCODE_INDEX_CORRUPTED),
						 errmsg("lion index \"%s\": block %u on the store chain of window %u ordinal %d is not one of its pages",
								RelationGetRelationName(index), blk, ckey, ord)));
			}
			if (ord == ix->nstored && steps == 0)
				wi->gen = lion_store_flags_gen(lion_store_page_header(BufferGetPage(buf))->flags);
			next = LionPageGetOpaque(BufferGetPage(buf))->rightlink;
			UnlockReleaseBuffer(buf);
			if (ord == ix->nstored)
				wi->perm_pages++;
			else
				wi->pages++;
			if (BlockNumberIsValid(next))
				lion_store_chain_step(index, blk, next, &steps);
			blk = next;
		}
	}
	if (!wi->exists)
		return;

	/* the permutation */
	{
		BlockNumber head = lion_storemap_head(index, ix->store.store_root,
											  lion_storemap_slot(ckey, stride, ix->nstored),
											  NULL);

		if (BlockNumberIsValid(head))
		{
			LionStoreWinHdr wh;

			lion_store_read_chain(index, ix->storeperm, ckey, head, wi->gen, m);
			if (m->extra != NULL)
			{
				memcpy(&wh, m->extra, sizeof(LionStoreWinHdr));
				wi->nsorted = wh.nsorted;
				wi->vwidth = wh.vwidth;
				wi->ndir = wh.ndir;
				wi->thin = (wh.flags & LION_STORE_WH_THIN) != 0;
			}
			for (k = 0; k <= m->hi; k++)
			{
				OffsetNumber off;
				uint16		vlo;

				for (off = FirstOffsetNumber; off <= m->nslots[k]; off++)
					if (lion_store_perm_entry(m, k, off, &vlo))
						wi->entries++;
			}
		}
	}

	/* the order column's append region */
	{
		const LionStoreCol *ocol = &ix->stored[ix->store_order];
		BlockNumber head = lion_storemap_head(index, ix->store.store_root,
											  lion_storemap_slot(ckey, stride, ocol->ord),
											  NULL);

		if (BlockNumberIsValid(head))
		{
			lion_store_read_chain(index, ocol, ckey, head, wi->gen, m);
			for (k = LION_STORE_APPEND_VPAGE; k <= m->hi; k++)
			{
				OffsetNumber off;

				if (m->absent[k] || m->slots[k] == NULL)
					continue;
				for (off = FirstOffsetNumber; off <= m->nslots[k]; off++)
					wi->appended += (m->slots[k][off - 1].data != NULL);
			}
		}
	}
}

bool
lion_store_page_linked(Relation index, LionIndexState *ix, BlockNumber blk,
					   uint32 ckey, uint16 ord)
{
	uint64		slot;
	BlockNumber head;
	BlockNumber cur;
	int			steps = 0;

	if ((int) ord >= lion_store_stride(ix))
		return false;			/* not a column this index stores */
	slot = lion_storemap_slot(ckey, lion_store_stride(ix), ord);
	head = lion_storemap_head(index, ix->store.store_root, slot, NULL);

	cur = head;
	while (BlockNumberIsValid(cur))
	{
		Buffer		buf;
		Page		page;
		BlockNumber next;

		if (cur == blk)
			return true;
		buf = ReadBuffer(index, cur);
		LockBuffer(buf, BUFFER_LOCK_SHARE);
		page = BufferGetPage(buf);
		if (!lion_store_page_owned(page, ckey, ord))
		{
			/*
			 * The chain is not what it should be.  This question decides
			 * whether a page is freed, so a doubt answers "linked".
			 */
			UnlockReleaseBuffer(buf);
			return true;
		}
		next = LionPageGetOpaque(page)->rightlink;
		UnlockReleaseBuffer(buf);
		if (BlockNumberIsValid(next))
		{
			if (next == cur || ++steps >= LION_STORE_MAX_VPAGES)
				return true;
		}
		cur = next;
	}
	return false;
}

/* ---------------------------------------------------------------------
 * 8. The gather (DESIGN.md §40, "Reads")
 *
 * See lion_store.h for why what it returns may be used, and when.  The page
 * is locked SHARE only while one heap page's sub-array is decoded, and
 * checked every time it is locked: it may have been split, rewritten, freed
 * and reused since the last look, and the check is what tells.
 * --------------------------------------------------------------------- */

struct LionStoreReader
{
	Relation	index;
	LionIndexState *ix;
	const LionStoreCol *col;
	MemoryContext cxt;			/* the reader's own */
	MemoryContext valcxt;		/* the values returned since the last reset */
	LionStoreCache cache;		/* the map blocks, and the last head */
	LionStoreCache pcache;		/* the same for the permutation (§41) */

	/*
	 * A DICT code's value, for the members of one heap page: decoded once
	 * per code per lock hold, since the dictionary is only stable while the
	 * page stays locked.  stamp[c] == gen says val[c] is this hold's.
	 */
	Datum	   *dval;
	uint32	   *dstamp;
	uint32		dcap;
	uint32		gen;

	/*
	 * Of the last gather's absent pages, those that are not ABSENT but have
	 * no store at all, or not for that offset: for verify(), to which a
	 * visible row without a value is damage (lion_store_compare()).
	 */
	uint64		missing;

	/*
	 * An ordered window's gather (§41): the members' permutation entries,
	 * their virtual lo in ascending order with the member each is, and the
	 * values read in that order; room for scap members.  dir holds the
	 * window header's directory entries for the reader's column.
	 */
	int			scap;
	Datum	   *pval;
	bool	   *pnull;
	uint32	   *vorder;			/* vlo << 16 | member */
	uint16	   *vlo;
	Datum	   *vval;
	bool	   *vnull;
	LionStoreDirEnt *dir;
	int			ndir;
	int			dircap;

	/*
	 * The index's tablespace's page costs, which decide per window whether a
	 * data walk jumps by the directory or reads its pages in order.
	 */
	double		rpc;
	double		spc;

	/*
	 * The permutation shared among the readers of one index's columns
	 * (lion_store_share()): a follower's leader, and a leader's last
	 * permutation read - the window, the members, their entries in
	 * pval/pnull, the generation and the whole directory - valid while
	 * pvalid.
	 */
	LionStoreReader *leader;
	bool		pvalid;
	uint32		pckey;
	int			pnlo;
	uint16	   *plo;
	int			pgen;
	bool		pheadseen;
	LionStoreDirEnt *alldir;
	int			nalldir;
	int			alldircap;
};

/*
 * One walk of one chain of a window (lion_store_walk()): what it is told,
 * and what it found.  Pages are counted in virtual pages for an ordered
 * index, so the masks have a bit for each of LION_STORE_MAX_VPAGES.
 */
typedef struct LionStoreWalk
{
	int			gen;			/* every page's generation; -1: any */
	bool		takegen;		/* the head's generation becomes gen */
	bool		headseen;		/* the head was read */
	bool		wanthdr;		/* keep the head's window header (PERM) */
	const LionStoreDirEnt *dir; /* entries for the column, ascending lo */
	int			ndir;
	bool		nojump;			/* enter by the directory, then walk right */
	uint64		absent[2];		/* pages ABSENT, or without the slot */
	uint64		missing[2];		/* of those, the ones not ABSENT */
} LionStoreWalk;

static inline void
lion_store_mask_set(uint64 *mask, int k)
{
	mask[k / 64] |= ((uint64) 1) << (k % 64);
}

static inline bool
lion_store_mask_get(const uint64 *mask, int k)
{
	return (mask[k / 64] & (((uint64) 1) << (k % 64))) != 0;
}

LionStoreReader *
lion_store_open(Relation index, LionIndexState *ix, int ord, MemoryContext cxt)
{
	MemoryContext rcxt;
	LionStoreReader *r;

	if (ord < 0 || ord >= ix->nstored)
		elog(ERROR, "lion index \"%s\" has no stored column %d",
			 RelationGetRelationName(index), ord);

	rcxt = AllocSetContextCreate(cxt, "lion store reader", ALLOCSET_SMALL_SIZES);
	r = (LionStoreReader *) MemoryContextAllocZero(rcxt, sizeof(LionStoreReader));
	r->index = index;
	r->ix = ix;
	r->col = &ix->stored[ord];
	r->cxt = rcxt;
	r->valcxt = AllocSetContextCreate(rcxt, "lion store values",
									  ALLOCSET_DEFAULT_SIZES);
	r->cache.innerblk = InvalidBlockNumber;
	r->cache.leafblk = InvalidBlockNumber;
	r->cache.head = InvalidBlockNumber;
	r->pcache.innerblk = InvalidBlockNumber;
	r->pcache.leafblk = InvalidBlockNumber;
	r->pcache.head = InvalidBlockNumber;
	get_tablespace_page_costs(index->rd_rel->reltablespace, &r->rpc, &r->spc);
	return r;
}

void
lion_store_share(LionStoreReader *r, LionStoreReader *leader)
{
	Assert(r != leader && r->index == leader->index && r->ix == leader->ix);
	Assert(leader->leader == NULL);
	r->leader = leader;
}

void
lion_store_close(LionStoreReader *r)
{
	MemoryContextDelete(r->cxt);
}

void
lion_store_gather_reset(LionStoreReader *r)
{
	MemoryContextReset(r->valcxt);
}

/* The head of map slot `slot`, from the cache or the map. */
static BlockNumber
lion_store_reader_head(LionStoreReader *r, LionStoreCache *cache, uint64 slot,
					   bool fresh)
{
	if (!fresh && cache->slot == slot && BlockNumberIsValid(cache->head))
		return cache->head;
	cache->slot = slot;
	cache->head = lion_storemap_head(r->index, r->ix->store.store_root, slot,
									 cache);
	return cache->head;
}

/* Member values of (heap or virtual) page k, from its checked sub-array s. */
static void
lion_store_gather_page(LionStoreReader *r, const LionStoreCol *col, Page page,
					   BlockNumber blk, const LionStoreHeader *h,
					   const LionStoreSub *s, const uint16 *lo, int from, int to,
					   Datum *values, bool *isnull, LionStoreWalk *w)
{
	const uint8 *body = (const uint8 *) s + sizeof(LionStoreSub);
	const char *ditem = NULL;
	Size		dlen = 0;
	int			j;

	if (h->mode == LION_STORE_DICT)
	{
		ItemId		diid = PageGetItemId(page, LION_STORE_DICT_OFF);

		ditem = (const char *) PageGetItem(page, diid);
		dlen = ItemIdGetLength(diid);
		if (!col->typbyval)
		{
			if (r->dcap < (uint32) h->ndict + 1)
			{
				uint32		cap = Max(64, (uint32) h->ndict + 1);

				if (r->dval != NULL)
				{
					pfree(r->dval);
					pfree(r->dstamp);
				}
				r->dval = (Datum *) MemoryContextAlloc(r->cxt, sizeof(Datum) * cap);
				r->dstamp = (uint32 *) MemoryContextAllocZero(r->cxt, sizeof(uint32) * cap);
				r->dcap = cap;
				r->gen = 0;
			}
			if (++r->gen == 0)
			{
				memset(r->dstamp, 0, sizeof(uint32) * r->dcap);
				r->gen = 1;
			}
		}
	}

	for (j = from; j < to; j++)
	{
		uint16		k;
		OffsetNumber off;
		uint32		i;

		lion_lo_split(lo[j], &k, &off);
		Assert(off >= FirstOffsetNumber);
		if (off > s->nslots)
		{
			/*
			 * A slot no insert has reached.  The order of §40 ("Writes")
			 * says a member's slot is written before the member exists, so
			 * this is not a member a caller can be asking about in earnest;
			 * the heap answers it.
			 */
			lion_store_mask_set(w->absent, k);
			lion_store_mask_set(w->missing, k);
			continue;
		}
		i = off - 1;

		if (h->mode == LION_STORE_DICT)
		{
			uint32		c = lion_store_code_get(body, h->width, i);
			uint32		len;
			const char *data;

			if (c == 0)
				continue;		/* NULL */
			if (c > h->ndict)
				ereport(ERROR,
						(errcode(ERRCODE_INDEX_CORRUPTED),
						 errmsg("lion index \"%s\": store page %u has code %u of a dictionary of %u entries",
								RelationGetRelationName(r->index), blk, c,
								(unsigned) h->ndict),
						 errhint("REINDEX the index.")));
			if (!col->typbyval && r->dstamp[c] == r->gen)
			{
				values[j] = r->dval[c];
				isnull[j] = false;
				continue;
			}
			data = lion_store_dict_entry(r->index, col, ditem, dlen, h->ndict,
										 c, &len);
			values[j] = lion_store_datum(col, data, len, r->valcxt);
			isnull[j] = false;
			if (!col->typbyval)
			{
				r->dval[c] = values[j];
				r->dstamp[c] = r->gen;
			}
		}
		else
		{
			const uint8 *slot = body + (Size) i * h->width;

			if (lion_store_null_get(body + (Size) s->nslots * h->width, i))
				continue;		/* NULL */
			if (col->typlen > 0)
				values[j] = lion_store_datum(col, (const char *) slot,
											 (uint32) col->typlen, r->valcxt);
			else
			{
				uint16		vlen;

				memcpy(&vlen, slot, sizeof(uint16));
				if ((int) vlen > (int) h->width - (int) sizeof(uint16))
					ereport(ERROR,
							(errcode(ERRCODE_INDEX_CORRUPTED),
							 errmsg("lion index \"%s\": store page %u has a value of %u bytes in a slot of %u",
									RelationGetRelationName(r->index), blk,
									(unsigned) vlen, (unsigned) h->width),
							 errhint("REINDEX the index.")));
				values[j] = lion_store_datum(col, (const char *) slot + sizeof(uint16),
											 vlen, r->valcxt);
			}
			isnull[j] = false;
		}
	}
}

/*
 * The directory's page for page k: the entry with the greatest first page
 * at or below k, or -1.  The directory is ascending by first page.
 */
static int
lion_store_dir_find(const LionStoreWalk *w, int k)
{
	int			lo = 0;
	int			hi = w->ndir - 1;
	int			best = -1;

	while (lo <= hi)
	{
		int			mid = (lo + hi) / 2;

		if ((int) w->dir[mid].lo <= k)
		{
			best = mid;
			lo = mid + 1;
		}
		else
			hi = mid - 1;
	}
	return best;
}

/* Of a window header's directory entries, the reader's column's. */
static void
lion_store_pick_dir(LionStoreReader *r, const LionStoreDirEnt *all, int n)
{
	int			i;

	if (r->dircap < n)
	{
		if (r->dir != NULL)
			pfree(r->dir);
		r->dircap = Max(16, n);
		r->dir = (LionStoreDirEnt *)
			MemoryContextAlloc(r->cxt, sizeof(LionStoreDirEnt) * r->dircap);
	}
	r->ndir = 0;
	for (i = 0; i < n; i++)
	{
		if (all[i].ord != (uint16) r->col->ord)
			continue;
		if (r->ndir > 0 && all[i].lo <= r->dir[r->ndir - 1].lo)
			continue;			/* out of order: verify() says so */
		r->dir[r->ndir++] = all[i];
	}
}

/* Keep the window header's directory, and the reader's column's entries. */
static void
lion_store_keep_dir(LionStoreReader *r, Page page)
{
	ItemId		iid = PageGetItemId(page, LION_STORE_DICT_OFF);
	const char *item = (const char *) PageGetItem(page, iid);
	LionStoreWinHdr wh;
	int			i;

	/* lion_store_check_header() checked the sizes */
	memcpy(&wh, item + sizeof(LionStoreDict), sizeof(LionStoreWinHdr));
	if (r->alldircap < wh.ndir)
	{
		if (r->alldir != NULL)
			pfree(r->alldir);
		r->alldircap = Max(16, wh.ndir);
		r->alldir = (LionStoreDirEnt *)
			MemoryContextAlloc(r->cxt, sizeof(LionStoreDirEnt) * r->alldircap);
	}
	r->nalldir = wh.ndir;
	for (i = 0; i < wh.ndir; i++)
		memcpy(&r->alldir[i], item + sizeof(LionStoreDict) +
			   sizeof(LionStoreWinHdr) + (Size) i * sizeof(LionStoreDirEnt),
			   sizeof(LionStoreDirEnt));
	lion_store_pick_dir(r, r->alldir, r->nalldir);
}

/*
 * Walk the chain of map slot `slot` (column col of window ckey) for the
 * sorted addresses lo[0 .. nlo - 1], heap lo for a heap-order or a
 * permutation chain, virtual lo for a data chain of an ordered index.
 * false when a page is of another generation than w->gen: the window
 * changed under the read, and the caller reads it again.
 */
static bool
lion_store_walk(LionStoreReader *r, const LionStoreCol *col,
				LionStoreCache *cache, uint32 ckey, uint64 slot, bool fresh,
				const uint16 *lo, int nlo, Datum *values, bool *isnull,
				LionStoreWalk *w)
{
	BlockNumber head;
	BlockNumber blk = InvalidBlockNumber;
	Buffer		buf = InvalidBuffer;
	int			expect = -1;	/* the first page a directory jump expects */
	int			restarts = 0;
	int			steps = 0;
	int			i;

	for (i = 0; i < nlo; i++)
	{
		values[i] = (Datum) 0;
		isnull[i] = true;
#ifdef USE_ASSERT_CHECKING
		if (i > 0)
			Assert(lo[i - 1] < lo[i]);
#endif
	}
	if (nlo == 0)
		return true;

	head = lion_store_reader_head(r, cache, slot, fresh);

	/*
	 * Nothing is locked or pinned here: a VACUUM may free the chain under
	 * this head, and an insert take its pages, before the read below locks
	 * it (test/isolation/store_vacuum_reader.spec parks a reader here).
	 */
	LION_INJECTION_POINT("lion-store-gather-head");
	i = 0;
	while (i < nlo)
	{
		uint16		k;
		uint16		k2;
		OffsetNumber off;
		int			end;
		Page		page;
		LionStoreHeader h;
		LionStoreSub *s;
		Size		slen;
		char	   *msg;
		bool		ok;
		BlockNumber next;

		lion_lo_split(lo[i], &k, &off);
		for (end = i + 1; end < nlo; end++)
		{
			lion_lo_split(lo[end], &k2, &off);
			if (k2 != k)
				break;
		}

		if (!BlockNumberIsValid(head))
		{
			/* the window has no store for this column (yet) */
			lion_store_mask_set(w->absent, k);
			lion_store_mask_set(w->missing, k);
			i = end;
			continue;
		}
		if (!BlockNumberIsValid(blk))
		{
			int			d = lion_store_dir_find(w, k);

			blk = head;
			expect = -1;
			if (d >= 0 && w->dir[d].lo > 0)
			{
				blk = w->dir[d].blk;
				expect = w->dir[d].lo;
			}
		}

		if (!BufferIsValid(buf) || BufferGetBlockNumber(buf) != blk)
		{
			if (BufferIsValid(buf))
				ReleaseBuffer(buf);
			buf = ReadBuffer(r->index, blk);
		}
		LockBuffer(buf, BUFFER_LOCK_SHARE);
		page = BufferGetPage(buf);

		/*
		 * The page this read was led to must still be this window's store
		 * for this column, and a head must still be one (§18; the same test
		 * the insert path makes, lion_store_find_page()).  If it is not, the
		 * chain was freed under a cached head, or the map was read before a
		 * cleanup: read the map again.
		 */
		ok = false;
		if (lion_store_page_owned(page, ckey, (uint16) col->ord))
		{
			if ((msg = lion_store_check_header(page, col)) != NULL)
			{
				LockBuffer(buf, BUFFER_LOCK_UNLOCK);
				ereport(ERROR,
						(errcode(ERRCODE_INDEX_CORRUPTED),
						 errmsg("lion index \"%s\": store page %u %s",
								RelationGetRelationName(r->index), blk, msg),
						 errhint("REINDEX the index.")));
			}
			memcpy(&h, lion_store_page_header(page), sizeof(LionStoreHeader));
			ok = !(blk == head && h.lo != 0) && k >= h.lo;
		}

		/*
		 * A directory entry is the block a sort wrote for a page starting at
		 * its first page (§41).  One that no longer is (which nothing of
		 * this generation should cause) costs the jump, not the read: walk
		 * from the head instead.
		 */
		if (expect >= 0 && (!ok || h.lo != expect))
		{
			LockBuffer(buf, BUFFER_LOCK_UNLOCK);
			w->ndir = 0;
			blk = InvalidBlockNumber;
			expect = -1;
			continue;
		}
		expect = -1;

		if (!ok)
		{
			LockBuffer(buf, BUFFER_LOCK_UNLOCK);
			if (++restarts > LION_STORE_MAX_ATTEMPTS)
				ereport(ERROR,
						(errcode(ERRCODE_INDEX_CORRUPTED),
						 errmsg("lion index \"%s\": could not find the store page of window %u column %d",
								RelationGetRelationName(r->index), ckey,
								col->attno),
						 errhint("REINDEX the index.")));
			head = lion_store_reader_head(r, cache, slot, true);
			blk = InvalidBlockNumber;
			steps = 0;
			continue;
		}

		/* the generation (§41, "Reads") */
		if (blk == head)
		{
			if (w->takegen)
			{
				w->gen = lion_store_flags_gen(h.flags);
				w->takegen = false;
			}
			if (w->wanthdr && h.lo == 0)
			{
				lion_store_keep_dir(r, page);
				w->wanthdr = false;
			}
			w->headseen = true;
		}
		if (w->gen >= 0 && lion_store_flags_gen(h.flags) != w->gen)
		{
			LockBuffer(buf, BUFFER_LOCK_UNLOCK);
			ReleaseBuffer(buf);
			return false;
		}

		if (k > h.hi)
		{
			int			d = lion_store_dir_find(w, k);

			next = LionPageGetOpaque(page)->rightlink;
			LockBuffer(buf, BUFFER_LOCK_UNLOCK);
			if (!w->nojump && d >= 0 && w->dir[d].lo > h.hi)
			{
				/* the directory knows a page further on: go straight there */
				blk = w->dir[d].blk;
				expect = w->dir[d].lo;
				continue;
			}
			if (!BlockNumberIsValid(next))
			{
				/* past every range: no row of page k is stored yet */
				lion_store_mask_set(w->absent, k);
				lion_store_mask_set(w->missing, k);
				i = end;
				continue;
			}
			lion_store_chain_step(r->index, blk, next, &steps);
			blk = next;
			continue;
		}

		if ((msg = lion_store_check_sub(page, &h, k)) != NULL)
		{
			LockBuffer(buf, BUFFER_LOCK_UNLOCK);
			ereport(ERROR,
					(errcode(ERRCODE_INDEX_CORRUPTED),
					 errmsg("lion index \"%s\": store page %u %s",
							RelationGetRelationName(r->index), blk, msg),
					 errhint("REINDEX the index.")));
		}
		s = lion_store_page_sub(page, k, &slen);
		if ((s->flags & LION_STORE_ABSENT) != 0)
			lion_store_mask_set(w->absent, k);
		else
			lion_store_gather_page(r, col, page, blk, &h, s, lo, i, end, values,
								   isnull, w);
		LockBuffer(buf, BUFFER_LOCK_UNLOCK);
		i = end;
	}

	if (BufferIsValid(buf))
		ReleaseBuffer(buf);
	return true;
}

/*
 * Jump or read on (§41, "As built: the walk decides per window"), for a data
 * walk over the ascending virtual lo vlo[0 .. nlo - 1].  The directory says
 * which of the chain's pages hold them: a walk that enters at the first and
 * jumps to each of the others reads J pages at random, one that enters at the
 * first and reads on to the last reads R pages, all but the first in chain
 * order - consecutive blocks, as a build or a split lays them, which the
 * kernel reads ahead.  The walk jumps when J random reads cost less than one
 * random and R - 1 sequential ones at the tablespace's page costs, and asks
 * for the pages it will jump to ahead of the walk; otherwise it reads on.
 */
static void
lion_store_plan_walk(LionStoreReader *r, LionStoreWalk *w, const uint16 *vlo,
					 int nlo)
{
	int			first = -1;
	int			last = -1;
	int			npages = 0;
	int			i;

	if (w->ndir == 0 || nlo == 0)
		return;					/* nothing to jump with: the walk reads on */
	for (i = 0; i < nlo; i++)
	{
		int			d = lion_store_dir_find(w, lion_store_vlo_page(vlo[i]));

		if (d != last || npages == 0)
		{
			npages++;
			if (first < 0)
				first = d;
		}
		last = d;
	}
	if ((double) npages * r->rpc <
		r->rpc + (double) (last - first) * r->spc)
	{
		int			prev = first;

		for (i = 0; i < nlo; i++)
		{
			int			d = lion_store_dir_find(w, lion_store_vlo_page(vlo[i]));

			if (d != prev && d >= 0)
				(void) PrefetchBuffer(r->index, MAIN_FORKNUM, w->dir[d].blk);
			prev = d;
		}
		return;
	}
	w->nojump = true;
}

/* Room in the reader for an ordered gather of n members. */
static void
lion_store_reader_room(LionStoreReader *r, int n)
{
	if (r->scap >= n)
		return;
	if (r->scap > 0)
	{
		pfree(r->pval);
		pfree(r->pnull);
		pfree(r->vorder);
		pfree(r->vlo);
		pfree(r->vval);
		pfree(r->vnull);
		pfree(r->plo);
	}
	r->pvalid = false;
	r->scap = Max(n, 256);
	r->pval = (Datum *) MemoryContextAlloc(r->cxt, sizeof(Datum) * r->scap);
	r->pnull = (bool *) MemoryContextAlloc(r->cxt, sizeof(bool) * r->scap);
	r->vorder = (uint32 *) MemoryContextAlloc(r->cxt, sizeof(uint32) * r->scap);
	r->vlo = (uint16 *) MemoryContextAlloc(r->cxt, sizeof(uint16) * r->scap);
	r->vval = (Datum *) MemoryContextAlloc(r->cxt, sizeof(Datum) * r->scap);
	r->vnull = (bool *) MemoryContextAlloc(r->cxt, sizeof(bool) * r->scap);
	r->plo = (uint16 *) MemoryContextAlloc(r->cxt, sizeof(uint16) * r->scap);
}

static int
lion_store_uint32_cmp(const void *a, const void *b)
{
	uint32		x = *(const uint32 *) a;
	uint32		y = *(const uint32 *) b;

	return (x > y) - (x < y);
}

/*
 * A follower's permutation from its leader (lion_store_share()), when the
 * leader's last read was of this window and these members: the entries, the
 * generation, and the follower's own entries of the directory.  The data
 * walk that follows checks every page against that generation, as after a
 * read of its own; a mismatch reads the permutation again itself.
 */
static bool
lion_store_shared_perm(LionStoreReader *r, uint32 ckey, const uint16 *lo,
					   int nlo, LionStoreWalk *pw)
{
	LionStoreReader *l = r->leader;

	if (l == NULL || !l->pvalid || l->pckey != ckey || l->pnlo != nlo ||
		memcmp(l->plo, lo, sizeof(uint16) * nlo) != 0)
		return false;
	memcpy(r->pval, l->pval, sizeof(Datum) * nlo);
	memcpy(r->pnull, l->pnull, sizeof(bool) * nlo);
	pw->gen = l->pgen;
	pw->headseen = l->pheadseen;
	lion_store_pick_dir(r, l->alldir, l->nalldir);
	return true;
}

/*
 * The gather of an ordered window (§41, "Reads"): the members' permutation
 * entries, then the column's chain at the virtual lo they give, ascending;
 * a page of another generation than the permutation's reads the window again.
 */
static void
lion_store_gather_ordered(LionStoreReader *r, uint32 ckey, const uint16 *lo,
						  int nlo, Datum *values, bool *isnull,
						  uint64 *absent_pages)
{
	const LionStoreCol *col = r->col;
	int			stride = lion_store_stride(r->ix);
	uint64		pslot = lion_storemap_slot(ckey, stride, r->ix->nstored);
	uint64		dslot = lion_storemap_slot(ckey, stride, col->ord);
	int			attempt;
	int			i;

	lion_store_reader_room(r, nlo);
	for (attempt = 0; attempt <= LION_STORE_MAX_ATTEMPTS; attempt++)
	{
		LionStoreWalk pw;
		LionStoreWalk dw;
		bool		fresh = (attempt > 0);

		memset(&pw, 0, sizeof(pw));
		pw.gen = -1;
		pw.takegen = true;
		pw.wanthdr = true;
		r->ndir = 0;
		r->nalldir = 0;
		r->pvalid = false;
		if (!fresh && lion_store_shared_perm(r, ckey, lo, nlo, &pw))
		{
			/* the leader's read of this window's permutation, just made */
		}
		else if (!lion_store_walk(r, r->ix->storeperm, &r->pcache, ckey, pslot,
								  fresh, lo, nlo, r->pval, r->pnull, &pw))
			continue;
		else if (r->leader == NULL)
		{
			/* for the followers, which gather the same members next */
			r->pvalid = true;
			r->pckey = ckey;
			r->pnlo = nlo;
			memcpy(r->plo, lo, sizeof(uint16) * nlo);
			r->pgen = pw.gen;
			r->pheadseen = pw.headseen;
		}

		/*
		 * Each member's address: its sorted slot, or with no entry its heap
		 * position in the append region.  A window without a permutation
		 * has had no sort, and its pages are of generation 0.
		 */
		for (i = 0; i < nlo; i++)
		{
			uint16		v;

			if (r->pnull[i])
				v = lion_store_append_vlo(lo[i]);
			else
			{
				v = (uint16) DatumGetInt16(r->pval[i]);
				if (lion_store_vlo_page(v) >= LION_STORE_APPEND_VPAGE ||
					lion_store_vlo_off(v) < FirstOffsetNumber)
					ereport(ERROR,
							(errcode(ERRCODE_INDEX_CORRUPTED),
							 errmsg("lion index \"%s\": the permutation of window %u gives heap lo %u the slot %u",
									RelationGetRelationName(r->index), ckey,
									(unsigned) lo[i], (unsigned) v),
							 errhint("REINDEX the index.")));
			}
			r->vorder[i] = ((uint32) v << 16) | (uint32) i;
		}
		qsort(r->vorder, nlo, sizeof(uint32), lion_store_uint32_cmp);
		for (i = 0; i < nlo; i++)
		{
			r->vlo[i] = (uint16) (r->vorder[i] >> 16);
			if (i > 0 && r->vlo[i] == r->vlo[i - 1])
				ereport(ERROR,
						(errcode(ERRCODE_INDEX_CORRUPTED),
						 errmsg("lion index \"%s\": the permutation of window %u gives two rows the slot %u",
								RelationGetRelationName(r->index), ckey,
								(unsigned) r->vlo[i]),
						 errhint("REINDEX the index.")));
		}

		memset(&dw, 0, sizeof(dw));
		dw.gen = pw.headseen ? pw.gen : 0;
		dw.dir = r->dir;
		dw.ndir = r->ndir;
		lion_store_plan_walk(r, &dw, r->vlo, nlo);
		if (!lion_store_walk(r, col, &r->cache, ckey, dslot, fresh, r->vlo, nlo,
							 r->vval, r->vnull, &dw))
			continue;

		/* back to the members' order, and their heap pages' bits */
		for (i = 0; i < nlo; i++)
		{
			int			j = (int) (r->vorder[i] & 0xFFFF);
			int			vp = lion_store_vlo_page(r->vlo[i]);
			uint16		k;
			OffsetNumber off;

			values[j] = r->vval[i];
			isnull[j] = r->vnull[i];
			lion_lo_split(lo[j], &k, &off);
			if (lion_store_mask_get(dw.absent, vp))
				*absent_pages |= ((uint64) 1) << k;
			if (lion_store_mask_get(dw.missing, vp))
				r->missing |= ((uint64) 1) << k;
		}
		return;
	}
	ereport(ERROR,
			(errcode(ERRCODE_INDEX_CORRUPTED),
			 errmsg("lion index \"%s\": window %u changed under every one of %d reads",
					RelationGetRelationName(r->index), ckey,
					LION_STORE_MAX_ATTEMPTS),
			 errhint("REINDEX the index.")));
}

void
lion_store_gather(LionStoreReader *r, uint32 ckey, const uint16 *lo, int nlo,
				  Datum *values, bool *isnull, uint64 *absent_pages)
{
	LionStoreWalk w;
	int			i;

	*absent_pages = 0;
	r->missing = 0;
	if (nlo == 0)
		return;
	if (r->ix->store_order >= 0)
	{
		for (i = 0; i < nlo; i++)
		{
			values[i] = (Datum) 0;
			isnull[i] = true;
		}
		lion_store_gather_ordered(r, ckey, lo, nlo, values, isnull,
								  absent_pages);
		return;
	}

	memset(&w, 0, sizeof(w));
	w.gen = -1;
	(void) lion_store_walk(r, r->col, &r->cache, ckey,
						   lion_storemap_slot(ckey, r->ix->nstored, r->col->ord),
						   false, lo, nlo, values, isnull, &w);
	*absent_pages = w.absent[0];
	r->missing = w.missing[0];
}

bool
lion_storemap_linked(Relation index, LionIndexState *ix, BlockNumber blk,
					 uint16 level, uint64 number)
{
	BlockNumber root = ix->store.store_root;

	if (level == LION_STOREMAP_ROOT)
		return number == 0 && blk == root;
	if (level == LION_STOREMAP_INNER)
		return number < LION_STOREMAP_FANOUT &&
			lion_storemap_read(index, root, LION_STOREMAP_ROOT, 0,
							   (uint32) number) == blk;
	if (level == LION_STOREMAP_LEAF)
		return number / LION_STOREMAP_FANOUT < LION_STOREMAP_FANOUT &&
			lion_storemap_leaf(index, root, number, NULL) == blk;
	return false;
}

int
lion_store_compare(LionStoreReader *r, ItemPointer tid, Datum d, bool isnull)
{
	uint64		code = lion_tid_to_code(tid);
	uint16		lo = lion_code_lo(code);
	Datum		got;
	bool		gotnull;
	uint64		absent;
	LionStoreVal want;
	LionStoreVal have;
	Datum		fix1;
	Datum		fix2;
	int			result;

	lion_store_gather(r, lion_code_ckey(code), &lo, 1, &got, &gotnull, &absent);
	if (absent != 0)
		return (r->missing != 0) ? LION_STORE_CMP_MISSING : LION_STORE_CMP_ABSENT;
	if (gotnull || isnull)
		result = (gotnull == isnull) ? LION_STORE_CMP_EQUAL : LION_STORE_CMP_DIFFERENT;
	else
	{
		lion_store_value(r->index, r->col, d, false, &want, &fix1);
		lion_store_value(r->index, r->col, got, false, &have, &fix2);
		result = lion_store_val_equal(&want, have.data, have.len) ?
			LION_STORE_CMP_EQUAL : LION_STORE_CMP_DIFFERENT;
	}
	lion_store_gather_reset(r);
	return result;
}

/* ---------------------------------------------------------------------
 * 9. The readers' page model (DESIGN.md §41, "Costs")
 *
 * Every reader's cost function prices a window's chain of a column as it
 * would for heap order, and asks here what an ordered index changes: the
 * permutation's pages are not a column's, every gather reads the
 * permutation too, and the members of one value of the order column are
 * adjacent slots.
 * --------------------------------------------------------------------- */

void
lion_store_shape(LionIndexState *ix, IndexOptInfo *idx, double storepages,
				 LionStoreShape *sh)
{
	RelOptInfo *rel = idx->rel;
	double		windows = Max(1.0, ceil(Max((double) rel->pages, 1.0) /
										LION_BLOCKS_PER_CONTAINER));

	memset(sh, 0, sizeof(LionStoreShape));
	sh->datapages = storepages;
	sh->rowsper = Max(rel->tuples, 1.0) / windows;
	if (ix->store_order < 0 || ix->nstored == 0)
		return;
	sh->ordered = true;

	/*
	 * Two bytes a row and the sub-arrays' headers: a window of up to about
	 * 3,500 rows has one permutation page.  The meta page counts the store's
	 * pages all together, so the columns' share is what is left.
	 */
	sh->permpages = Max(1.0, ceil(sh->rowsper * (sizeof(uint16) + 0.25) /
								  (double) LION_PAGE_CAPACITY));
	sh->datapages = Max(storepages - sh->permpages * windows, 0.0);
	sh->ordercol = ix->stored[ix->store_order].attno;
}

void
lion_store_shape_pin(IndexOptInfo *idx, LionStoreShape *sh, List *clauses,
					 List *except)
{
	RelOptInfo *rel = idx->rel;
	AttrNumber	hattno;
	double		best = 0.0;
	ListCell   *lc;

	sh->nvals = 0.0;
	if (!sh->ordered || sh->ordercol < 1 || sh->ordercol > idx->ncolumns)
		return;
	hattno = idx->indexkeys[sh->ordercol - 1];
	if (hattno <= 0)
		return;

	foreach(lc, clauses)
	{
		Node	   *clause = (Node *) lfirst(lc);
		double		n = 0.0;
		Node	   *var = NULL;

		if (IsA(clause, RestrictInfo))
			clause = (Node *) ((RestrictInfo *) clause)->clause;
		if (list_member(except, clause))
			continue;			/* a filter on what the access returns */

		if (IsA(clause, OpExpr) && list_length(((OpExpr *) clause)->args) == 2)
		{
			OpExpr	   *op = (OpExpr *) clause;
			Node	   *l = strip_implicit_coercions(linitial(op->args));
			Node	   *r = strip_implicit_coercions(lsecond(op->args));

			if (!op_mergejoinable(op->opno, exprType(l)))
				continue;
			if (IsA(r, Const) || IsA(r, Param))
				var = l;
			else if (IsA(l, Const) || IsA(l, Param))
				var = r;
			n = 1.0;
		}
		else if (IsA(clause, ScalarArrayOpExpr) &&
				 ((ScalarArrayOpExpr *) clause)->useOr)
		{
			ScalarArrayOpExpr *sa = (ScalarArrayOpExpr *) clause;
			Node	   *l = strip_implicit_coercions(linitial(sa->args));
			Node	   *r = lsecond(sa->args);

			if (!op_mergejoinable(sa->opno, exprType(l)))
				continue;
			if (IsA(r, Const) && !((Const *) r)->constisnull)
			{
				ArrayType  *arr = DatumGetArrayTypeP(((Const *) r)->constvalue);

				n = (double) ArrayGetNItems(ARR_NDIM(arr), ARR_DIMS(arr));
			}
			else if (IsA(r, ArrayExpr))
				n = (double) list_length(((ArrayExpr *) r)->elements);
			var = l;
		}
		if (var == NULL || n < 1.0 || !IsA(var, Var) ||
			((Var *) var)->varno != (int) rel->relid ||
			((Var *) var)->varattno != hattno)
			continue;
		if (best == 0.0 || n < best)
			best = n;
	}
	sh->nvals = best;
}

double
lion_store_window_pages(const LionStoreShape *sh, double heapreads,
						double colpages, double members)
{
	double		nvals = sh->nvals;
	double		reads = heapreads;

	if (!sh->ordered)
		return heapreads;
	if (nvals >= 1.0 && members >= 1.0)
	{
		double		perslot = Max(colpages, 1.0) / Max(sh->rowsper, 1.0);
		double		pervalue = Max(members / nvals, 1.0);

		/* the pages m / nvals adjacent slots span, for each value */
		reads = Min(heapreads,
					nvals * (1.0 + (pervalue - 1.0) * perslot));
		reads = Max(reads, 1.0);
	}
	return reads + sh->permpages;
}
