/*-------------------------------------------------------------------------
 *
 * lion_scan.c
 *		Bitmap scan support for the lion index (DESIGN.md section 5, SCAN).
 *
 * A lion index answers one qual on its single key column: an equality to a
 * value, `= ANY (array)` (DESIGN.md §15, amsearcharray), `IS NULL` or
 * `IS NOT NULL` (DESIGN.md §14, amsearchnulls).  The scan finds the entries
 * the qual selects and emits their posting sets into the caller's TIDBitmap.
 *
 * A multi-key opclass (DESIGN.md §17) answers `@>`, `&&`, `<@` and `@@`
 * instead.  The query is handed to the opclass's extractQuery function, which
 * yields keys and a mode; an exact mode gives a boolean tree over those keys,
 * whose posting sets are combined by the very evaluator the count pushdown
 * uses (lion_sets_iterate(), DESIGN.md §9 cursors), and anything else falls
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
#include "utils/injection_point.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "storage/bufmgr.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"

#include "lion.h"
#include "lion_count.h"

typedef struct LionScanOpaqueData
{
	LionState   *state;			/* cached relation state */
	Oid			subtype;		/* type of the scan key argument */
	FmgrInfo	subhashproc;	/* hash function for that type */
	bool		subhashvalid;
	bool		recheck;		/* the emitted TIDs are a superset */
} LionScanOpaqueData;

typedef LionScanOpaqueData *LionScanOpaque;

/* State threaded through lion_container_iterate() by lion_container_to_tbm() */
typedef struct LionTbmState
{
	TIDBitmap  *tbm;
	BlockNumber firstblk;		/* first heap block of the container */
	BlockNumber curblk;			/* heap block the buffered TIDs belong to */
	int			ntids;
	int64		total;
	bool		recheck;		/* passed on to tbm_add_tuples() */
	ItemPointerData tids[LION_MAX_OFFSET + 1];
} LionTbmState;

static bool
lion_tbm_callback(uint16 lo, void *arg)
{
	LionTbmState *st = (LionTbmState *) arg;
	uint16		blkinc;
	OffsetNumber off;
	BlockNumber blk;

	lion_lo_split(lo, &blkinc, &off);
	blk = st->firstblk + (BlockNumber) blkinc;

	/*
	 * Members arrive in ascending lo order, hence in heap block order, so one
	 * tbm_add_tuples() call per heap block is enough.  The length test is
	 * belt and braces: a corrupt container must not overrun tids[].
	 */
	if (st->ntids > 0 &&
		(blk != st->curblk || st->ntids > LION_MAX_OFFSET))
	{
		tbm_add_tuples(st->tbm, st->tids, st->ntids, st->recheck);
		st->ntids = 0;
	}
	st->curblk = blk;

	Assert(st->ntids <= LION_MAX_OFFSET);
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
lion_tbm_pair_callback(uint32 ckey, uint16 lo, void *arg)
{
	LionTbmState *st = (LionTbmState *) arg;

	st->firstblk = lion_ckey_first_block(ckey);
	return lion_tbm_callback(lo, arg);
}

/*
 * Emit one item (container or sparse segment) into a TIDBitmap, one
 * tbm_add_tuples() call per heap block.  Returns the number of TIDs emitted.
 */
int64
lion_container_to_tbm(const LionContainer *c, TIDBitmap *tbm, bool recheck)
{
	LionTbmState st;

	st.tbm = tbm;
	st.firstblk = lion_ckey_first_block(c->ckey);
	st.curblk = InvalidBlockNumber;
	st.ntids = 0;
	st.total = 0;
	st.recheck = recheck;

	if (c->type == LION_CT_SPARSE)
		lion_sparse_iterate(c, lion_tbm_pair_callback, &st);
	else
		lion_container_iterate(c, lion_tbm_callback, &st);

	if (st.ntids > 0)
		tbm_add_tuples(tbm, st.tids, st.ntids, st.recheck);

	return st.total;
}

/*
 * Emit every item of an INLINE entry payload.  Those items are packed
 * without padding, so each one is copied into an aligned buffer first.
 */
static int64
lion_emit_inline(const char *payload, Size paylen, TIDBitmap *tbm, bool recheck)
{
	int64		ntids = 0;
	Size		off = 0;
	LionContainer *cbuf = (LionContainer *) palloc(LION_CONTAINER_MAX_SIZE);

	while (lion_inline_fetch(payload, paylen, &off, cbuf) > 0)
		ntids += lion_container_to_tbm(cbuf, tbm, recheck);

	pfree(cbuf);
	return ntids;
}

IndexScanDesc
lionbeginscan(Relation r, int nkeys, int norderbys)
{
	IndexScanDesc scan;
	LionScanOpaque so;

	scan = RelationGetIndexScan(r, nkeys, norderbys);

	so = (LionScanOpaque) palloc0(sizeof(LionScanOpaqueData));
	so->state = lion_get_state(r);
	so->subtype = InvalidOid;
	so->subhashvalid = false;

	scan->opaque = so;

	return scan;
}

void
lionrescan(IndexScanDesc scan, ScanKey scankey, int nscankeys,
		  ScanKey orderbys, int norderbys)
{
	if (scankey && scan->numberOfKeys > 0)
		memcpy(scan->keyData, scankey,
			   scan->numberOfKeys * sizeof(ScanKeyData));
}

void
lionendscan(IndexScanDesc scan)
{
	LionScanOpaque so = (LionScanOpaque) scan->opaque;

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
lion_emit_chain(Relation index, uint32 hash, BlockNumber head, TIDBitmap *tbm,
			   bool recheck)
{
	PGAlignedBlock *copy;
	BlockNumber blkno = head;
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
		bool		owned;

		buf = ReadBuffer(index, blkno);
		LockBuffer(buf, BUFFER_LOCK_SHARE);
		page = BufferGetPage(buf);
		owned = lion_page_owns_entry(page, hash, head);
		if (owned)
			memcpy(cpage, page, BLCKSZ);
		UnlockReleaseBuffer(buf);

		/*
		 * DESIGN.md §18: a page that no longer claims this chain means the
		 * chain was freed after the entry was copied out, which can only
		 * happen once its posting set held nothing visible to anyone.  The
		 * scan stops there; outside verify() this is never an error.
		 */
		if (!owned)
			break;

		blkno = LionPageGetOpaque(cpage)->rightlink;

		maxoff = PageGetMaxOffsetNumber(cpage);
		for (off = FirstOffsetNumber; off <= maxoff; off++)
		{
			ItemId		iid = PageGetItemId(cpage, off);

			if (!ItemIdIsUsed(iid))
				continue;
			ntids += lion_container_to_tbm((LionContainer *) PageGetItem(cpage, iid),
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
lion_emit_entry(Relation index, Buffer headbuf, Buffer entrybuf,
			   OffsetNumber entryoff, TIDBitmap *tbm, bool recheck)
{
	Page		page = BufferGetPage(entrybuf);
	ItemId		iid = PageGetItemId(page, entryoff);
	LionEntryTuple *entry = (LionEntryTuple *) PageGetItem(page, iid);
	char	   *payload = NULL;
	Size		paylen = 0;
	BlockNumber blkno = InvalidBlockNumber;
	uint32		hash = entry->hash;
	int64		ntids;

	if (entry->flags & LION_ENTRY_INLINE)
	{
		paylen = LION_ENTRY_PAYLOAD_LEN(entry, ItemIdGetLength(iid));
		if (paylen > 0)
		{
			payload = (char *) palloc(paylen);
			memcpy(payload, LionEntryGetPayload(entry), paylen);
		}
	}
	else
	{
		Assert(entry->flags & LION_ENTRY_CHAIN);
		blkno = entry->head;
	}

	if (entrybuf != headbuf)
		UnlockReleaseBuffer(entrybuf);
	UnlockReleaseBuffer(headbuf);

	if (payload != NULL)
	{
		ntids = lion_emit_inline(payload, paylen, tbm, recheck);
		pfree(payload);
		return ntids;
	}

	/*
	 * Test hook: the entry has been copied and its bucket page released, so
	 * all this scan holds of the posting set is a head block - no pin, no
	 * lock, nothing that stops a VACUUM from deleting the entry and freeing
	 * the chain, or an insert from taking those pages back.  What keeps the
	 * walk below correct is the owner check in each page's special area
	 * (DESIGN.md §18); test/isolation/vacuum_chain_reuse.spec parks a reader
	 * here and does exactly that to it.  Compiles to nothing without
	 * --enable-injection-points.
	 */
	INJECTION_POINT("lion-scan-chain-entered", NULL);

	return lion_emit_chain(index, hash, blkno, tbm, recheck);
}

/*
 * Hash a search value.  Cross-type equality (int4 column = int8 constant)
 * needs the hash function of the value's own type, exactly as the hash AM
 * does in _hash_datum2hashkey_type().  subtype is the scan key's sk_subtype,
 * which for an array key is the type of its elements.
 */
static uint32
lion_scankey_hash(IndexScanDesc scan, LionScanOpaque so, Oid subtype,
				 Oid collation, Datum value)
{
	Relation	index = scan->indexRelation;

	if (!OidIsValid(subtype) || subtype == index->rd_opcintype[0])
		return lion_hash_key(so->state, value);

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
lion_emit_value(IndexScanDesc scan, LionScanOpaque so, ScanKey skey, Datum value,
			   TIDBitmap *tbm)
{
	Relation	index = scan->indexRelation;
	LionState   *state = so->state;
	uint32		hash;
	Buffer		headbuf;
	Buffer		entrybuf;
	OffsetNumber entryoff;

	hash = lion_scankey_hash(scan, so, skey->sk_subtype, skey->sk_collation,
							value);

	headbuf = ReadBuffer(index,
						 LION_BUCKET_BLKNO(lion_bucket_of(hash,
														state->meta.nbuckets)));
	LockBuffer(headbuf, BUFFER_LOCK_SHARE);

	if (!lion_find_entry_ext(index, state, headbuf, BUFFER_LOCK_SHARE,
							value, hash, &skey->sk_func, skey->sk_collation,
							&entrybuf, &entryoff))
	{
		UnlockReleaseBuffer(headbuf);
		return 0;
	}

	return lion_emit_entry(index, headbuf, entrybuf, entryoff, tbm, so->recheck);
}

/*
 * `col = ANY (array)` (DESIGN.md §15).  The elements are looked up one at a
 * time and their sets emitted into the same bitmap; NULL elements select
 * nothing, and repeated elements are harmless because a TIDBitmap is a set.
 */
static int64
lion_emit_array(IndexScanDesc scan, LionScanOpaque so, ScanKey skey,
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
		ntids += lion_emit_value(scan, so, skey, elems[i], tbm);
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
lion_emit_null(Relation index, LionState *state, TIDBitmap *tbm, bool recheck)
{
	Buffer		headbuf;
	Buffer		entrybuf;
	OffsetNumber entryoff;

	headbuf = ReadBuffer(index, LION_BUCKET_BLKNO(LION_NULLKEY_BUCKET));
	LockBuffer(headbuf, BUFFER_LOCK_SHARE);

	if (!lion_find_null_entry(index, headbuf, BUFFER_LOCK_SHARE,
							 &entrybuf, &entryoff))
	{
		UnlockReleaseBuffer(headbuf);
		return 0;
	}

	return lion_emit_entry(index, headbuf, entrybuf, entryoff, tbm, recheck);
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
lion_emit_all_keys(Relation index, LionState *state, TIDBitmap *tbm,
				  bool recheck)
{
	PGAlignedBlock *copy = (PGAlignedBlock *) palloc(sizeof(PGAlignedBlock));
	Page		cpage = (Page) copy->data;
	int64		ntids = 0;
	uint32		b;

	for (b = 0; b < state->meta.nbuckets; b++)
	{
		BlockNumber blkno = LION_BUCKET_BLKNO(b);

		while (BlockNumberIsValid(blkno))
		{
			Buffer		buf;
			Page		page;
			OffsetNumber maxoff;
			OffsetNumber off;

			buf = ReadBuffer(index, blkno);
			LockBuffer(buf, BUFFER_LOCK_SHARE);
			page = BufferGetPage(buf);
			if (!LionPageIsBucket(page))
			{
				UnlockReleaseBuffer(buf);
				elog(ERROR, "lion index: block %u is not a bucket page",
					 blkno);
			}
			memcpy(cpage, page, BLCKSZ);
			UnlockReleaseBuffer(buf);

			blkno = LionPageGetOpaque(cpage)->rightlink;
			maxoff = PageGetMaxOffsetNumber(cpage);

			for (off = FirstOffsetNumber; off <= maxoff; off++)
			{
				ItemId		iid = PageGetItemId(cpage, off);
				LionEntryTuple *entry;

				if (!ItemIdIsUsed(iid))
					continue;
				entry = (LionEntryTuple *) PageGetItem(cpage, iid);
				if (LionEntryIsNullKey(entry))
					continue;

				if ((entry->flags & LION_ENTRY_INLINE) != 0)
					ntids += lion_emit_inline(LionEntryGetPayload(entry),
											 LION_ENTRY_PAYLOAD_LEN(entry,
																   ItemIdGetLength(iid)),
											 tbm, recheck);
				else
					ntids += lion_emit_chain(index, entry->hash, entry->head,
											tbm, recheck);

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

/* State threaded through lion_sets_iterate() by lion_emit_query(). */
typedef struct LionQueryEmitState
{
	TIDBitmap  *tbm;
	bool		recheck;
	int64		ntids;
} LionQueryEmitState;

static bool
lion_query_emit_cb(const LionContainer *c, void *arg)
{
	LionQueryEmitState *es = (LionQueryEmitState *) arg;

	es->ntids += lion_container_to_tbm(c, es->tbm, es->recheck);
	return true;
}

/*
 * Answer one multi-key query (`tags @> '{a,b}'`, `tsv @@ 'a & b'`, ...).
 *
 * The opclass's extractQuery function says which keys the query needs and how
 * exactly they answer it; lion_extract_query() turns that into one of three
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
lion_emit_query(Relation index, LionState *state, StrategyNumber strategy,
			   Datum query, TIDBitmap *tbm, bool recheck)
{
	LionQuery	q;
	LionPostingSet *sets;
	LionQueryEmitState es;
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
								"lion index multikey scan",
								ALLOCSET_DEFAULT_SIZES);
	oldcxt = MemoryContextSwitchTo(cxt);

	lion_extract_query(state, query, strategy, &q);

	if (q.mode != LION_QMODE_KEYS)
	{
		int64		ntids = 0;

		MemoryContextSwitchTo(oldcxt);
		if (q.mode == LION_QMODE_ALL)
			ntids = lion_emit_all_keys(index, state, tbm, true);
		MemoryContextDelete(cxt);
		return ntids;
	}

	Assert(q.nkeys > 0 && q.tree != NULL);

	sets = (LionPostingSet *) palloc0(sizeof(LionPostingSet) * q.nkeys);
	for (i = 0; i < q.nkeys; i++)
	{
		(void) lion_posting_set_lookup(index, q.keys[i], InvalidOid, &sets[i]);
		CHECK_FOR_INTERRUPTS();
	}

	es.tbm = tbm;
	es.recheck = recheck;
	es.ntids = 0;

	(void) lion_sets_iterate(q.nkeys, sets, q.tree, lion_query_emit_cb, &es);

	/* Every pin goes before the memory the sets live in does. */
	for (i = 0; i < q.nkeys; i++)
		lion_posting_set_release(&sets[i]);

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
lion_emit_multikey(IndexScanDesc scan, LionScanOpaque so, ScanKey skey,
				  TIDBitmap *tbm)
{
	Relation	index = scan->indexRelation;
	LionState   *state = so->state;

	/*
	 * A strategy this file does not know is an ERROR and not an empty result:
	 * the opclass would be claiming an operator the scan cannot answer, and
	 * answering "no rows" would be a wrong answer rather than a missing
	 * optimisation.  lion_gin_strategy() raises it.
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
			ntids += lion_emit_query(index, state, skey->sk_strategy,
									elems[i], tbm, so->recheck);
			CHECK_FOR_INTERRUPTS();
		}

		pfree(elems);
		pfree(nulls);
		if ((Pointer) arr != DatumGetPointer(skey->sk_argument))
			pfree(arr);

		return ntids;
	}

	return lion_emit_query(index, state, skey->sk_strategy, skey->sk_argument,
						  tbm, so->recheck);
}

int64
liongetbitmap(IndexScanDesc scan, TIDBitmap *tbm)
{
	Relation	index = scan->indexRelation;
	LionScanOpaque so = (LionScanOpaque) scan->opaque;
	LionState   *state;
	ScanKey		skey;

	/*
	 * Re-fetch the cached state: a relcache invalidation since ambeginscan
	 * would have thrown the copy in rd_amcache away.
	 */
	state = so->state = lion_get_state(index);

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
		return lion_emit_null(index, state, tbm, so->recheck);
	if ((skey->sk_flags & SK_SEARCHNOTNULL) != 0)
		return lion_emit_all_keys(index, state, tbm, so->recheck);

	/* `col = NULL` (or a NULL array) is never true. */
	if ((skey->sk_flags & SK_ISNULL) != 0)
		return 0;

	/* A multi-key opclass answers the strategies of DESIGN.md §17. */
	if (state->multikey)
		return lion_emit_multikey(scan, so, skey, tbm);

	if (skey->sk_strategy != LION_STRAT_EQUAL)
		return 0;

	if ((skey->sk_flags & SK_SEARCHARRAY) != 0)
		return lion_emit_array(scan, so, skey, tbm);

	return lion_emit_value(scan, so, skey, skey->sk_argument, tbm);
}
