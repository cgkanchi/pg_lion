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

#include "access/htup_details.h"
#include "access/tupmacs.h"
#include "access/xact.h"
#include "catalog/pg_type.h"
#include "commands/vacuum.h"
#include "common/hashfn.h"
#include "miscadmin.h"
#include "nodes/nodeFuncs.h"
#include "port/pg_bitutils.h"
#include "storage/bufmgr.h"
#include "storage/indexfsm.h"
#include "storage/lmgr.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"
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
	if (h.lo > h.hi || h.hi >= LION_BLOCKS_PER_CONTAINER)
		return psprintf("covers heap pages %u to %u of its window",
						(unsigned) h.lo, (unsigned) h.hi);
	if ((int) maxoff != (int) LION_STORE_SUB_FIRST - 1 + (h.hi - h.lo + 1))
		return psprintf("has %u items for heap pages %u to %u",
						(unsigned) maxoff, (unsigned) h.lo, (unsigned) h.hi);
	if (h.typlen != col->typlen || h.flags != 0)
		return psprintf("has typlen %d and flags %u, expected typlen %d",
						(int) h.typlen, (unsigned) h.flags, (int) col->typlen);

	iid = PageGetItemId(page, LION_STORE_DICT_OFF);
	if (ItemIdGetLength(iid) < sizeof(LionStoreDict))
		return psprintf("has a dictionary of %u bytes", ItemIdGetLength(iid));
	memcpy(&d, PageGetItem(page, iid), sizeof(LionStoreDict));

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
	uint16		nslots[LION_BLOCKS_PER_CONTAINER];
	bool		absent[LION_BLOCKS_PER_CONTAINER];
	LionStoreVal *slots[LION_BLOCKS_PER_CONTAINER];	/* [nslots[k]] */
} LionStoreModel;

/* A page image the encoder made: its items, and what they take on a page. */
typedef struct LionStoreImage
{
	LionStoreHeader hdr;
	int			nitems;
	char	   *items[2 + LION_BLOCKS_PER_CONTAINER];
	Size		lens[2 + LION_BLOCKS_PER_CONTAINER];
	Size		need;
} LionStoreImage;

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
	uint16	   *codes[LION_BLOCKS_PER_CONTAINER];
	int			width;
	Size		dictlen;
	Size		need;
	int			k;
	int			n = 0;

	Assert(from >= m->lo && to <= m->hi && from <= to);

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
		dictlen = sizeof(LionStoreDict);
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
	img->hdr.flags = 0;
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
 * pages and root at the end.
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

	/* a chain being written: its last page, kept for its rightlink */
	BulkWriteBuffer pending;
	BlockNumber pendblk;

	/* the map */
	bool		leafused;
	uint64		leafno;			/* the leaf being filled */
	BlockNumber *leaf;			/* ... its entries */
	BlockNumber *leafblks;		/* the leaves written, by number */
	uint64		nleafblks;
	uint64		maxleafblks;

	int64		pages;			/* store and map pages written */
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
	sb->pending = NULL;
	sb->leaf = (BlockNumber *) MemoryContextAlloc(cxt, LION_STOREMAP_ITEM_SIZE);
	sb->maxleafblks = 16;
	sb->leafblks = (BlockNumber *)
		MemoryContextAlloc(cxt, sizeof(BlockNumber) * sb->maxleafblks);

	return sb;
}

/* Write a page image as the next page of the chain being written. */
static void
lion_store_build_write(LionStoreBuild *sb, const LionStoreImage *img,
					   BlockNumber *head)
{
	BlockNumber blk = (*sb->nblocks)++;
	BulkWriteBuffer buf = smgr_bulk_get_buf(sb->bulk);

	lion_store_place((Page) buf->data, img, InvalidBlockNumber);
	if (sb->pending != NULL)
	{
		LionPageGetOpaque((Page) sb->pending->data)->rightlink = blk;
		smgr_bulk_write(sb->bulk, sb->pendblk, sb->pending, true);
	}
	else
		*head = blk;
	sb->pending = buf;
	sb->pendblk = blk;
	sb->pages++;
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
		need = LION_STORE_ITEM_COST(sizeof(LionStoreDict));
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
lion_store_build_single(LionStoreBuild *sb, const LionStoreCol *col,
						LionStoreModel *m, int k, int mode, BlockNumber *head)
{
	LionStoreImage img;
	int			other = (mode == LION_STORE_DICT) ? LION_STORE_RAW : LION_STORE_DICT;

	if ((other == LION_STORE_DICT || col->rawwidth > 0) &&
		lion_store_encode(col, m, k, k, other, &sb->ds, &img))
	{
		lion_store_build_write(sb, &img, head);
		return;
	}
	m->absent[k] = true;
	if (!lion_store_encode(col, m, k, k, mode, &sb->ds, &img))
		elog(ERROR, "lion store: an ABSENT heap page does not fit a page");
	lion_store_build_write(sb, &img, head);
}

/* Write the open window's pages of one stored column. */
static void
lion_store_build_emit(LionStoreBuild *sb, int ord)
{
	const LionStoreCol *col = &sb->ix->stored[ord];
	LionStoreModel *m = &sb->models[ord];
	MemoryContext old = MemoryContextSwitchTo(sb->tmpcxt);
	BlockNumber head = InvalidBlockNumber;
	LionStoreImage img;
	int			mode;
	int			start = 0;
	int			k = 0;

	m->ckey = sb->ckey;
	m->ord = (uint16) ord;
	m->lo = 0;
	m->hi = sb->maxpage;
	mode = lion_store_choose_mode(col, m, 0, sb->maxpage, &sb->ds);

	lion_store_dset_reset(&sb->ds);
	while (k <= sb->maxpage)
	{
		if (mode == LION_STORE_DICT)
			lion_store_dset_add_page(&sb->ds, m, k);
		if (lion_store_group_need(col, m, start, k, mode, &sb->ds) <=
			LION_PAGE_CAPACITY)
		{
			k++;
			continue;
		}
		if (k > start)
		{
			/* close the page before k, and try k again on a page of its own */
			if (!lion_store_encode(col, m, start, k - 1, mode, &sb->ds, &img))
				elog(ERROR, "lion store: a build page does not fit");
			lion_store_build_write(sb, &img, &head);
			start = k;
			lion_store_dset_reset(&sb->ds);
			continue;
		}
		lion_store_build_single(sb, col, m, k, mode, &head);
		k++;
		start = k;
		lion_store_dset_reset(&sb->ds);
	}
	if (start <= sb->maxpage)
	{
		if (!lion_store_encode(col, m, start, sb->maxpage, mode, &sb->ds, &img))
			elog(ERROR, "lion store: a build page does not fit");
		lion_store_build_write(sb, &img, &head);
	}

	/* the chain's last page has no right link */
	if (sb->pending != NULL)
	{
		smgr_bulk_write(sb->bulk, sb->pendblk, sb->pending, true);
		sb->pending = NULL;
	}
	Assert(BlockNumberIsValid(head));
	lion_store_build_map_set(sb, lion_storemap_slot(sb->ckey, sb->ix->nstored, ord),
							 head);

	MemoryContextSwitchTo(old);
	MemoryContextReset(sb->tmpcxt);
}

static void
lion_store_build_close_window(LionStoreBuild *sb)
{
	int			ord;

	if (!sb->open)
		return;
	for (ord = 0; ord < sb->ix->nstored; ord++)
		lion_store_build_emit(sb, ord);
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
					   OffsetNumber off, const LionStoreVal *v, uint64 slot)
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
		++(*steps) >= LION_BLOCKS_PER_CONTAINER)
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

/* Write v as column col's value of (ckey, k, off). */
static void
lion_store_write(Relation index, Relation heaprel, LionIndexState *ix,
				 const LionStoreCol *col, uint32 ckey, int k, OffsetNumber off,
				 const LionStoreVal *v)
{
	uint64		slot = lion_storemap_slot(ckey, ix->nstored, col->ord);
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
									   v, slot))
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
	for (ord = 0; ord < ix->nstored; ord++)
		lion_store_write(index, heaprel, ix, &ix->stored[ord], ckey, k, off,
						 &vals[ord]);
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
 * Clear the dead slots of the store page in buf (EXCLUSIVE, checked), in
 * place or by a rewrite.
 */
static void
lion_store_vacuum_page(Relation index, const LionStoreCol *col, Buffer buf,
					   IndexBulkDeleteCallback callback, void *callback_state,
					   LionStoreVacStats *st)
{
	Page		page = BufferGetPage(buf);
	LionStoreHeader h;
	BlockNumber first;
	bool	   *dead[LION_BLOCKS_PER_CONTAINER];
	int64		written = 0;
	int64		ndead = 0;
	int			k;

	memcpy(&h, lion_store_page_header(page), sizeof(LionStoreHeader));
	first = lion_ckey_first_block(h.ckey);

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
			ItemPointerData tid;
			bool		isnull;

			if (h.mode == LION_STORE_DICT)
				isnull = (lion_store_code_get(body, h.width, i) == 0);
			else
				isnull = lion_store_null_get(body + (Size) s->nslots * h.width, i);
			if (isnull)
				continue;		/* NULL, never written, or cleared */
			written++;
			ItemPointerSet(&tid, first + k, (OffsetNumber) (i + 1));
			if (callback(&tid, callback_state))
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
		if (h.mode == LION_STORE_RAW || col->rawwidth > 0)
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
	IndexBulkDeleteCallback callback;
	void	   *callback_state;
	bool		write;
	LionStoreVisit visit;
	void	   *visitarg;
	LionStoreVacStats *st;
	MemoryContext pagecxt;
} LionStoreVacArg;

/* One window's chain for one column: visit each page, clear its dead slots. */
static void
lion_store_vacuum_chain(void *arg, uint64 slot, BlockNumber head)
{
	LionStoreVacArg *va = (LionStoreVacArg *) arg;
	uint32		ckey = (uint32) (slot / (uint64) va->ix->nstored);
	int			ord = (int) (slot % (uint64) va->ix->nstored);
	const LionStoreCol *col = &va->ix->stored[ord];
	BlockNumber blk = head;
	int			steps = 0;

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
			lion_store_vacuum_page(va->index, col, buf, va->callback,
								   va->callback_state, va->st);
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
	va.callback = callback;
	va.callback_state = callback_state;
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
					   lion_storemap_slot(firstwin, ix->nstored, 0),
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
		uint32		ckey = (uint32) (slot / (uint64) ix->nstored);
		uint16		ord = (uint16) (slot % (uint64) ix->nstored);
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
		uint32		ckey = (uint32) (d.slots[i] / (uint64) ix->nstored);
		uint16		ord = (uint16) (d.slots[i] % (uint64) ix->nstored);

		if (!BlockNumberIsValid(d.heads[i]))
			continue;
		RecordFreeIndexPage(index, d.heads[i]);
		if (BlockNumberIsValid(nexts[i]))
			freed += lion_store_free_chain(index, ckey, ord, nexts[i], 1);
		CHECK_FOR_INTERRUPTS();
	}

	return freed;
}

bool
lion_store_page_linked(Relation index, LionIndexState *ix, BlockNumber blk,
					   uint32 ckey, uint16 ord)
{
	uint64		slot;
	BlockNumber head;
	BlockNumber cur;
	int			steps = 0;

	if ((int) ord >= ix->nstored)
		return false;			/* not a column this index stores */
	slot = lion_storemap_slot(ckey, ix->nstored, ord);
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
			if (next == cur || ++steps >= LION_BLOCKS_PER_CONTAINER)
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
};

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
	return r;
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

/* The head of the reader's window, from the cache or the map. */
static BlockNumber
lion_store_reader_head(LionStoreReader *r, uint64 slot, bool fresh)
{
	if (!fresh && r->cache.slot == slot && BlockNumberIsValid(r->cache.head))
		return r->cache.head;
	r->cache.slot = slot;
	r->cache.head = lion_storemap_head(r->index, r->ix->store.store_root, slot,
									   &r->cache);
	return r->cache.head;
}

/* Member values of heap page k, from its checked sub-array s on page. */
static void
lion_store_gather_page(LionStoreReader *r, Page page, BlockNumber blk,
					   const LionStoreHeader *h, const LionStoreSub *s,
					   const uint16 *lo, int from, int to, Datum *values,
					   bool *isnull, uint64 *absent_pages)
{
	const LionStoreCol *col = r->col;
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
			*absent_pages |= ((uint64) 1) << k;
			r->missing |= ((uint64) 1) << k;
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

void
lion_store_gather(LionStoreReader *r, uint32 ckey, const uint16 *lo, int nlo,
				  Datum *values, bool *isnull, uint64 *absent_pages)
{
	const LionStoreCol *col = r->col;
	uint64		slot = lion_storemap_slot(ckey, r->ix->nstored, col->ord);
	BlockNumber head;
	BlockNumber blk = InvalidBlockNumber;
	Buffer		buf = InvalidBuffer;
	int			restarts = 0;
	int			steps = 0;
	int			i;

	*absent_pages = 0;
	r->missing = 0;
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
		return;

	head = lion_store_reader_head(r, slot, false);

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
			*absent_pages |= ((uint64) 1) << k;
			r->missing |= ((uint64) 1) << k;
			i = end;
			continue;
		}
		if (!BlockNumberIsValid(blk))
			blk = head;

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
			head = lion_store_reader_head(r, slot, true);
			blk = InvalidBlockNumber;
			steps = 0;
			continue;
		}

		if (k > h.hi)
		{
			next = LionPageGetOpaque(page)->rightlink;
			LockBuffer(buf, BUFFER_LOCK_UNLOCK);
			if (!BlockNumberIsValid(next))
			{
				/* past every range: no row of heap page k is stored yet */
				*absent_pages |= ((uint64) 1) << k;
				r->missing |= ((uint64) 1) << k;
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
			*absent_pages |= ((uint64) 1) << k;
		else
			lion_store_gather_page(r, page, blk, &h, s, lo, i, end, values,
								   isnull, absent_pages);
		LockBuffer(buf, BUFFER_LOCK_UNLOCK);
		i = end;
	}

	if (BufferIsValid(buf))
		ReleaseBuffer(buf);
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
