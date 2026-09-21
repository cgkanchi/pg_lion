/*-------------------------------------------------------------------------
 *
 * rbi_scan.c
 *		Bitmap scan support for the roaring index (DESIGN.md section 5, SCAN).
 *
 * A roaring index answers one qual on its single key column: an equality to a
 * value, `= ANY (array)` (DESIGN.md §15, amsearcharray), `IS NULL` or
 * `IS NOT NULL` (DESIGN.md §14, amsearchnulls).  The scan finds the entries
 * the qual selects and emits their posting sets into the caller's TIDBitmap.
 *
 * A multi-key opclass (DESIGN.md §17) answers `@>`, `&&`, `<@` and `@@`
 * instead.  The query is handed to the opclass's extractQuery function, which
 * yields keys and a mode; an exact mode gives a boolean tree over those keys,
 * whose posting sets are combined by the very evaluator the count pushdown
 * uses (rbi_sets_iterate(), DESIGN.md §9 cursors), and anything else falls
 * back to emitting every indexed row with recheck set.
 *
 * Only one page lock is held at a time, and never across the TIDBitmap calls:
 * every page is copied into backend-local memory before its containers are
 * emitted.  The bucket page is always released before any container page is
 * pinned, which is the reader side of the deadlock rule in DESIGN.md §11.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/relscan.h"
#include "executor/instrument_node.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "storage/bufmgr.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"

#include "rbi.h"
#include "rbi_count.h"

typedef struct RBIScanOpaqueData
{
	RBIState   *state;			/* cached relation state */
	Oid			subtype;		/* type of the scan key argument */
	FmgrInfo	subhashproc;	/* hash function for that type */
	bool		subhashvalid;
	bool		recheck;		/* the emitted TIDs are a superset */
} RBIScanOpaqueData;

typedef RBIScanOpaqueData *RBIScanOpaque;

/* State threaded through rbi_container_iterate() by rbi_container_to_tbm() */
typedef struct RBITbmState
{
	TIDBitmap  *tbm;
	BlockNumber firstblk;		/* first heap block of the container */
	BlockNumber curblk;			/* heap block the buffered TIDs belong to */
	int			ntids;
	int64		total;
	bool		recheck;		/* passed on to tbm_add_tuples() */
	ItemPointerData tids[RBI_MAX_OFFSET + 1];
} RBITbmState;

static bool
rbi_tbm_callback(uint16 lo, void *arg)
{
	RBITbmState *st = (RBITbmState *) arg;
	uint16		blkinc;
	OffsetNumber off;
	BlockNumber blk;

	rbi_lo_split(lo, &blkinc, &off);
	blk = st->firstblk + (BlockNumber) blkinc;

	/*
	 * Members arrive in ascending lo order, hence in heap block order, so one
	 * tbm_add_tuples() call per heap block is enough.  The length test is
	 * belt and braces: a corrupt container must not overrun tids[].
	 */
	if (st->ntids > 0 &&
		(blk != st->curblk || st->ntids > RBI_MAX_OFFSET))
	{
		tbm_add_tuples(st->tbm, st->tids, st->ntids, st->recheck);
		st->ntids = 0;
	}
	st->curblk = blk;

	Assert(st->ntids <= RBI_MAX_OFFSET);
	ItemPointerSet(&st->tids[st->ntids], blk, off);
	st->ntids++;
	st->total++;
	return true;
}

/*
 * The same for a sparse segment (DESIGN.md §13): its pairs are sorted by
 * (ckey, lo), hence by heap block, so the very same buffering emits one
 * tbm_add_tuples() call per heap block.  Only the first block of the pair's
 * own container key has to be recomputed as the ckey changes.
 */
static bool
rbi_tbm_pair_callback(uint32 ckey, uint16 lo, void *arg)
{
	RBITbmState *st = (RBITbmState *) arg;

	st->firstblk = rbi_ckey_first_block(ckey);
	return rbi_tbm_callback(lo, arg);
}

/*
 * Emit one item (container or sparse segment) into a TIDBitmap, one
 * tbm_add_tuples() call per heap block.  Returns the number of TIDs emitted.
 */
int64
rbi_container_to_tbm(const RBIContainer *c, TIDBitmap *tbm, bool recheck)
{
	RBITbmState st;

	st.tbm = tbm;
	st.firstblk = rbi_ckey_first_block(c->ckey);
	st.curblk = InvalidBlockNumber;
	st.ntids = 0;
	st.total = 0;
	st.recheck = recheck;

	if (c->type == RBI_CT_SPARSE)
		rbi_sparse_iterate(c, rbi_tbm_pair_callback, &st);
	else
		rbi_container_iterate(c, rbi_tbm_callback, &st);

	if (st.ntids > 0)
		tbm_add_tuples(tbm, st.tids, st.ntids, st.recheck);

	return st.total;
}

/*
 * Emit every item of an INLINE entry payload.  Those items are packed
 * without padding, so each one is copied into an aligned buffer first.
 */
static int64
rbi_emit_inline(const char *payload, Size paylen, TIDBitmap *tbm, bool recheck)
{
	int64		ntids = 0;
	Size		off = 0;
	RBIContainer *cbuf = (RBIContainer *) palloc(RBI_CONTAINER_MAX_SIZE);

	while (rbi_inline_fetch(payload, paylen, &off, cbuf) > 0)
		ntids += rbi_container_to_tbm(cbuf, tbm, recheck);

	pfree(cbuf);
	return ntids;
}

IndexScanDesc
rbibeginscan(Relation r, int nkeys, int norderbys)
{
	IndexScanDesc scan;
	RBIScanOpaque so;

	scan = RelationGetIndexScan(r, nkeys, norderbys);

	so = (RBIScanOpaque) palloc0(sizeof(RBIScanOpaqueData));
	so->state = rbi_get_state(r);
	so->subtype = InvalidOid;
	so->subhashvalid = false;

	scan->opaque = so;

	return scan;
}

void
rbirescan(IndexScanDesc scan, ScanKey scankey, int nscankeys,
		  ScanKey orderbys, int norderbys)
{
	if (scankey && scan->numberOfKeys > 0)
		memcpy(scan->keyData, scankey,
			   scan->numberOfKeys * sizeof(ScanKeyData));
}

void
rbiendscan(IndexScanDesc scan)
{
	RBIScanOpaque so = (RBIScanOpaque) scan->opaque;

	if (so != NULL)
	{
		pfree(so);
		scan->opaque = NULL;
	}
}

/*
 * Emit every container of a CHAIN entry, walking the chain one shared lock at
 * a time.  The page image (items and rightlink) is read atomically under the
 * lock, so a concurrent split - which only ever moves items rightwards onto a
 * brand new page - cannot make a reader miss or repeat a container.
 *
 * No bucket page may be held here: DESIGN.md §11 forbids taking a bucket lock
 * while a container page is pinned, and this function pins them.
 */
static int64
rbi_emit_chain(Relation index, BlockNumber blkno, TIDBitmap *tbm, bool recheck)
{
	PGAlignedBlock *copy;
	int64		ntids = 0;

	if (!BlockNumberIsValid(blkno))
		return 0;

	copy = (PGAlignedBlock *) palloc(sizeof(PGAlignedBlock));

	while (BlockNumberIsValid(blkno))
	{
		Buffer		buf;
		Page		page;
		Page		cpage = (Page) copy->data;
		OffsetNumber maxoff;
		OffsetNumber off;

		buf = ReadBuffer(index, blkno);
		LockBuffer(buf, BUFFER_LOCK_SHARE);
		page = BufferGetPage(buf);
		if (!RBIPageIsContainer(page))
		{
			UnlockReleaseBuffer(buf);
			elog(ERROR, "roaring index: block %u is not a container page",
				 blkno);
		}
		memcpy(cpage, page, BLCKSZ);
		UnlockReleaseBuffer(buf);

		blkno = RBIPageGetOpaque(cpage)->rightlink;

		maxoff = PageGetMaxOffsetNumber(cpage);
		for (off = FirstOffsetNumber; off <= maxoff; off++)
		{
			ItemId		iid = PageGetItemId(cpage, off);

			if (!ItemIdIsUsed(iid))
				continue;
			ntids += rbi_container_to_tbm((RBIContainer *) PageGetItem(cpage, iid),
										  tbm, recheck);
		}

		CHECK_FOR_INTERRUPTS();
	}

	pfree(copy);
	return ntids;
}

/*
 * Emit the posting set of the entry at (entrybuf, entryoff), which the caller
 * holds SHARE-locked together with the bucket head headbuf (they may be the
 * same buffer).  Both are released here, before any container page is
 * touched.
 */
static int64
rbi_emit_entry(Relation index, Buffer headbuf, Buffer entrybuf,
			   OffsetNumber entryoff, TIDBitmap *tbm, bool recheck)
{
	Page		page = BufferGetPage(entrybuf);
	ItemId		iid = PageGetItemId(page, entryoff);
	RBIEntryTuple *entry = (RBIEntryTuple *) PageGetItem(page, iid);
	char	   *payload = NULL;
	Size		paylen = 0;
	BlockNumber blkno = InvalidBlockNumber;
	int64		ntids;

	if (entry->flags & RBI_ENTRY_INLINE)
	{
		paylen = RBI_ENTRY_PAYLOAD_LEN(entry, ItemIdGetLength(iid));
		if (paylen > 0)
		{
			payload = (char *) palloc(paylen);
			memcpy(payload, RBIEntryGetPayload(entry), paylen);
		}
	}
	else
	{
		Assert(entry->flags & RBI_ENTRY_CHAIN);
		blkno = entry->head;
	}

	if (entrybuf != headbuf)
		UnlockReleaseBuffer(entrybuf);
	UnlockReleaseBuffer(headbuf);

	if (payload != NULL)
	{
		ntids = rbi_emit_inline(payload, paylen, tbm, recheck);
		pfree(payload);
		return ntids;
	}

	return rbi_emit_chain(index, blkno, tbm, recheck);
}

/*
 * Hash a search value.  Cross-type equality (int4 column = int8 constant)
 * needs the hash function of the value's own type, exactly as the hash AM
 * does in _hash_datum2hashkey_type().  subtype is the scan key's sk_subtype,
 * which for an array key is the type of its elements.
 */
static uint32
rbi_scankey_hash(IndexScanDesc scan, RBIScanOpaque so, Oid subtype,
				 Oid collation, Datum value)
{
	Relation	index = scan->indexRelation;

	if (!OidIsValid(subtype) || subtype == index->rd_opcintype[0])
		return rbi_hash_key(so->state, value);

	if (!so->subhashvalid || so->subtype != subtype)
	{
		Oid			hashproc;

		hashproc = get_opfamily_proc(index->rd_opfamily[0],
									 subtype, subtype, 1);
		if (!OidIsValid(hashproc))
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_FUNCTION),
					 errmsg("missing support function 1 for type %s in operator family \"%s\"",
							format_type_be(subtype),
							get_opfamily_name(index->rd_opfamily[0], false))));
		fmgr_info(hashproc, &so->subhashproc);
		so->subtype = subtype;
		so->subhashvalid = true;
	}

	return DatumGetUInt32(FunctionCall1Coll(&so->subhashproc, collation,
											value));
}

/*
 * Emit the posting set of one search value.
 */
static int64
rbi_emit_value(IndexScanDesc scan, RBIScanOpaque so, ScanKey skey, Datum value,
			   TIDBitmap *tbm)
{
	Relation	index = scan->indexRelation;
	RBIState   *state = so->state;
	uint32		hash;
	Buffer		headbuf;
	Buffer		entrybuf;
	OffsetNumber entryoff;

	hash = rbi_scankey_hash(scan, so, skey->sk_subtype, skey->sk_collation,
							value);

	headbuf = ReadBuffer(index,
						 RBI_BUCKET_BLKNO(rbi_bucket_of(hash,
														state->meta.nbuckets)));
	LockBuffer(headbuf, BUFFER_LOCK_SHARE);

	if (!rbi_find_entry_ext(index, state, headbuf, BUFFER_LOCK_SHARE,
							value, hash, &skey->sk_func, skey->sk_collation,
							&entrybuf, &entryoff))
	{
		UnlockReleaseBuffer(headbuf);
		return 0;
	}

	return rbi_emit_entry(index, headbuf, entrybuf, entryoff, tbm, so->recheck);
}

/*
 * `col = ANY (array)` (DESIGN.md §15).  The elements are looked up one at a
 * time and their sets emitted into the same bitmap; NULL elements select
 * nothing, and repeated elements are harmless because a TIDBitmap is a set.
 */
static int64
rbi_emit_array(IndexScanDesc scan, RBIScanOpaque so, ScanKey skey,
			   TIDBitmap *tbm)
{
	ArrayType  *arr = DatumGetArrayTypeP(skey->sk_argument);
	Oid			elemtype = ARR_ELEMTYPE(arr);
	int16		elmlen;
	bool		elmbyval;
	char		elmalign;
	Datum	   *elems;
	bool	   *nulls;
	int			nelems;
	int64		ntids = 0;
	int			i;

	get_typlenbyvalalign(elemtype, &elmlen, &elmbyval, &elmalign);
	deconstruct_array(arr, elemtype, elmlen, elmbyval, elmalign,
					  &elems, &nulls, &nelems);

	for (i = 0; i < nelems; i++)
	{
		if (nulls[i])
			continue;			/* `x = NULL` is never true */
		ntids += rbi_emit_value(scan, so, skey, elems[i], tbm);
		CHECK_FOR_INTERRUPTS();
	}

	pfree(elems);
	pfree(nulls);
	if ((Pointer) arr != DatumGetPointer(skey->sk_argument))
		pfree(arr);

	return ntids;
}

/*
 * `col IS NULL` (DESIGN.md §14): the reserved NULL entry of bucket 0.
 */
static int64
rbi_emit_null(Relation index, RBIState *state, TIDBitmap *tbm, bool recheck)
{
	Buffer		headbuf;
	Buffer		entrybuf;
	OffsetNumber entryoff;

	headbuf = ReadBuffer(index, RBI_BUCKET_BLKNO(RBI_NULLKEY_BUCKET));
	LockBuffer(headbuf, BUFFER_LOCK_SHARE);

	if (!rbi_find_null_entry(index, headbuf, BUFFER_LOCK_SHARE,
							 &entrybuf, &entryoff))
	{
		UnlockReleaseBuffer(headbuf);
		return 0;
	}

	return rbi_emit_entry(index, headbuf, entrybuf, entryoff, tbm, recheck);
}

/*
 * Every indexed row: the union of every entry but the NULL one, which means
 * walking the whole index.  Correct, and expensive in proportion to the
 * number of distinct keys - the planner's cost estimate for such a scan is
 * the whole index, so it only happens when that is cheap or when nothing else
 * can answer the query.
 *
 * Two quals need it:
 *
 *	- `col IS NOT NULL` (DESIGN.md §14), which is exactly "every row whose
 *	  value is not NULL" and needs no recheck of its own;
 *	- the ALL fallback of a multi-key query (DESIGN.md §17): `tags @> '{}'`,
 *	  `<@`, a tsquery with NOT/phrase/prefix/weights.  Those ask for a
 *	  superset and the caller passes recheck = true.
 *
 * The reserved EMPTY entry is emitted like any other, which is what it exists
 * for: a row an opclass extracted no key from is under no key, and only a
 * walk like this one can find it.  The NULL entry is left out, because a NULL
 * value satisfies neither kind of qual (the multi-key operators are strict).
 *
 * Each bucket page is copied into backend-local memory and released before
 * its entries are emitted, so no bucket page is held while container pages
 * are pinned (DESIGN.md §11).  Working from the copy can miss an entry added
 * after the copy was taken, which is exactly as acceptable as it is for a
 * single key: such an entry can only hold TIDs that no snapshot older than
 * this scan can see.
 */
static int64
rbi_emit_all_keys(Relation index, RBIState *state, TIDBitmap *tbm,
				  bool recheck)
{
	PGAlignedBlock *copy = (PGAlignedBlock *) palloc(sizeof(PGAlignedBlock));
	Page		cpage = (Page) copy->data;
	int64		ntids = 0;
	uint32		b;

	for (b = 0; b < state->meta.nbuckets; b++)
	{
		BlockNumber blkno = RBI_BUCKET_BLKNO(b);

		while (BlockNumberIsValid(blkno))
		{
			Buffer		buf;
			Page		page;
			OffsetNumber maxoff;
			OffsetNumber off;

			buf = ReadBuffer(index, blkno);
			LockBuffer(buf, BUFFER_LOCK_SHARE);
			page = BufferGetPage(buf);
			if (!RBIPageIsBucket(page))
			{
				UnlockReleaseBuffer(buf);
				elog(ERROR, "roaring index: block %u is not a bucket page",
					 blkno);
			}
			memcpy(cpage, page, BLCKSZ);
			UnlockReleaseBuffer(buf);

			blkno = RBIPageGetOpaque(cpage)->rightlink;
			maxoff = PageGetMaxOffsetNumber(cpage);

			for (off = FirstOffsetNumber; off <= maxoff; off++)
			{
				ItemId		iid = PageGetItemId(cpage, off);
				RBIEntryTuple *entry;

				if (!ItemIdIsUsed(iid))
					continue;
				entry = (RBIEntryTuple *) PageGetItem(cpage, iid);
				if (RBIEntryIsNullKey(entry))
					continue;

				if ((entry->flags & RBI_ENTRY_INLINE) != 0)
					ntids += rbi_emit_inline(RBIEntryGetPayload(entry),
											 RBI_ENTRY_PAYLOAD_LEN(entry,
																   ItemIdGetLength(iid)),
											 tbm, recheck);
				else
					ntids += rbi_emit_chain(index, entry->head, tbm, recheck);

				CHECK_FOR_INTERRUPTS();
			}
		}
	}

	pfree(copy);
	return ntids;
}

/* ---------------------------------------------------------------------
 * Multi-key opclasses (DESIGN.md §17)
 * --------------------------------------------------------------------- */

/* State threaded through rbi_sets_iterate() by rbi_emit_query(). */
typedef struct RBIQueryEmitState
{
	TIDBitmap  *tbm;
	bool		recheck;
	int64		ntids;
} RBIQueryEmitState;

static bool
rbi_query_emit_cb(const RBIContainer *c, void *arg)
{
	RBIQueryEmitState *es = (RBIQueryEmitState *) arg;

	es->ntids += rbi_container_to_tbm(c, es->tbm, es->recheck);
	return true;
}

/*
 * Answer one multi-key query (`tags @> '{a,b}'`, `tsv @@ 'a & b'`, ...).
 *
 * The opclass's extractQuery function says which keys the query needs and how
 * exactly they answer it; rbi_extract_query() turns that into one of three
 * shapes (DESIGN.md §17):
 *
 *	NONE	nothing can match - `tags && '{}'`, an empty tsquery;
 *	KEYS	the rows are exactly the ones a boolean tree over the keys
 *			selects, so the posting sets are located and combined and the
 *			TIDs go out without a recheck of their own;
 *	ALL		the index cannot decide, so every indexed row goes out with
 *			recheck set and the bitmap heap scan re-applies the operator.
 *
 * The posting sets are ALL located before any of them is walked, which keeps
 * the reader side of the DESIGN.md §11 deadlock rule: every bucket-page lock
 * this function takes is taken before the first container page is pinned.
 */
static int64
rbi_emit_query(Relation index, RBIState *state, StrategyNumber strategy,
			   Datum query, TIDBitmap *tbm, bool recheck)
{
	RBIQuery	q;
	RBIPostingSet *sets;
	RBIQueryEmitState es;
	MemoryContext cxt;
	MemoryContext oldcxt;
	int			i;

	/*
	 * The extracted keys, the tree and the located payloads all live and die
	 * with one query.  A scan of `col op ANY (...)` calls this once per
	 * element, and a tsquery can carry hundreds of lexemes, so they get a
	 * context of their own rather than the executor's.
	 */
	cxt = AllocSetContextCreate(CurrentMemoryContext,
								"roaring index multikey scan",
								ALLOCSET_DEFAULT_SIZES);
	oldcxt = MemoryContextSwitchTo(cxt);

	rbi_extract_query(state, query, strategy, &q);

	if (q.mode != RBI_QMODE_KEYS)
	{
		int64		ntids = 0;

		MemoryContextSwitchTo(oldcxt);
		if (q.mode == RBI_QMODE_ALL)
			ntids = rbi_emit_all_keys(index, state, tbm, true);
		MemoryContextDelete(cxt);
		return ntids;
	}

	Assert(q.nkeys > 0 && q.tree != NULL);

	sets = (RBIPostingSet *) palloc0(sizeof(RBIPostingSet) * q.nkeys);
	for (i = 0; i < q.nkeys; i++)
	{
		(void) rbi_posting_set_lookup(index, q.keys[i], InvalidOid, &sets[i]);
		CHECK_FOR_INTERRUPTS();
	}

	es.tbm = tbm;
	es.recheck = recheck;
	es.ntids = 0;

	(void) rbi_sets_iterate(q.nkeys, sets, q.tree, rbi_query_emit_cb, &es);

	/* Every pin goes before the memory the sets live in does. */
	for (i = 0; i < q.nkeys; i++)
		rbi_posting_set_release(&sets[i]);

	MemoryContextSwitchTo(oldcxt);
	MemoryContextDelete(cxt);

	return es.ntids;
}

/*
 * The multi-key entry point of the scan.
 *
 * `col op ANY (const array)` reaches a multi-key index too (amsearcharray is
 * on for the scalar classes of DESIGN.md §15), and there the array holds one
 * QUERY per element - an array of arrays, or of tsqueries.  Each is answered
 * separately and the results go into the same bitmap, which is their union
 * and exactly what `ANY` means; a TIDBitmap is a set, so overlapping elements
 * cost nothing but the second lookup.
 */
static int64
rbi_emit_multikey(IndexScanDesc scan, RBIScanOpaque so, ScanKey skey,
				  TIDBitmap *tbm)
{
	Relation	index = scan->indexRelation;
	RBIState   *state = so->state;

	/*
	 * A strategy this file does not know is an ERROR and not an empty result:
	 * the opclass would be claiming an operator the scan cannot answer, and
	 * answering "no rows" would be a wrong answer rather than a missing
	 * optimisation.  rbi_gin_strategy() raises it.
	 */
	if ((skey->sk_flags & SK_SEARCHARRAY) != 0)
	{
		ArrayType  *arr = DatumGetArrayTypeP(skey->sk_argument);
		Oid			elemtype = ARR_ELEMTYPE(arr);
		int16		elmlen;
		bool		elmbyval;
		char		elmalign;
		Datum	   *elems;
		bool	   *nulls;
		int			nelems;
		int64		ntids = 0;
		int			i;

		get_typlenbyvalalign(elemtype, &elmlen, &elmbyval, &elmalign);
		deconstruct_array(arr, elemtype, elmlen, elmbyval, elmalign,
						  &elems, &nulls, &nelems);

		for (i = 0; i < nelems; i++)
		{
			if (nulls[i])
				continue;		/* a strict operator with a NULL is not true */
			ntids += rbi_emit_query(index, state, skey->sk_strategy,
									elems[i], tbm, so->recheck);
			CHECK_FOR_INTERRUPTS();
		}

		pfree(elems);
		pfree(nulls);
		if ((Pointer) arr != DatumGetPointer(skey->sk_argument))
			pfree(arr);

		return ntids;
	}

	return rbi_emit_query(index, state, skey->sk_strategy, skey->sk_argument,
						  tbm, so->recheck);
}

int64
rbigetbitmap(IndexScanDesc scan, TIDBitmap *tbm)
{
	Relation	index = scan->indexRelation;
	RBIScanOpaque so = (RBIScanOpaque) scan->opaque;
	RBIState   *state;
	ScanKey		skey;

	/*
	 * Re-fetch the cached state: a relcache invalidation since ambeginscan
	 * would have thrown the copy in rd_amcache away.
	 */
	state = so->state = rbi_get_state(index);

	pgstat_count_index_scan(index);
	if (scan->instrument)
		scan->instrument->nsearches++;

	if (scan->numberOfKeys < 1)
		return 0;

	/*
	 * The index has one key column, but the planner may still hand it more
	 * than one qual on that column (`b = ANY (x) AND b = ANY (y)`, or an
	 * equality next to a null test).  Only one of them is answered here, and
	 * every TID is then marked for recheck: the bitmap heap scan re-applies
	 * the original quals to each tuple it fetches, so a superset is correct.
	 * The qual answered is the most selective-looking one, because the others
	 * only shrink the result: a plain equality first, then a list, then a
	 * null test.
	 */
	so->recheck = (scan->numberOfKeys > 1);
	skey = &scan->keyData[0];
	if (scan->numberOfKeys > 1)
	{
		int			i;
		int			best = 0;
		int			bestrank = 3;

		for (i = 0; i < scan->numberOfKeys; i++)
		{
			ScanKey		k = &scan->keyData[i];
			int			rank;

			if (k->sk_attno != 1)
				continue;
			if ((k->sk_flags & (SK_SEARCHNULL | SK_SEARCHNOTNULL)) != 0)
				rank = 2;
			else if ((k->sk_flags & SK_SEARCHARRAY) != 0)
				rank = 1;
			else
				rank = 0;

			if (rank < bestrank)
			{
				bestrank = rank;
				best = i;
			}
		}
		skey = &scan->keyData[best];
	}

	if (skey->sk_attno != 1)
		return 0;

	/* The null tests carry no strategy number and must be tested first. */
	if ((skey->sk_flags & SK_SEARCHNULL) != 0)
		return rbi_emit_null(index, state, tbm, so->recheck);
	if ((skey->sk_flags & SK_SEARCHNOTNULL) != 0)
		return rbi_emit_all_keys(index, state, tbm, so->recheck);

	/* `col = NULL` (or a NULL array) is never true. */
	if ((skey->sk_flags & SK_ISNULL) != 0)
		return 0;

	/* A multi-key opclass answers the strategies of DESIGN.md §17. */
	if (state->multikey)
		return rbi_emit_multikey(scan, so, skey, tbm);

	if (skey->sk_strategy != RBI_STRAT_EQUAL)
		return 0;

	if ((skey->sk_flags & SK_SEARCHARRAY) != 0)
		return rbi_emit_array(scan, so, skey, tbm);

	return rbi_emit_value(scan, so, skey, skey->sk_argument, tbm);
}
