/*-------------------------------------------------------------------------
 *
 * lion_scan.c
 *		Bitmap scan support for the lion index (DESIGN.md section 5, SCAN).
 *
 * A lion index answers one qual per key column: an equality to a value,
 * `= ANY (array)` (DESIGN.md §15, amsearcharray), `IS NULL` or `IS NOT NULL`
 * (DESIGN.md §14, amsearchnulls).  The scan finds the entries the qual
 * selects and emits their posting sets into the caller's TIDBitmap.
 *
 * A MULTICOLUMN index (DESIGN.md §24) holds each column's keys as an
 * independent set of entries, so a scan key resolves to its column by
 * sk_attno and several columns' answers are INTERSECTED - through the very
 * expression evaluator a multi-key query already uses, because "the sets of
 * a and the sets of b" is an AND of set trees whatever produced them.  A qual
 * the sets cannot express (`IS NOT NULL`, a multi-key query that needs the
 * whole index) is dropped and the TIDs are marked for recheck, which is
 * always correct: the bitmap heap scan re-applies the original quals.
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

/*
 * Per key column: the cross-type support functions resolved for the last scan
 * key seen on that column.  They are cached per column because a multicolumn
 * scan has one scan key per column and they would otherwise evict each other
 * on every rescan (DESIGN.md §24).
 */
typedef struct LionScanCol
{
	Oid			subtype;		/* type of the scan key argument */
	FmgrInfo	subhashproc;	/* hash function for that type */
	FmgrInfo	subcmpproc;		/* ordering of a stored key against it (§21) */
	bool		subcmpvalid;
	bool		subhashvalid;
} LionScanCol;

typedef struct LionScanOpaqueData
{
	LionIndexState *ix;			/* cached relation state */
	LionScanCol *cols;			/* [ix->ncolumns] */
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
	so->ix = lion_get_index_state(r);
	so->cols = (LionScanCol *) palloc0(sizeof(LionScanCol) * so->ix->ncolumns);

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
		if (so->cols != NULL)
			pfree(so->cols);
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
	BlockNumber blkno;
	Buffer		buf;
	int64		ntids = 0;

	if (!BlockNumberIsValid(head))
		return 0;

	/*
	 * DESIGN.md §22: `head` is the ROOT of the posting tree, which is the
	 * single leaf only while the set fits one page.  The leaves are still one
	 * rightlinked list in ckey order, so the walk below is unchanged once it
	 * has descended to the leftmost one - and the descent hands that leaf back
	 * LOCKED, which is what keeps a root push-down from slipping in between
	 * (the block would then be an internal page and this walk would find no
	 * containers on it at all).
	 */
	buf = lion_posting_search(index, NULL, hash, head, 0, BUFFER_LOCK_SHARE,
							  false);
	if (!BufferIsValid(buf))
		return 0;

	copy = (PGAlignedBlock *) palloc(sizeof(PGAlignedBlock));

	for (;;)
	{
		Page		page;
		Page		cpage = (Page) copy->data;
		OffsetNumber maxoff;
		OffsetNumber off;

		page = BufferGetPage(buf);
		memcpy(cpage, page, BLCKSZ);
		UnlockReleaseBuffer(buf);

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

		if (!BlockNumberIsValid(blkno))
			break;

		buf = ReadBuffer(index, blkno);
		LockBuffer(buf, BUFFER_LOCK_SHARE);

		/*
		 * DESIGN.md §18: a page that no longer claims this posting set means
		 * the set was freed after the entry was copied out, which can only
		 * happen once it held nothing visible to anyone.  The scan stops
		 * there; outside verify() this is never an error.  A leaf's rightlink
		 * always names another leaf, so a page at a level above zero is the
		 * same kind of accident and is treated the same way.
		 */
		if (!lion_page_owns_entry(BufferGetPage(buf), hash, head) ||
			!LionPageIsPostingLeaf(BufferGetPage(buf)))
		{
			UnlockReleaseBuffer(buf);
			break;
		}
	}

	pfree(copy);
	return ntids;
}

/*
 * Emit the posting set of the entry at (entrybuf, entryoff), a directory leaf
 * the caller holds SHARE-locked.  It is released here, before any container
 * page is touched (the reader side of the DESIGN.md §11 deadlock rule).
 */
static int64
lion_emit_entry(Relation index, Buffer entrybuf,
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

	UnlockReleaseBuffer(entrybuf);

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
lion_scankey_hash(IndexScanDesc scan, LionScanOpaque so, LionState *col,
				 Oid subtype, Oid collation, Datum value)
{
	Relation	index = scan->indexRelation;
	int			ci = col->attno - 1;
	LionScanCol *sc = &so->cols[ci];

	if (!OidIsValid(subtype) || subtype == index->rd_opcintype[ci])
		return lion_hash_key(col, value);

	if (!sc->subhashvalid || sc->subtype != subtype)
	{
		Oid			hashproc;

		hashproc = get_opfamily_proc(index->rd_opfamily[ci],
									 subtype, subtype, 1);
		if (!OidIsValid(hashproc))
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_FUNCTION),
					 errmsg("missing support function 1 for type %s in operator family \"%s\"",
							format_type_be(subtype),
							get_opfamily_name(index->rd_opfamily[ci], false))));
		fmgr_info(hashproc, &sc->subhashproc);
		sc->subtype = subtype;
		sc->subhashvalid = true;
		sc->subcmpvalid = false;
	}

	return DatumGetUInt32(FunctionCall1Coll(&sc->subhashproc, collation,
											value));
}

/*
 * The ordering of a STORED key against a search value of subtype: support
 * proc 4 of the opfamily for (opcintype, subtype), DESIGN.md §21.  NULL means
 * the family has none, and lion_dir_find() then falls back to walking the
 * leaves.
 */
static FmgrInfo *
lion_scankey_cmp(IndexScanDesc scan, LionScanOpaque so, LionState *col,
				Oid subtype)
{
	Relation	index = scan->indexRelation;
	int			ci = col->attno - 1;
	LionScanCol *sc = &so->cols[ci];

	if (!OidIsValid(subtype) || subtype == index->rd_opcintype[ci])
		return col->ordered ? &col->cmpproc : NULL;

	if (!sc->subcmpvalid)
	{
		Oid			cmpproc = get_opfamily_proc(index->rd_opfamily[ci],
												index->rd_opcintype[ci],
												subtype, LION_CMP_PROC);

		if (!OidIsValid(cmpproc))
			return NULL;
		fmgr_info(cmpproc, &sc->subcmpproc);
		sc->subcmpvalid = true;
	}

	return &sc->subcmpproc;
}

/*
 * Emit the posting set of one search value on one key column.
 */
static int64
lion_emit_value(IndexScanDesc scan, LionScanOpaque so, LionState *col,
			   ScanKey skey, Datum value, TIDBitmap *tbm)
{
	Relation	index = scan->indexRelation;
	uint32		hash;
	Buffer		entrybuf;
	OffsetNumber entryoff;

	hash = lion_scankey_hash(scan, so, col, skey->sk_subtype,
							skey->sk_collation, value);

	if (!lion_find_entry_ext(index, col, BUFFER_LOCK_SHARE, value, hash,
							&skey->sk_func,
							lion_scankey_cmp(scan, so, col, skey->sk_subtype),
							skey->sk_collation, &entrybuf, &entryoff))
	{
		if (BufferIsValid(entrybuf))
			UnlockReleaseBuffer(entrybuf);
		return 0;
	}

	return lion_emit_entry(index, entrybuf, entryoff, tbm, so->recheck);
}

/*
 * `col = ANY (array)` (DESIGN.md §15).  The elements are looked up one at a
 * time and their sets emitted into the same bitmap; NULL elements select
 * nothing, and repeated elements are harmless because a TIDBitmap is a set.
 */
static int64
lion_emit_array(IndexScanDesc scan, LionScanOpaque so, LionState *col,
			   ScanKey skey, TIDBitmap *tbm)
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
		ntids += lion_emit_value(scan, so, col, skey, elems[i], tbm);
		CHECK_FOR_INTERRUPTS();
	}

	pfree(elems);
	pfree(nulls);
	if ((Pointer) arr != DatumGetPointer(skey->sk_argument))
		pfree(arr);

	return ntids;
}

/*
 * `col IS NULL` (DESIGN.md §14): that column's reserved NULL entry.
 */
static int64
lion_emit_null(Relation index, LionState *col, TIDBitmap *tbm, bool recheck)
{
	Buffer		entrybuf;
	OffsetNumber entryoff;

	if (!lion_find_null_entry(index, col, BUFFER_LOCK_SHARE,
							 &entrybuf, &entryoff))
	{
		if (BufferIsValid(entrybuf))
			UnlockReleaseBuffer(entrybuf);
		return 0;
	}

	return lion_emit_entry(index, entrybuf, entryoff, tbm, recheck);
}

/*
 * Every row indexed under one key column: the union of every entry of that
 * column but its NULL one, which means walking the column's whole run of
 * entries.  Correct, and expensive in proportion to the number of distinct
 * keys - the planner's cost estimate for such a scan is the whole index, so
 * it only happens when that is cheap or when nothing else can answer the
 * query.
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
 * The walk is the column's first leaf and then the right links (DESIGN.md §21
 * and §24), so for an ordered opclass the entries come out in key order, and
 * it stops at the first entry of the NEXT column.  Each leaf is copied into
 * backend-local memory and released before its entries are emitted, so no
 * directory page is held while container pages are pinned (DESIGN.md §11).
 * Working from the copy can miss an entry added after the copy was taken,
 * which is exactly as acceptable as it is for a single key: such an entry can
 * only hold TIDs that no snapshot older than this scan can see.  A concurrent
 * split moves entries only to a page further right, which this walk has not
 * passed yet, so nothing is seen twice either.
 */
static int64
lion_emit_all_keys_ext(Relation index, LionState *col, TIDBitmap *tbm,
					  bool recheck, bool withnull)
{
	PGAlignedBlock *copy = (PGAlignedBlock *) palloc(sizeof(PGAlignedBlock));
	Page		cpage = (Page) copy->data;
	BlockNumber blkno = lion_dir_column_first(index, col, NULL);
	int64		ntids = 0;
	bool		done = false;

	while (!done && BlockNumberIsValid(blkno))
	{
		Buffer		buf;
		Page		page;
		OffsetNumber maxoff;
		OffsetNumber off;

		buf = ReadBuffer(index, blkno);
		lion_dir_pages_read++;
		LockBuffer(buf, BUFFER_LOCK_SHARE);
		page = BufferGetPage(buf);
		if (!LionPageIsLeaf(page))
		{
			UnlockReleaseBuffer(buf);
			elog(ERROR, "lion index: block %u is not a directory leaf", blkno);
		}
		memcpy(cpage, page, BLCKSZ);
		UnlockReleaseBuffer(buf);

		blkno = LionPageGetOpaque(cpage)->rightlink;
		maxoff = PageGetMaxOffsetNumber(cpage);

		for (off = lion_page_first_data(cpage); off <= maxoff; off++)
		{
			ItemId		iid = PageGetItemId(cpage, off);
			LionEntryTuple *entry;

			if (!ItemIdIsUsed(iid))
				continue;
			entry = (LionEntryTuple *) PageGetItem(cpage, iid);

			/* Bounded to one key column (DESIGN.md §24). */
			if (entry->attno < col->attno)
				continue;
			if (entry->attno > col->attno)
			{
				done = true;
				break;
			}

			if (LionEntryIsNullKey(entry) && !withnull)
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

	pfree(copy);
	return ntids;
}

static int64
lion_emit_all_keys(Relation index, LionState *col, TIDBitmap *tbm,
				  bool recheck)
{
	return lion_emit_all_keys_ext(index, col, tbm, recheck, false);
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
 * the reader side of the DESIGN.md §11 deadlock rule: every directory-page
 * lock this function takes is taken before the first container page is
 * pinned.
 */
static int64
lion_emit_query(Relation index, LionState *col, StrategyNumber strategy,
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

	lion_extract_query(col, query, strategy, &q);

	if (q.mode != LION_QMODE_KEYS)
	{
		int64		ntids = 0;

		MemoryContextSwitchTo(oldcxt);
		if (q.mode == LION_QMODE_ALL)
			ntids = lion_emit_all_keys(index, col, tbm, true);
		MemoryContextDelete(cxt);
		return ntids;
	}

	Assert(q.nkeys > 0 && q.tree != NULL);

	sets = (LionPostingSet *) palloc0(sizeof(LionPostingSet) * q.nkeys);
	for (i = 0; i < q.nkeys; i++)
	{
		(void) lion_posting_set_lookup_col(index, (AttrNumber) col->attno,
										  q.keys[i], InvalidOid, &sets[i]);
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
lion_emit_multikey(IndexScanDesc scan, LionScanOpaque so, LionState *col,
				  ScanKey skey, TIDBitmap *tbm)
{
	Relation	index = scan->indexRelation;

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
			ntids += lion_emit_query(index, col, skey->sk_strategy,
									elems[i], tbm, so->recheck);
			CHECK_FOR_INTERRUPTS();
		}

		pfree(elems);
		pfree(nulls);
		if ((Pointer) arr != DatumGetPointer(skey->sk_argument))
			pfree(arr);

		return ntids;
	}

	return lion_emit_query(index, col, skey->sk_strategy, skey->sk_argument,
						  tbm, so->recheck);
}

/* ---------------------------------------------------------------------
 * Several key columns at once (DESIGN.md §24)
 *
 * Each column's qual becomes a boolean TREE over posting sets, all of them
 * located into one array, and the columns are ANDed together.  That is the
 * very shape a multi-key query already produces (§17), so the evaluator of
 * lion_count.c answers both without knowing which is which.
 *
 * Every set is located BEFORE the merge starts, which is the reader side of
 * the §11 rule: the directory locks are all taken and released before the
 * first container page is pinned.
 * --------------------------------------------------------------------- */

typedef struct LionScanSets
{
	LionPostingSet *sets;
	int			nsets;
	int			maxsets;
} LionScanSets;

static int
lion_sets_reserve(LionScanSets *acc, int n)
{
	int			first = acc->nsets;

	if (acc->nsets + n > acc->maxsets)
	{
		acc->maxsets = Max(acc->maxsets * 2, acc->nsets + n);
		acc->sets = (LionPostingSet *)
			repalloc(acc->sets, sizeof(LionPostingSet) * acc->maxsets);
	}
	memset(&acc->sets[first], 0, sizeof(LionPostingSet) * n);
	acc->nsets += n;

	return first;
}

/* A leaf naming the set at absolute position keyno. */
static LionKeyNode *
lion_scan_leaf(int keyno)
{
	LionKeyNode *n = (LionKeyNode *) palloc0(sizeof(LionKeyNode));

	n->kind = LION_KN_KEY;
	n->keyno = keyno;
	return n;
}

static LionKeyNode *
lion_scan_op(LionKeyNodeKind kind, LionKeyNode **args, int nargs)
{
	LionKeyNode *n;

	if (nargs == 1)
		return args[0];

	n = (LionKeyNode *) palloc0(sizeof(LionKeyNode));
	n->kind = kind;
	n->nargs = nargs;
	n->args = args;
	return n;
}

/*
 * Renumber a tree lion_extract_query() built over keys 0 .. n-1 so that its
 * leaves name sets at base .. base + n - 1 of the shared array.
 */
static void
lion_scan_shift(LionKeyNode *node, int base)
{
	int			i;

	if (node == NULL)
		return;
	if (node->kind == LION_KN_KEY)
	{
		node->keyno += base;
		return;
	}
	for (i = 0; i < node->nargs; i++)
		lion_scan_shift(node->args[i], base);
}

/*
 * The set tree of ONE column's scan key, with its sets appended to *acc.
 *
 * Returns NULL and leaves *ok false when the qual cannot be expressed as a
 * tree over posting sets at all (`IS NOT NULL`, a multi-key query that needs
 * every row): the caller then drops the qual and rechecks.  *nomatch means
 * the qual selects nothing whatsoever, which settles the entire scan.
 */
static LionKeyNode *
lion_scan_col_tree(IndexScanDesc scan, LionScanOpaque so, LionState *col,
				  ScanKey skey, LionScanSets *acc, bool *ok, bool *nomatch)
{
	Relation	index = scan->indexRelation;
	AttrNumber	attno = (AttrNumber) col->attno;

	*ok = true;
	*nomatch = false;

	/* The null tests carry no strategy number and must be tested first. */
	if ((skey->sk_flags & SK_SEARCHNULL) != 0)
	{
		int			base = lion_sets_reserve(acc, 1);

		if (!lion_posting_set_lookup_null_col(index, attno, &acc->sets[base]))
			*nomatch = true;
		return lion_scan_leaf(base);
	}
	if ((skey->sk_flags & SK_SEARCHNOTNULL) != 0)
	{
		/* The complement of a set is not a set the evaluator can build. */
		*ok = false;
		return NULL;
	}

	/* `col = NULL` (or a NULL array) is never true. */
	if ((skey->sk_flags & SK_ISNULL) != 0)
	{
		*nomatch = true;
		return NULL;
	}

	if (col->multikey)
	{
		Datum	   *queries;
		bool	   *qnulls = NULL;
		int			nqueries = 1;
		LionKeyNode **args;
		int			nargs = 0;
		int			i;

		if ((skey->sk_flags & SK_SEARCHARRAY) != 0)
		{
			ArrayType  *arr = DatumGetArrayTypeP(skey->sk_argument);
			Oid			elemtype = ARR_ELEMTYPE(arr);
			int16		elmlen;
			bool		elmbyval;
			char		elmalign;

			get_typlenbyvalalign(elemtype, &elmlen, &elmbyval, &elmalign);
			deconstruct_array(arr, elemtype, elmlen, elmbyval, elmalign,
							  &queries, &qnulls, &nqueries);
		}
		else
		{
			queries = (Datum *) palloc(sizeof(Datum));
			queries[0] = skey->sk_argument;
		}

		args = (LionKeyNode **) palloc(sizeof(LionKeyNode *) *
									   Max(nqueries, 1));

		for (i = 0; i < nqueries; i++)
		{
			LionQuery	q;
			int			base;
			int			k;

			if (qnulls != NULL && qnulls[i])
				continue;		/* a strict operator with a NULL is not true */

			lion_extract_query(col, queries[i], skey->sk_strategy, &q);

			if (q.mode == LION_QMODE_NONE)
				continue;		/* this element selects nothing */
			if (q.mode != LION_QMODE_KEYS)
			{
				*ok = false;	/* needs every indexed row */
				return NULL;
			}

			base = lion_sets_reserve(acc, q.nkeys);
			for (k = 0; k < q.nkeys; k++)
			{
				(void) lion_posting_set_lookup_col(index, attno, q.keys[k],
												  InvalidOid,
												  &acc->sets[base + k]);
				CHECK_FOR_INTERRUPTS();
			}
			lion_scan_shift(q.tree, base);
			args[nargs++] = q.tree;
		}

		if (nargs == 0)
		{
			*nomatch = true;	/* every element selects nothing */
			return NULL;
		}

		/* `op ANY (array)` is the UNION of the elements' answers. */
		return lion_scan_op(LION_KN_OR, args, nargs);
	}

	if (skey->sk_strategy != LION_STRAT_EQUAL)
	{
		*ok = false;
		return NULL;
	}

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
		int			nsets;
		int			nfound;
		int			base;
		LionKeyNode **args;
		int			i;

		get_typlenbyvalalign(elemtype, &elmlen, &elmbyval, &elmalign);
		deconstruct_array(arr, elemtype, elmlen, elmbyval, elmalign,
						  &elems, &nulls, &nelems);

		if (nelems == 0)
		{
			*nomatch = true;
			return NULL;
		}

		base = lion_sets_reserve(acc, nelems);
		nsets = lion_posting_set_lookup_many_col(index, attno, elemtype,
												nelems, elems, nulls,
												&acc->sets[base], &nfound);
		/* The lookup packs what it located at the front; drop the rest. */
		acc->nsets = base + nsets;
		if (nfound == 0)
		{
			*nomatch = true;
			return NULL;
		}

		args = (LionKeyNode **) palloc(sizeof(LionKeyNode *) * Max(nsets, 1));
		for (i = 0; i < nsets; i++)
			args[i] = lion_scan_leaf(base + i);

		return lion_scan_op(LION_KN_OR, args, nsets);
	}

	{
		int			base = lion_sets_reserve(acc, 1);

		if (!lion_posting_set_lookup_col(index, attno, skey->sk_argument,
										skey->sk_subtype, &acc->sets[base]))
			*nomatch = true;
		return lion_scan_leaf(base);
	}
}

/*
 * Answer a scan whose keys span SEVERAL key columns: intersect the columns'
 * set trees (DESIGN.md §24).  Any column whose qual cannot be expressed is
 * dropped and *recheck is set, which is correct because the bitmap heap scan
 * re-applies the original quals.  Returns -1 when NOTHING could be expressed,
 * so the caller falls back to a single-column answer.
 */
static int64
lion_emit_columns(IndexScanDesc scan, LionScanOpaque so, ScanKey *keys,
				 int nkeys, TIDBitmap *tbm)
{
	LionScanSets acc;
	LionKeyNode **args;
	LionKeyNode *tree;
	LionQueryEmitState es;
	MemoryContext cxt;
	MemoryContext oldcxt;
	int			nargs = 0;
	int			i;
	int			j;

	cxt = AllocSetContextCreate(CurrentMemoryContext,
								"lion index multicolumn scan",
								ALLOCSET_DEFAULT_SIZES);
	oldcxt = MemoryContextSwitchTo(cxt);

	acc.maxsets = 8;
	acc.nsets = 0;
	acc.sets = (LionPostingSet *) palloc0(sizeof(LionPostingSet) * acc.maxsets);
	args = (LionKeyNode **) palloc(sizeof(LionKeyNode *) * nkeys);

	for (i = 0; i < nkeys; i++)
	{
		LionState  *col = lion_column(so->ix, keys[i]->sk_attno);
		LionKeyNode *node;
		bool		ok;
		bool		nomatch;

		node = lion_scan_col_tree(scan, so, col, keys[i], &acc, &ok, &nomatch);

		if (nomatch)
		{
			/* One column selects nothing, so the intersection does too. */
			for (j = 0; j < acc.nsets; j++)
				lion_posting_set_release(&acc.sets[j]);
			MemoryContextSwitchTo(oldcxt);
			MemoryContextDelete(cxt);
			return 0;
		}
		if (!ok || node == NULL)
		{
			so->recheck = true;	/* the heap scan re-applies this qual */
			continue;
		}
		args[nargs++] = node;
	}

	if (nargs == 0)
	{
		for (i = 0; i < acc.nsets; i++)
			lion_posting_set_release(&acc.sets[i]);
		MemoryContextSwitchTo(oldcxt);
		MemoryContextDelete(cxt);
		return -1;
	}

	tree = lion_scan_op(LION_KN_AND, args, nargs);

	es.tbm = tbm;
	es.recheck = so->recheck;
	es.ntids = 0;

	(void) lion_sets_iterate(acc.nsets, acc.sets, tree, lion_query_emit_cb,
							 &es);

	for (i = 0; i < acc.nsets; i++)
		lion_posting_set_release(&acc.sets[i]);

	MemoryContextSwitchTo(oldcxt);
	MemoryContextDelete(cxt);

	return es.ntids;
}

int64
liongetbitmap(IndexScanDesc scan, TIDBitmap *tbm)
{
	Relation	index = scan->indexRelation;
	LionScanOpaque so = (LionScanOpaque) scan->opaque;
	ScanKey		best[INDEX_MAX_KEYS];
	int			bestrank[INDEX_MAX_KEYS];
	int			ncols;
	int			nchosen = 0;
	int			ndropped = 0;
	int			i;
	LionState  *col;
	ScanKey		skey;

	/*
	 * Re-fetch the cached state: a relcache invalidation since ambeginscan
	 * would have thrown the copy in rd_amcache away.
	 */
	so->ix = lion_get_index_state(index);
	ncols = so->ix->ncolumns;

	pgstat_count_index_scan(index);
	if (scan->instrument)
		scan->instrument->nsearches++;

	/*
	 * No scan key at all: a PARTIAL index whose predicate the query implies
	 * (amoptionalkey, DESIGN.md §24).  The answer is every row the index
	 * holds, which is the union of every entry of ANY ONE key column - the
	 * NULL entry included this time, because a row whose value is NULL is
	 * still an indexed row.  No recheck: these TIDs are exactly the rows the
	 * scan selects.
	 */
	if (scan->numberOfKeys < 1)
	{
		so->recheck = false;
		return lion_emit_all_keys_ext(index, lion_column(so->ix, 1), tbm,
									 false, true);
	}

	/*
	 * ONE qual per key column is answered (DESIGN.md §24).  The planner may
	 * hand a column more than one (`b = ANY (x) AND b = ANY (y)`, or an
	 * equality next to a null test); the most selective-looking one is
	 * answered and the rest are left to the heap recheck, which is correct
	 * because the others only shrink the result.  The ranking is a plain
	 * equality first, then a list, then a null test - and it is mirrored in
	 * lion_scan_walks_whole_index() so that the path is priced as the path it
	 * takes.
	 */
	for (i = 0; i < ncols; i++)
	{
		best[i] = NULL;
		bestrank[i] = 3;
	}

	for (i = 0; i < scan->numberOfKeys; i++)
	{
		ScanKey		k = &scan->keyData[i];
		int			ci = k->sk_attno - 1;
		int			rank;

		if (k->sk_attno < 1 || k->sk_attno > ncols)
			continue;			/* not a key column of this index */

		if ((k->sk_flags & (SK_SEARCHNULL | SK_SEARCHNOTNULL)) != 0)
			rank = 2;
		else if ((k->sk_flags & SK_SEARCHARRAY) != 0)
			rank = 1;
		else
			rank = 0;

		if (best[ci] == NULL)
			nchosen++;
		else
			ndropped++;

		if (rank < bestrank[ci])
		{
			bestrank[ci] = rank;
			best[ci] = k;
		}
	}

	if (nchosen == 0)
		return 0;

	so->recheck = (ndropped > 0);

	/*
	 * Several columns: intersect their set trees.  A column whose qual the
	 * sets cannot express is dropped there and rechecked; when NONE of them
	 * can be expressed the answer falls through to the single-column path
	 * below, which knows how to walk a whole column.
	 */
	if (nchosen > 1)
	{
		ScanKey		chosen[INDEX_MAX_KEYS];
		int			n = 0;
		int64		ntids;

		for (i = 0; i < ncols; i++)
		{
			if (best[i] != NULL)
				chosen[n++] = best[i];
		}

		ntids = lion_emit_columns(scan, so, chosen, n, tbm);
		if (ntids >= 0)
			return ntids;

		/* Nothing was expressible: answer the first column and recheck. */
		so->recheck = true;
	}

	for (i = 0; i < ncols; i++)
	{
		if (best[i] != NULL)
			break;
	}
	Assert(i < ncols);
	skey = best[i];
	col = lion_column(so->ix, skey->sk_attno);

	/* The null tests carry no strategy number and must be tested first. */
	if ((skey->sk_flags & SK_SEARCHNULL) != 0)
		return lion_emit_null(index, col, tbm, so->recheck);
	if ((skey->sk_flags & SK_SEARCHNOTNULL) != 0)
		return lion_emit_all_keys(index, col, tbm, so->recheck);

	/* `col = NULL` (or a NULL array) is never true. */
	if ((skey->sk_flags & SK_ISNULL) != 0)
		return 0;

	/* A multi-key opclass answers the strategies of DESIGN.md §17. */
	if (col->multikey)
		return lion_emit_multikey(scan, so, col, skey, tbm);

	if (skey->sk_strategy != LION_STRAT_EQUAL)
		return 0;

	if ((skey->sk_flags & SK_SEARCHARRAY) != 0)
		return lion_emit_array(scan, so, col, skey, tbm);

	return lion_emit_value(scan, so, col, skey, skey->sk_argument, tbm);
}
