/*-------------------------------------------------------------------------
 *
 * rbi_pages.c
 *		Storage primitives for the roaring index access method: meta page,
 *		bucket pages and entry tuples, container chains and their splits,
 *		page allocation.  See DESIGN.md sections 4 and 5.
 *
 * Every page modification in this file goes through GenericXLog: the buffer
 * is registered before it is touched and GenericXLogFinish() runs before any
 * lock is dropped.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/generic_xlog.h"
#include "access/htup_details.h"
#include "catalog/pg_type.h"
#include "miscadmin.h"
#include "storage/bufmgr.h"
#include "storage/bufpage.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "varatt.h"

#include "rbi.h"

static void rbi_split_and_place(Relation index, Buffer buf, OffsetNumber off,
								bool replace, Buffer entrybuf,
								OffsetNumber entryoff, RBIEntryTuple *entry,
								RBIContainer **items, int nitems);

/*
 * Clamp a requested bucket count into [1, RBI_MAX_BUCKETS].
 *
 * Bucket counts are not rounded to a power of two: a hash is mapped to its
 * bucket with a modulo (rbi_bucket_of()), so any count works, and ambuild
 * picks one from the bytes the entries need rather than from a key count
 * (DESIGN.md §5).  The `buckets` reloption therefore means exactly what it
 * says.
 */
uint32
rbi_clamp_buckets(int64 nbuckets)
{
	if (nbuckets <= 1)
		return 1;
	if (nbuckets >= RBI_MAX_BUCKETS)
		return RBI_MAX_BUCKETS;
	return (uint32) nbuckets;
}

/*
 * Initialise a page of the roaring index.  Sets up the special area.
 */
void
rbi_init_page(Page page, uint16 flags)
{
	RBIPageOpaque opaque;

	PageInit(page, BLCKSZ, RBI_SPECIAL_SIZE);

	opaque = RBIPageGetOpaque(page);
	opaque->rightlink = InvalidBlockNumber;
	opaque->minckey = 0;
	opaque->maxckey = 0;
	opaque->flags = flags;
	opaque->page_id = RBI_PAGE_ID;
}

/*
 * Fill in a meta page image.
 */
void
rbi_init_metapage(Page page, uint32 nbuckets, uint32 inline_limit)
{
	RBIMetaPageData *meta;

	rbi_init_page(page, RBI_PAGE_META);

	meta = RBIPageGetMeta(page);
	memset(meta, 0, sizeof(RBIMetaPageData));
	meta->magic = RBI_MAGIC;
	meta->version = RBI_VERSION;
	meta->offset_bits = RBI_OFFSET_BITS;
	meta->container_bits = RBI_CONTAINER_BITS;
	meta->nbuckets = nbuckets;
	meta->inline_limit = inline_limit;

	((PageHeader) page)->pd_lower += sizeof(RBIMetaPageData);
	Assert(((PageHeader) page)->pd_lower <= ((PageHeader) page)->pd_upper);
}

/*
 * Extend the index by one block and register the new buffer in the caller's
 * GenericXLog record, which logs the page initialisation as a full image.
 * The buffer comes back pinned and exclusively locked; *pagep (if not NULL)
 * receives the registered page image the caller has to work on.
 *
 * Allocating the page inside the record that links it into a chain is what
 * keeps a crash from leaving an initialised page that nothing points at.
 */
Buffer
rbi_new_buffer_xl(Relation index, GenericXLogState *xstate, uint16 flags,
				  Page *pagep)
{
	Buffer		buffer;
	Page		page;

	buffer = ExtendBufferedRel(BMR_REL(index), MAIN_FORKNUM, NULL,
							   EB_LOCK_FIRST);

	page = GenericXLogRegisterBuffer(xstate, buffer, GENERIC_XLOG_FULL_IMAGE);
	rbi_init_page(page, flags);

	if (pagep != NULL)
		*pagep = page;

	return buffer;
}

/*
 * Extend the index by one block and return the new buffer, pinned and
 * exclusively locked, with the page already initialised.  The initialisation
 * is WAL-logged on its own so that a later GenericXLog delta against this
 * page replays correctly.
 */
Buffer
rbi_new_buffer(Relation index, uint16 flags)
{
	Buffer		buffer;
	GenericXLogState *xstate;

	xstate = GenericXLogStart(index);
	buffer = rbi_new_buffer_xl(index, xstate, flags, NULL);
	GenericXLogFinish(xstate);

	return buffer;
}

/*
 * Read and validate the meta page.
 */
void
rbi_read_meta(Relation index, RBIMetaPageData *meta)
{
	Buffer		buf;
	Page		page;
	RBIMetaPageData *ondisk;

	buf = ReadBuffer(index, RBI_METAPAGE_BLKNO);
	LockBuffer(buf, BUFFER_LOCK_SHARE);
	page = BufferGetPage(buf);

	if (PageIsNew(page) || !RBIPageIsMeta(page) ||
		RBIPageGetOpaque(page)->page_id != RBI_PAGE_ID)
	{
		UnlockReleaseBuffer(buf);
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg("index \"%s\" is not a valid roaring index",
						RelationGetRelationName(index))));
	}

	ondisk = RBIPageGetMeta(page);
	*meta = *ondisk;
	UnlockReleaseBuffer(buf);

	if (meta->magic != RBI_MAGIC || meta->version != RBI_VERSION)
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg("index \"%s\" is not a valid roaring index",
						RelationGetRelationName(index)),
				 errdetail("Meta page magic %08X version %u, expected %08X version %u.",
						   meta->magic, meta->version, RBI_MAGIC, RBI_VERSION)));

	if (meta->offset_bits != RBI_OFFSET_BITS ||
		meta->container_bits != RBI_CONTAINER_BITS)
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg("index \"%s\" was built for a different block size",
						RelationGetRelationName(index)),
				 errdetail("Index has offset_bits %u, container_bits %u; this build uses %d and %d.",
						   meta->offset_bits, meta->container_bits,
						   RBI_OFFSET_BITS, RBI_CONTAINER_BITS)));

	if (meta->nbuckets == 0 || meta->nbuckets > RBI_MAX_BUCKETS)
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg("index \"%s\" has an invalid bucket count %u",
						RelationGetRelationName(index), meta->nbuckets)));
}

/*
 * Fill in *state for index.  The meta page image is supplied by the caller
 * because ambuild needs a state before the meta page exists.
 */
void
rbi_fill_state(Relation index, RBIState *state, const RBIMetaPageData *meta,
			   MemoryContext cxt)
{
	Form_pg_attribute att;
	Oid			eqopr;
	Oid			eqfunc;

	memset(state, 0, sizeof(RBIState));
	state->meta = *meta;

	if (IndexRelationGetNumberOfKeyAttributes(index) != 1)
		elog(ERROR, "roaring index \"%s\" must have exactly one key column",
			 RelationGetRelationName(index));

	att = TupleDescAttr(RelationGetDescr(index), 0);
	state->typid = att->atttypid;
	get_typlenbyvalalign(state->typid, &state->typlen, &state->typbyval,
						 &state->typalign);
	state->collation = index->rd_indcollation[0];

	fmgr_info_copy(&state->hashproc, index_getprocinfo(index, 1, 1), cxt);

	eqopr = get_opfamily_member(index->rd_opfamily[0],
								index->rd_opcintype[0],
								index->rd_opcintype[0],
								1);
	if (!OidIsValid(eqopr))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("operator class of index \"%s\" has no equality operator",
						RelationGetRelationName(index))));
	eqfunc = get_opcode(eqopr);
	if (!OidIsValid(eqfunc))
		elog(ERROR, "could not find function for operator %u", eqopr);
	fmgr_info_cxt(eqfunc, &state->eqproc, cxt);
}

/*
 * Get the cached per-relation state, building it on first use.
 */
RBIState *
rbi_get_state(Relation index)
{
	RBIState   *state;
	RBIMetaPageData meta;

	if (index->rd_amcache != NULL)
		return (RBIState *) index->rd_amcache;

	rbi_read_meta(index, &meta);

	state = (RBIState *) MemoryContextAlloc(index->rd_indexcxt,
											sizeof(RBIState));
	rbi_fill_state(index, state, &meta, index->rd_indexcxt);

	index->rd_amcache = (void *) state;
	return state;
}

/*
 * Heap TIDs whose offset does not fit in RBI_OFFSET_BITS cannot be encoded.
 */
void
rbi_check_key_offset(ItemPointer tid)
{
	if (ItemPointerGetOffsetNumber(tid) > RBI_MAX_OFFSET)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("roaring index: table access method is not supported"),
				 errdetail("Item pointer offset %u exceeds the maximum of %u supported by this index.",
						   ItemPointerGetOffsetNumber(tid),
						   (unsigned) RBI_MAX_OFFSET)));
}

/* ---------------------------------------------------------------------
 * Key handling
 * --------------------------------------------------------------------- */

uint32
rbi_hash_key(RBIState *state, Datum key)
{
	return DatumGetUInt32(FunctionCall1Coll(&state->hashproc,
											state->collation, key));
}

/*
 * Number of bytes needed to store key, with datumCopy semantics
 * (DESIGN.md section 4).
 */
Size
rbi_key_datum_size(RBIState *state, Datum key)
{
	Size		size;

	if (state->typbyval)
		size = sizeof(Datum);
	else if (state->typlen > 0)
		size = (Size) state->typlen;
	else if (state->typlen == -1)
	{
		struct varlena *v = PG_DETOAST_DATUM(key);

		size = VARSIZE(v);
		if ((Pointer) v != DatumGetPointer(key))
			pfree(v);
	}
	else
	{
		Assert(state->typlen == -2);
		size = strlen(DatumGetCString(key)) + 1;
	}

	if (size > RBI_MAX_KEY_SIZE)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("roaring index key size %zu exceeds maximum %d",
						size, RBI_MAX_KEY_SIZE)));

	return size;
}

/*
 * Store key at dest, which must have rbi_key_datum_size() bytes.
 */
void
rbi_store_key(RBIState *state, Datum key, char *dest)
{
	if (state->typbyval)
		memcpy(dest, &key, sizeof(Datum));
	else if (state->typlen > 0)
		memcpy(dest, DatumGetPointer(key), (Size) state->typlen);
	else if (state->typlen == -1)
	{
		struct varlena *v = PG_DETOAST_DATUM(key);

		memcpy(dest, v, VARSIZE(v));
		if ((Pointer) v != DatumGetPointer(key))
			pfree(v);
	}
	else
	{
		char	   *s = DatumGetCString(key);

		memcpy(dest, s, strlen(s) + 1);
	}
}

/*
 * Reconstruct a key Datum from stored bytes.  For by-reference types the
 * result points into src, so it is only valid while the caller keeps the
 * page pinned.
 */
Datum
rbi_fetch_key(RBIState *state, const char *src)
{
	if (state->typbyval)
	{
		Datum		d;

		memcpy(&d, src, sizeof(Datum));
		return d;
	}

	return PointerGetDatum(src);
}

bool
rbi_keys_equal(RBIState *state, Datum a, Datum b)
{
	return DatumGetBool(FunctionCall2Coll(&state->eqproc, state->collation,
										  a, b));
}

/* ---------------------------------------------------------------------
 * Entry tuples
 * --------------------------------------------------------------------- */

/*
 * Fetch the item (container or sparse segment) at *off of an INLINE payload
 * into the aligned buffer buf and advance *off past it.  Returns the item
 * size, or 0 when the payload has been consumed.
 */
Size
rbi_inline_fetch(const char *payload, Size paylen, Size *off, RBIContainer *buf)
{
	Size		avail;
	Size		peek;
	Size		csize;

	Assert(*off <= paylen);
	avail = paylen - *off;
	if (avail < RBI_CONTAINER_HDRSZ)
	{
		if (avail != 0)
			elog(ERROR, "roaring index: malformed inline posting set");
		return 0;
	}

	/*
	 * The item may be unaligned, so everything is read through the caller's
	 * buffer.  rbi_container_size() of a RUN container needs the nruns field,
	 * which is the first uint16 of the payload, so peek that far before asking
	 * for the size.  Every other item kind, sparse segments included, is sized
	 * from the header alone.
	 */
	peek = Min(avail, RBI_CONTAINER_HDRSZ + sizeof(uint16));
	memcpy(buf, payload + *off, peek);
	if (buf->type != RBI_CT_ARRAY && buf->type != RBI_CT_BITSET &&
		buf->type != RBI_CT_RUN && buf->type != RBI_CT_SPARSE)
		elog(ERROR, "roaring index: malformed inline item of type %u",
			 buf->type);
	csize = rbi_item_size(buf);
	if (csize < RBI_CONTAINER_HDRSZ || csize > RBI_CONTAINER_MAX_SIZE ||
		csize > avail)
		elog(ERROR, "roaring index: malformed inline item");

	memcpy(buf, payload + *off, csize);
	*off += csize;

	return csize;
}

/*
 * Build an entry tuple in palloc'd memory.
 *
 * The layout is: header, key bytes, MAXALIGN padding, payload.  For INLINE
 * entries the payload is a sequence of containers in ascending ckey order,
 * packed back to back with no padding, so a container inside it is generally
 * unaligned; read them with rbi_inline_fetch().  head/tail/ncontainers/ntids
 * are left at their empty values; the caller fills them in.
 */
RBIEntryTuple *
rbi_make_entry(RBIState *state, Datum key, uint32 hash, uint16 flags,
			   const char *payload, Size payloadlen, Size *size)
{
	RBIEntryTuple *entry;
	Size		keylen = rbi_key_datum_size(state, key);
	Size		payoff = MAXALIGN((RBI_ENTRY_HDRSZ) + keylen);
	Size		total = payoff + payloadlen;

	if (total > RBI_MAX_ITEM_SIZE)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("roaring index entry of %zu bytes is too large for a page",
						total)));

	entry = (RBIEntryTuple *) palloc0(total);
	entry->hash = hash;
	entry->flags = flags;
	entry->keylen = (uint16) keylen;
	entry->head = InvalidBlockNumber;
	entry->tail = InvalidBlockNumber;
	entry->ncontainers = 0;
	entry->ntids = 0;

	rbi_store_key(state, key, RBIEntryGetKey(entry));

	if (payloadlen > 0)
	{
		Assert(payload != NULL);
		memcpy(((char *) entry) + payoff, payload, payloadlen);
	}

	*size = total;
	return entry;
}

/*
 * Build the reserved NULL-key entry tuple (DESIGN.md §14).
 *
 * It has no key at all: keylen 0, hash 0, and the RBI_ENTRY_NULLKEY flag,
 * which is how every reader recognises it.  Its payload is an ordinary
 * INLINE payload, so once it exists the insert, VACUUM and count paths treat
 * it exactly like any other entry.
 */
RBIEntryTuple *
rbi_make_null_entry(uint16 flags, const char *payload, Size payloadlen,
					Size *size)
{
	RBIEntryTuple *entry;
	Size		payoff = MAXALIGN(RBI_ENTRY_HDRSZ);
	Size		total = payoff + payloadlen;

	if (total > RBI_MAX_ITEM_SIZE)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("roaring index entry of %zu bytes is too large for a page",
						total)));

	entry = (RBIEntryTuple *) palloc0(total);
	entry->hash = RBI_NULLKEY_HASH;
	entry->flags = flags | RBI_ENTRY_NULLKEY;
	entry->keylen = 0;
	entry->head = InvalidBlockNumber;
	entry->tail = InvalidBlockNumber;
	entry->ncontainers = 0;
	entry->ntids = 0;

	if (payloadlen > 0)
	{
		Assert(payload != NULL);
		memcpy(((char *) entry) + payoff, payload, payloadlen);
	}

	*size = total;
	return entry;
}

/*
 * Private copy of an entry tuple with a new payload.
 *
 * The header, the key bytes and their alignment padding are taken from entry
 * (which usually points into a page); payload replaces whatever payload the
 * original had.  A zero-length payload produces the shape of a CHAIN entry.
 * The caller owns the result and fills in flags, head, tail, ncontainers and
 * ntids as needed.
 */
RBIEntryTuple *
rbi_entry_rebuild(const RBIEntryTuple *entry, const char *payload,
				  Size payloadlen, Size *size)
{
	Size		payoff = RBIEntryPayloadOffset(entry);
	Size		total = payoff + payloadlen;
	RBIEntryTuple *copy;

	if (total > RBI_MAX_ITEM_SIZE)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("roaring index entry of %zu bytes is too large for a page",
						total)));

	copy = (RBIEntryTuple *) palloc(total);
	memcpy(copy, entry, payoff);
	if (payloadlen > 0)
	{
		Assert(payload != NULL);
		memcpy(((char *) copy) + payoff, payload, payloadlen);
	}

	*size = total;
	return copy;
}

/*
 * Find the entry for (hash, key) in the bucket chain starting at headbuf,
 * comparing keys with eqproc (NULL means state->eqproc/state->collation).
 *
 * The caller holds headbuf locked in lockmode and keeps it locked.  On
 * success *buf is left locked in the same mode; it is headbuf itself when the
 * entry lives on the bucket head page, and an extra pinned buffer otherwise.
 */
bool
rbi_find_entry_ext(Relation index, RBIState *state, Buffer headbuf, int lockmode,
				   Datum key, uint32 hash, FmgrInfo *eqproc, Oid collation,
				   Buffer *buf, OffsetNumber *offnum)
{
	Buffer		cur = headbuf;

	if (eqproc == NULL)
	{
		eqproc = &state->eqproc;
		collation = state->collation;
	}

	for (;;)
	{
		Page		page = BufferGetPage(cur);
		OffsetNumber maxoff = PageGetMaxOffsetNumber(page);
		OffsetNumber off;
		BlockNumber next;

		Assert(RBIPageIsBucket(page));

		for (off = FirstOffsetNumber; off <= maxoff; off++)
		{
			ItemId		iid = PageGetItemId(page, off);
			RBIEntryTuple *entry;
			Datum		stored;

			if (!ItemIdIsUsed(iid))
				continue;
			entry = (RBIEntryTuple *) PageGetItem(page, iid);
			if (entry->hash != hash)
				continue;

			/*
			 * The NULL-key entry has no key to compare and hashes to 0, which
			 * a real key may hash to as well: it is only ever found through
			 * rbi_find_null_entry() (DESIGN.md §14).
			 */
			if (RBIEntryIsNullKey(entry))
				continue;

			stored = rbi_fetch_key(state, RBIEntryGetKey(entry));
			if (DatumGetBool(FunctionCall2Coll(eqproc, collation, stored, key)))
			{
				*buf = cur;
				*offnum = off;
				return true;
			}
		}

		next = RBIPageGetOpaque(page)->rightlink;
		if (!BlockNumberIsValid(next))
			break;

		{
			Buffer		nbuf = ReadBuffer(index, next);

			LockBuffer(nbuf, lockmode);
			if (cur != headbuf)
				UnlockReleaseBuffer(cur);
			cur = nbuf;
		}
		CHECK_FOR_INTERRUPTS();
	}

	if (cur != headbuf)
		UnlockReleaseBuffer(cur);

	*buf = InvalidBuffer;
	*offnum = InvalidOffsetNumber;
	return false;
}

bool
rbi_find_entry(Relation index, RBIState *state, Buffer headbuf, int lockmode,
			   Datum key, uint32 hash, Buffer *buf, OffsetNumber *offnum)
{
	return rbi_find_entry_ext(index, state, headbuf, lockmode, key, hash,
							  NULL, InvalidOid, buf, offnum);
}

/*
 * Find the reserved NULL-key entry (DESIGN.md §14).  It lives in bucket 0 and
 * is recognised by its flag; headbuf must be the head page of bucket 0, held
 * in lockmode by the caller, and is left locked.
 */
bool
rbi_find_null_entry(Relation index, Buffer headbuf, int lockmode,
					Buffer *buf, OffsetNumber *offnum)
{
	Buffer		cur = headbuf;

	for (;;)
	{
		Page		page = BufferGetPage(cur);
		OffsetNumber maxoff = PageGetMaxOffsetNumber(page);
		OffsetNumber off;
		BlockNumber next;

		Assert(RBIPageIsBucket(page));

		for (off = FirstOffsetNumber; off <= maxoff; off++)
		{
			ItemId		iid = PageGetItemId(page, off);
			RBIEntryTuple *entry;

			if (!ItemIdIsUsed(iid))
				continue;
			entry = (RBIEntryTuple *) PageGetItem(page, iid);
			if (!RBIEntryIsNullKey(entry))
				continue;

			*buf = cur;
			*offnum = off;
			return true;
		}

		next = RBIPageGetOpaque(page)->rightlink;
		if (!BlockNumberIsValid(next))
			break;

		{
			Buffer		nbuf = ReadBuffer(index, next);

			LockBuffer(nbuf, lockmode);
			if (cur != headbuf)
				UnlockReleaseBuffer(cur);
			cur = nbuf;
		}
		CHECK_FOR_INTERRUPTS();
	}

	if (cur != headbuf)
		UnlockReleaseBuffer(cur);

	*buf = InvalidBuffer;
	*offnum = InvalidOffsetNumber;
	return false;
}

/*
 * Append a new entry tuple to the bucket chain starting at headbuf (held
 * EXCLUSIVE by the caller).  Adds a bucket page if no existing page has room.
 */
void
rbi_add_entry(Relation index, Buffer headbuf, RBIEntryTuple *entry, Size size)
{
	Buffer		cur = headbuf;
	Size		need = MAXALIGN(size);
	GenericXLogState *xstate;
	Page		p;
	Buffer		nbuf;

	for (;;)
	{
		Page		page = BufferGetPage(cur);
		BlockNumber next;

		Assert(RBIPageIsBucket(page));

		if (PageGetFreeSpace(page) >= need)
		{
			xstate = GenericXLogStart(index);
			p = GenericXLogRegisterBuffer(xstate, cur, 0);
			if (PageAddItemExtended(p, entry, size,
									InvalidOffsetNumber, 0) == InvalidOffsetNumber)
				elog(ERROR, "roaring index: failed to add entry to bucket page");
			GenericXLogFinish(xstate);
			if (cur != headbuf)
				UnlockReleaseBuffer(cur);
			return;
		}

		next = RBIPageGetOpaque(page)->rightlink;
		if (!BlockNumberIsValid(next))
			break;

		{
			Buffer		nextbuf = ReadBuffer(index, next);

			LockBuffer(nextbuf, BUFFER_LOCK_EXCLUSIVE);
			if (cur != headbuf)
				UnlockReleaseBuffer(cur);
			cur = nextbuf;
		}
		CHECK_FOR_INTERRUPTS();
	}

	/*
	 * No room anywhere: append a new bucket page after cur.  The page is
	 * allocated inside the record that links it in, so that a crash cannot
	 * leave an initialised page nothing points at.
	 */
	xstate = GenericXLogStart(index);
	p = GenericXLogRegisterBuffer(xstate, cur, 0);
	{
		Page		np;

		nbuf = rbi_new_buffer_xl(index, xstate, RBI_PAGE_BUCKET, &np);

		if (PageAddItemExtended(np, entry, size,
								InvalidOffsetNumber, 0) == InvalidOffsetNumber)
			elog(ERROR, "roaring index: failed to add entry to new bucket page");
		RBIPageGetOpaque(p)->rightlink = BufferGetBlockNumber(nbuf);
	}
	GenericXLogFinish(xstate);

	UnlockReleaseBuffer(nbuf);
	if (cur != headbuf)
		UnlockReleaseBuffer(cur);
}

/*
 * Replace the entry at (buf, offnum).  Returns false and changes nothing if
 * the new version does not fit.
 */
bool
rbi_replace_entry(Relation index, GenericXLogState *state, Buffer buf,
				  OffsetNumber offnum, RBIEntryTuple *entry, Size size)
{
	GenericXLogState *xstate = state;
	Page		page;
	bool		ok;

	if (xstate == NULL)
		xstate = GenericXLogStart(index);

	page = GenericXLogRegisterBuffer(xstate, buf, 0);
	ok = PageIndexTupleOverwrite(page, offnum, entry, size);

	if (state == NULL)
	{
		if (ok)
			GenericXLogFinish(xstate);
		else
			GenericXLogAbort(xstate);
	}

	return ok;
}

/* ---------------------------------------------------------------------
 * Container chains
 * --------------------------------------------------------------------- */

/*
 * Recompute minckey/maxckey of a container page from its items.
 *
 * The bounds are the first ckey of the first item and the LAST ckey of the
 * last item: a sparse segment covers a range, and the whole point of
 * minckey/maxckey is to say which ckeys this page owns (DESIGN.md §13).
 */
void
rbi_page_update_minmax(Page page)
{
	RBIPageOpaque opaque = RBIPageGetOpaque(page);
	OffsetNumber maxoff = PageGetMaxOffsetNumber(page);

	if (maxoff < FirstOffsetNumber)
	{
		opaque->minckey = 0;
		opaque->maxckey = 0;
		return;
	}

	opaque->minckey = rbi_item_first_ckey((RBIContainer *)
										  PageGetItem(page,
													  PageGetItemId(page, FirstOffsetNumber)));
	opaque->maxckey = rbi_item_last_ckey((RBIContainer *)
										 PageGetItem(page,
													 PageGetItemId(page, maxoff)));
}

/*
 * Find the container page that owns ckey (DESIGN.md section 4).  No locks are
 * held on return; the caller must be holding the bucket head page so that the
 * chain cannot change under it.
 */
BlockNumber
rbi_chain_find_page(Relation index, BlockNumber head, BlockNumber tail, uint32 ckey)
{
	BlockNumber blk;
	Buffer		buf;
	Page		page;
	uint32		minckey;
	OffsetNumber tailitems;

	Assert(BlockNumberIsValid(head) && BlockNumberIsValid(tail));

	/*
	 * Append fast path: does the tail own this ckey?  An empty tail (VACUUM
	 * may have deleted every container on it) owns nothing: its minckey is 0,
	 * which would swallow every ckey and break the ordering across pages, so
	 * fall through to the walk in that case.
	 */
	buf = ReadBuffer(index, tail);
	LockBuffer(buf, BUFFER_LOCK_SHARE);
	page = BufferGetPage(buf);
	Assert(RBIPageIsContainer(page));
	minckey = RBIPageGetOpaque(page)->minckey;
	tailitems = PageGetMaxOffsetNumber(page);
	UnlockReleaseBuffer(buf);

	if (tailitems > 0 && ckey >= minckey)
		return tail;

	/*
	 * Otherwise walk from the head and stop at the first non-empty page whose
	 * maxckey reaches ckey, or at the last page.  Empty pages (left behind by
	 * VACUUM) are skipped: only a page that already holds a larger ckey can be
	 * proven to own this one.
	 */
	blk = head;
	for (;;)
	{
		BlockNumber next;
		uint32		maxckey;
		OffsetNumber nitems;

		buf = ReadBuffer(index, blk);
		LockBuffer(buf, BUFFER_LOCK_SHARE);
		page = BufferGetPage(buf);
		Assert(RBIPageIsContainer(page));
		maxckey = RBIPageGetOpaque(page)->maxckey;
		next = RBIPageGetOpaque(page)->rightlink;
		nitems = PageGetMaxOffsetNumber(page);
		UnlockReleaseBuffer(buf);

		if (nitems > 0 && maxckey >= ckey)
			return blk;
		if (!BlockNumberIsValid(next))
			return blk;
		blk = next;
		CHECK_FOR_INTERRUPTS();
	}
}

/*
 * Binary search a container page for the item that covers ckey: a container
 * whose ckey it is, or a sparse segment whose range it falls in.
 *
 * With *found = false the result is the offset an item for ckey would have to
 * take: the first item whose first ckey is above ckey (one past the last item
 * when there is none).  Items are ordered by first ckey and their ranges
 * never overlap, so "the last item whose first ckey is <= ckey" is the only
 * item that can possibly cover it.
 */
OffsetNumber
rbi_page_find_item(Page page, uint32 ckey, bool *found)
{
	OffsetNumber low = FirstOffsetNumber;
	OffsetNumber high = PageGetMaxOffsetNumber(page);
	OffsetNumber cand = InvalidOffsetNumber;

	*found = false;

	while (low <= high)
	{
		OffsetNumber mid = low + (high - low) / 2;
		RBIContainer *c = (RBIContainer *) PageGetItem(page, PageGetItemId(page, mid));

		if (rbi_item_first_ckey(c) <= ckey)
		{
			cand = mid;
			low = mid + 1;
		}
		else
		{
			if (mid == FirstOffsetNumber)
				break;
			high = mid - 1;
		}
	}

	if (cand == InvalidOffsetNumber)
		return FirstOffsetNumber;	/* ckey belongs before every item */

	{
		RBIContainer *c = (RBIContainer *) PageGetItem(page, PageGetItemId(page, cand));

		if (rbi_item_last_ckey(c) >= ckey)
		{
			*found = true;
			return cand;
		}
	}

	return OffsetNumberNext(cand);
}

/*
 * Binary search a container page for ckey.  When not found, the returned
 * offset is the position the container should be inserted at.
 */
OffsetNumber
rbi_page_find_container(Page page, uint32 ckey, bool *found)
{
	OffsetNumber low = FirstOffsetNumber;
	OffsetNumber high = PageGetMaxOffsetNumber(page);

	*found = false;

	/* Invariant: everything below low is < ckey, everything above high is > ckey */
	while (low <= high)
	{
		OffsetNumber mid = low + (high - low) / 2;
		RBIContainer *c = (RBIContainer *) PageGetItem(page, PageGetItemId(page, mid));

		if (c->ckey == ckey)
		{
			*found = true;
			return mid;
		}
		if (c->ckey < ckey)
			low = mid + 1;
		else
		{
			if (mid == FirstOffsetNumber)
				return FirstOffsetNumber;
			high = mid - 1;
		}
	}

	return low;
}

/*
 * Write the caller's private copy of the entry tuple back into the record
 * that is changing the entry's containers.  An entry's size never changes
 * once it is a CHAIN entry, so this cannot fail.
 */
static void
rbi_put_entry(Relation index, GenericXLogState *xstate, Buffer entrybuf,
			  OffsetNumber entryoff, RBIEntryTuple *entry)
{
	if (!rbi_replace_entry(index, xstate, entrybuf, entryoff, entry,
						   RBIEntryPayloadOffset(entry)))
		elog(ERROR, "roaring index: could not update entry tuple at %u/%u",
			 BufferGetBlockNumber(entrybuf), entryoff);
}

/*
 * Insert or replace a container on the container page buf, which the caller
 * holds EXCLUSIVE (or with a cleanup lock) and which owns c->ckey, splitting
 * the page when the container does not fit.  The buffer stays locked.
 *
 * entrybuf/entryoff identify the entry tuple, which the caller holds
 * EXCLUSIVE; entry must be a private (palloc'd) copy with ntids and
 * ncontainers already updated for this change.  It is written back in the
 * same WAL record as the container, so that a crash can never desynchronise
 * the counters from the containers.
 */
void
rbi_chain_put_container_locked(Relation index, Buffer buf, Buffer entrybuf,
							   OffsetNumber entryoff, RBIEntryTuple *entry,
							   RBIContainer *c, int *ncontainers_delta)
{
	Page		page = BufferGetPage(buf);
	OffsetNumber off;
	bool		found;

	Assert(c->type != RBI_CT_SPARSE);
	Assert(RBIPageIsContainer(page));

	off = rbi_page_find_container(page, c->ckey, &found);
	*ncontainers_delta = found ? 0 : 1;

	rbi_chain_put_items_locked(index, buf, entrybuf, entryoff, entry, off,
							   found, &c, 1);
}

/*
 * Replace the item at off, or insert at off, with nitems items.
 *
 * The items must be in ascending ckey order and their ranges must fit in the
 * gap the replaced item (or the insert position) leaves, so that the page
 * stays ordered by first ckey with non-overlapping item ranges (DESIGN.md
 * §13).  Everything happens in one WAL record together with the entry tuple,
 * which is what keeps a segment that is being split around a promoted
 * container from being visible as two items holding the same ckey.
 */
void
rbi_chain_put_items_locked(Relation index, Buffer buf, Buffer entrybuf,
						   OffsetNumber entryoff, RBIEntryTuple *entry,
						   OffsetNumber off, bool replace,
						   RBIContainer **items, int nitems)
{
	Page		page = BufferGetPage(buf);
	Size		sizes[RBI_MAX_PUT_ITEMS];
	Size		need = 0;
	Size		have;
	int			i;

	Assert((entry->flags & RBI_ENTRY_CHAIN) != 0);
	Assert(BlockNumberIsValid(entry->head) && BlockNumberIsValid(entry->tail));
	Assert(RBIPageIsContainer(page));
	Assert(nitems >= 1 && nitems <= RBI_MAX_PUT_ITEMS);

	for (i = 0; i < nitems; i++)
	{
		sizes[i] = rbi_item_size(items[i]);
		Assert(sizes[i] <= RBI_CONTAINER_MAX_SIZE);
		Assert(i == 0 ||
			   rbi_item_first_ckey(items[i]) > rbi_item_last_ckey(items[i - 1]));
		need += MAXALIGN(sizes[i]) + sizeof(ItemIdData);
	}
	Assert(need <= RBI_MAX_ITEM_SIZE + sizeof(ItemIdData));

	/* One item taking another one's place: overwrite it where it is. */
	if (replace && nitems == 1)
	{
		GenericXLogState *xstate = GenericXLogStart(index);
		Page		p = GenericXLogRegisterBuffer(xstate, buf, 0);

		if (PageIndexTupleOverwrite(p, off, items[0], sizes[0]))
		{
			rbi_page_update_minmax(p);
			rbi_put_entry(index, xstate, entrybuf, entryoff, entry);
			GenericXLogFinish(xstate);
			return;
		}
		GenericXLogAbort(xstate);
	}

	/*
	 * Does everything fit as it stands?  PageIndexTupleDelete() compacts, so
	 * the space of the item that goes away is available to the new ones.
	 */
	have = PageGetExactFreeSpace(page);
	if (replace)
		have += MAXALIGN(ItemIdGetLength(PageGetItemId(page, off))) +
			sizeof(ItemIdData);

	if (have >= need)
	{
		GenericXLogState *xstate = GenericXLogStart(index);
		Page		p = GenericXLogRegisterBuffer(xstate, buf, 0);

		if (replace)
			PageIndexTupleDelete(p, off);

		for (i = 0; i < nitems; i++)
		{
			if (PageAddItemExtended(p, items[i], sizes[i],
									off + (OffsetNumber) i,
									0) == InvalidOffsetNumber)
				elog(ERROR, "roaring index: failed to add item to page %u",
					 BufferGetBlockNumber(buf));
		}

		rbi_page_update_minmax(p);
		rbi_put_entry(index, xstate, entrybuf, entryoff, entry);
		GenericXLogFinish(xstate);
		return;
	}

	/* Not enough room: split the page and place the items. */
	rbi_split_and_place(index, buf, off, replace, entrybuf, entryoff, entry,
						items, nitems);
}

/*
 * Insert or replace a container in the chain of entry, splitting pages when
 * necessary.  Finds and locks the owning container page itself; see
 * rbi_chain_put_container_locked() for the contract on entry.
 */
void
rbi_chain_put_container(Relation index, Buffer entrybuf, OffsetNumber entryoff,
						RBIEntryTuple *entry, RBIContainer *c,
						int *ncontainers_delta)
{
	BlockNumber blk;
	Buffer		buf;

	Assert((entry->flags & RBI_ENTRY_CHAIN) != 0);
	Assert(BlockNumberIsValid(entry->head) && BlockNumberIsValid(entry->tail));

	blk = rbi_chain_find_page(index, entry->head, entry->tail, c->ckey);

	buf = ReadBuffer(index, blk);
	LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);

	rbi_chain_put_container_locked(index, buf, entrybuf, entryoff, entry, c,
								   ncontainers_delta);

	UnlockReleaseBuffer(buf);
}

/*
 * Move an INLINE entry's payload onto a chain of container pages and turn it
 * into a CHAIN entry.
 *
 * entry is a private copy of the entry tuple in its CHAIN shape (no payload,
 * with ncontainers and ntids already set by the caller); payload holds the
 * containers to write out, packed as in an INLINE payload.  The entry page is
 * held EXCLUSIVE by the caller and is rewritten here, in the same WAL record
 * as the last container page.
 */
void
rbi_entry_spill(Relation index, Buffer entrybuf, OffsetNumber entryoff,
				RBIEntryTuple *entry, const char *payload, Size paylen)
{
	GenericXLogState *xstate;
	Buffer		curbuf;
	Page		curpage;
	RBIContainer *cbuf;
	BlockNumber head;
	Size		off = 0;
	Size		csize;

	cbuf = (RBIContainer *) palloc(RBI_CONTAINER_MAX_SIZE);

	xstate = GenericXLogStart(index);
	curbuf = rbi_new_buffer_xl(index, xstate, RBI_PAGE_CONTAINER, &curpage);
	head = BufferGetBlockNumber(curbuf);

	while ((csize = rbi_inline_fetch(payload, paylen, &off, cbuf)) > 0)
	{
		if (PageGetFreeSpace(curpage) < MAXALIGN(csize))
		{
			Buffer		nextbuf;
			Page		nextpage;

			/*
			 * Link the next page in from the current one inside the same
			 * record, then continue the walk in a new record.
			 */
			nextbuf = rbi_new_buffer_xl(index, xstate, RBI_PAGE_CONTAINER,
										&nextpage);
			rbi_page_update_minmax(curpage);
			RBIPageGetOpaque(curpage)->rightlink = BufferGetBlockNumber(nextbuf);
			GenericXLogFinish(xstate);
			UnlockReleaseBuffer(curbuf);

			curbuf = nextbuf;
			xstate = GenericXLogStart(index);
			curpage = GenericXLogRegisterBuffer(xstate, curbuf, 0);
		}

		if (PageAddItemExtended(curpage, cbuf, csize,
								InvalidOffsetNumber, 0) == InvalidOffsetNumber)
			elog(ERROR, "roaring index: failed to spill container to page %u",
				 BufferGetBlockNumber(curbuf));
	}

	rbi_page_update_minmax(curpage);

	/* Only the INLINE/CHAIN half of the flags changes: the reserved NULL-key
	 * entry stays the NULL-key entry once its payload moves to a chain. */
	entry->flags = (entry->flags & RBI_ENTRY_NULLKEY) | RBI_ENTRY_CHAIN;
	entry->head = head;
	entry->tail = BufferGetBlockNumber(curbuf);
	rbi_put_entry(index, xstate, entrybuf, entryoff, entry);

	GenericXLogFinish(xstate);
	UnlockReleaseBuffer(curbuf);

	pfree(cbuf);
}

/*
 * Split page buf and place the caller's items, all in one WAL record.
 *
 * Items at offsets off..maxoff (excluding the stale item at off when replace
 * is true, which is dropped) move to a brand new page N linked immediately
 * after P.  If the new items still do not fit on P, a second new page M
 * holding them is linked between P and N.  This always makes progress because
 * the caller's items together fit on an empty page (they are at most
 * RBI_MAX_PUT_ITEMS items of at most RBI_CONTAINER_MAX_SIZE bytes, and a
 * segment split only ever adds a few bytes to what was one item).  Items
 * never move left and never move to an existing page.
 */
static void
rbi_split_and_place(Relation index, Buffer buf, OffsetNumber off, bool replace,
					Buffer entrybuf, OffsetNumber entryoff,
					RBIEntryTuple *entry, RBIContainer **items, int nitems)
{
	Page		page = BufferGetPage(buf);
	BlockNumber blk = BufferGetBlockNumber(buf);
	OffsetNumber maxoff = PageGetMaxOffsetNumber(page);
	OffsetNumber firstright = replace ? OffsetNumberNext(off) : off;
	OffsetNumber delfirst = off;
	int			ndel = (int) (maxoff + 1 - delfirst);
	int			nmove = (int) (maxoff + 1 - firstright);
	Size		sizes[RBI_MAX_PUT_ITEMS];
	Size		need = 0;
	char	   *movebuf = NULL;
	Size	   *movelen = NULL;
	char	  **moveptr = NULL;
	OffsetNumber *delofs = NULL;
	GenericXLogState *xstate;
	Page		pP;
	Page		pN = NULL;
	Page		pM = NULL;
	Buffer		nbuf = InvalidBuffer;
	Buffer		mbuf = InvalidBuffer;
	BlockNumber oldright = RBIPageGetOpaque(page)->rightlink;
	BlockNumber nblk = InvalidBlockNumber;
	BlockNumber mblk = InvalidBlockNumber;
	int			i;

	Assert(ndel >= 0 && nmove >= 0 && nmove <= ndel);
	Assert(nitems >= 1 && nitems <= RBI_MAX_PUT_ITEMS);

	for (i = 0; i < nitems; i++)
	{
		sizes[i] = rbi_item_size(items[i]);
		need += MAXALIGN(sizes[i]) + sizeof(ItemIdData);
	}

	/* Copy out the items that are going to move, before touching the page. */
	if (nmove > 0)
	{
		Size		used = 0;

		movebuf = (char *) palloc(BLCKSZ);
		movelen = (Size *) palloc(sizeof(Size) * nmove);
		moveptr = (char **) palloc(sizeof(char *) * nmove);

		for (i = 0; i < nmove; i++)
		{
			ItemId		iid = PageGetItemId(page, firstright + i);
			Size		sz = ItemIdGetLength(iid);

			Assert(used + MAXALIGN(sz) <= BLCKSZ);
			memcpy(movebuf + used, PageGetItem(page, iid), sz);
			moveptr[i] = movebuf + used;
			movelen[i] = sz;
			used += MAXALIGN(sz);
		}
	}

	if (ndel > 0)
	{
		delofs = (OffsetNumber *) palloc(sizeof(OffsetNumber) * ndel);
		for (i = 0; i < ndel; i++)
			delofs[i] = delfirst + i;
	}

	xstate = GenericXLogStart(index);
	pP = GenericXLogRegisterBuffer(xstate, buf, 0);

	/*
	 * The new page(s) are allocated inside this record, so that replay either
	 * sees them linked into the chain or not at all.
	 */
	if (nmove > 0)
	{
		nbuf = rbi_new_buffer_xl(index, xstate, RBI_PAGE_CONTAINER, &pN);
		nblk = BufferGetBlockNumber(nbuf);
	}

	if (ndel > 0)
		PageIndexMultiDelete(pP, delofs, ndel);
	rbi_page_update_minmax(pP);

	if (PageGetExactFreeSpace(pP) >= need)
	{
		for (i = 0; i < nitems; i++)
		{
			if (PageAddItemExtended(pP, items[i], sizes[i],
									InvalidOffsetNumber, 0) == InvalidOffsetNumber)
				elog(ERROR, "roaring index: failed to place item after split");
		}
		rbi_page_update_minmax(pP);
	}
	else
	{
		/* The items get a page of their own, linked immediately after P. */
		mbuf = rbi_new_buffer_xl(index, xstate, RBI_PAGE_CONTAINER, &pM);
		mblk = BufferGetBlockNumber(mbuf);
		for (i = 0; i < nitems; i++)
		{
			if (PageAddItemExtended(pM, items[i], sizes[i],
									InvalidOffsetNumber, 0) == InvalidOffsetNumber)
				elog(ERROR, "roaring index: failed to place item on new page");
		}
		rbi_page_update_minmax(pM);
	}

	if (nmove > 0)
	{
		for (i = 0; i < nmove; i++)
		{
			if (PageAddItemExtended(pN, moveptr[i], movelen[i],
									InvalidOffsetNumber, 0) == InvalidOffsetNumber)
				elog(ERROR, "roaring index: failed to move container during split");
		}
		rbi_page_update_minmax(pN);
	}

	/* Relink: P -> [M] -> [N] -> oldright */
	{
		BlockNumber after_m = BlockNumberIsValid(nblk) ? nblk : oldright;

		if (BlockNumberIsValid(mblk))
		{
			RBIPageGetOpaque(pM)->rightlink = after_m;
			RBIPageGetOpaque(pP)->rightlink = mblk;
		}
		else
			RBIPageGetOpaque(pP)->rightlink = after_m;

		if (BlockNumberIsValid(nblk))
			RBIPageGetOpaque(pN)->rightlink = oldright;
	}

	/* If P was the tail, the chain has a new last page. */
	if (entry->tail == blk)
	{
		BlockNumber newtail = BlockNumberIsValid(nblk) ? nblk : mblk;

		Assert(!BlockNumberIsValid(oldright));
		if (BlockNumberIsValid(newtail))
			entry->tail = newtail;
	}

	/* The entry always travels with the container change. */
	rbi_put_entry(index, xstate, entrybuf, entryoff, entry);

	GenericXLogFinish(xstate);

	if (BufferIsValid(mbuf))
		UnlockReleaseBuffer(mbuf);
	if (BufferIsValid(nbuf))
		UnlockReleaseBuffer(nbuf);

	if (movebuf)
	{
		pfree(movebuf);
		pfree(movelen);
		pfree(moveptr);
	}
	if (delofs)
		pfree(delofs);
}
