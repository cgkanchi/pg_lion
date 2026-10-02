/*-------------------------------------------------------------------------
 *
 * lion_store.c
 *		The window store (DESIGN.md §40): which columns are stored, the
 *		format of the map and of a store page, and the build's emitter.
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
 *
 * The build writes its pages through the bulk writer, which logs them whole.
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

static inline BlockNumber *
lion_storemap_entries(Page page)
{
	return lion_storemap_page_entries(page);
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
