/*-------------------------------------------------------------------------
 *
 * lion_posfilter.c
 *		The position filter (DESIGN.md §17, "Stored positions"): a tsquery
 *		answered exactly from the positions a column stores.
 *
 * A query the posting sets only bound - a phrase, a weight, `a & !b` - is
 * answered in two steps.  lion_extract_query_superset() picks the candidates,
 * the rows of a boolean tree over the query's keys, and this file decides
 * each one with PostgreSQL's own TS_execute(), whose callback reads the
 * candidate's positions under each lexeme from the index instead of from the
 * heap's tsvector.  The callback is checkclass_str()'s (tsvector_op.c) over
 * those positions, stripped members included, so the answer is the one the
 * operator gives for the row: phrase distances, weights, NOT and the 16,383
 * position cap are core's rules, not ours.
 *
 * ONE CURSOR PER KEY.  Every lexeme the query names - those under a NOT too,
 * which the superset leaves out - gets a cursor over the key's members in
 * TID order: the INLINE entry's chunk, copied when the filter begins, or the
 * key's position tree, read a leaf at a time.  Candidates ascend, so each
 * cursor only moves forward: a seek decodes members until it reaches the
 * candidate's code, skipping the chunks that cannot hold its block (P2 of
 * lion_postree.c), and a leaf it runs off is followed by its right link, or
 * by a new descent when the candidate is past that leaf too.
 *
 * ABSENCE IS AUTHORITATIVE.  Every TID of a posting set has positions under
 * its key (P ⊇ C), so a candidate with no member under a lexeme is a row
 * without that lexeme.  The extra members P may hold belong to dead rows.
 *
 * WHAT A READER RELIES ON (the design note's R3).  A leaf is read only under
 * a SHARE lock, and copied whole before it is used; no position page is
 * pinned past that.  The posting sets and the positions are copied at
 * different moments, so a TID that VACUUM removes, and the heap gives to a
 * new row, in between can be judged from a mix of the two rows' lexemes.
 * That changes no answer: VACUUM removes the old row's positions before the
 * heap may reuse its TID, so the new row is inserted after the scan's
 * snapshot was taken and is invisible to the scan - and its page is not
 * all-visible to it either, so a count asks the heap about it too.  A bitmap
 * heap scan checks every TID's visibility anyway.  The count pushdown also
 * reads a container's positions after copying it and while its pages are
 * pinned (lion_count_container_masks()), the design note's R1 and R2.
 *
 * A filter is reused: a count per group or per join key walks the same
 * candidates again, and a container below where the cursors stand rewinds
 * them.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "storage/bufmgr.h"
#include "storage/bufpage.h"
#include "tsearch/ts_type.h"
#include "tsearch/ts_utils.h"
#include "utils/datum.h"
#include "utils/memutils.h"
#include "utils/rel.h"

#include "lion.h"
#include "lion_funcs.h"
#include "lion_positions.h"

/* A forward reader over one key's members. */
typedef struct LionPosCursor
{
	bool		absent;			/* the key has no members at all */
	bool		done;			/* no member left at or after the last seek */

	/* where the members are: an INLINE chunk, or a position tree */
	LionContainer *inl;
	uint32		hash;
	BlockNumber root;
	bool		flat;			/* the tree had no internal level when first
								 * descended: never descend it again */

	/* the leaf being read, a private copy, and its chunks */
	char	   *leaf;			/* BLCKSZ bytes, position trees only */
	BlockNumber leafblk;
	BlockNumber rightlink;
	bool		rightmost;
	LionContainer **chunks;
	int			nchunks;
	int			chunkno;		/* the chunk it is in, or about to enter */

	LionPosIter it;
	bool		itvalid;		/* it reads chunks[chunkno] */
	bool		mvalid;			/* m is the first member at or after the last
								 * seek's code */
	bool		countsonly;		/* m gets its code and npos, no positions */
	LionPosMember m;
	int64		ndecoded;		/* members decoded, rewinds and all */
} LionPosCursor;

/* A position tree cursor's pointers to the items of its leaf, as many as fit. */
#define LION_POSCURSOR_CHUNKS_BYTES	(sizeof(LionContainer *) * MaxOffsetNumber)

/*
 * The sizes of a filter's own memory contexts.  Everything the filter holds
 * is in them, and comes to what lion_posfilter_mem() charges at most,
 * whatever context the filter is begun in and whatever the backend did
 * before.  They are not ALLOCSET_SMALL_SIZES or ALLOCSET_DEFAULT_SIZES on
 * purpose: aset.c keeps a deleted context of either first size for the next
 * one created with it, and the context it hands back keeps the limit below
 * which a chunk shares a block, which its first owner derived from its own
 * largest block.  One begun as ALLOCSET_START_SMALL_SIZES, whose blocks grow
 * to 8 MB, leaves a limit of 8 kB to the small context that takes it over,
 * whose blocks stop at 8 kB; a leaf of BLCKSZ bytes then took a block of
 * 16 kB, where elsewhere it is a block of its own, BLCKSZ and the headers.
 * So one session's filter of a thousand lexemes held 33 MB where another's
 * held 25 MB (2026-10 review).  No context of these sizes is kept, so each
 * derives its own limit, 512 bytes.  Every buffer of a cursor, the member
 * arrays, and an array of the query's size past that limit are blocks of
 * their own: the MAXALIGNed size and a block's and a chunk's header, 48
 * bytes from 16 on - 56 in an assert build, whose chunk header is 16 bytes
 * and whose sentinel byte can take one MAXALIGN step more, 64 in all
 * (LION_POSFILTER_BLOCK_OVERHEAD).  The few smaller allocations there can be
 * - the filter, a copy of the query, itemcur and the cursors, one each - fit
 * the first block beside the context's header.
 */
#define LION_POSFILTER_CXT_SIZES	0, 4096, 4096
#define LION_POSFILTER_CXT_FIRST	4096
#define LION_POSFILTER_BLOCK_OVERHEAD	64

/* What an allocation of size bytes costs a filter's context, at most. */
static inline Size
lion_posfilter_alloc_cost(Size size)
{
	return MAXALIGN(size) + LION_POSFILTER_BLOCK_OVERHEAD;
}

struct LionPosFilter
{
	MemoryContext hold;			/* everything below, and this, is in it */
	Relation	index;
	TSQuery		query;
	QueryItem  *items;
	int		   *itemcur;		/* items[i] (a QI_VAL) reads cursors[itemcur[i]] */
	LionPosCursor *cursors;
	int			ncursors;
	uint64		code;			/* the candidate being decided */
	uint64		nextcode;		/* the cursors stand at or before this code */
	uint16	   *los;			/* LION_CONTAINER_RANGE members of a container */
	uint16	   *keep;			/* ... and those lion_posfilter_apply() keeps */
	LionContainer *work;		/* what lion_posfilter_apply() hands back */
	MemoryContext cxt;			/* reset per candidate: what TS_execute() and
								 * the callback allocate; a child of hold */
	int64		nchecked;
	int64		nremoved;
	int64		nseeks;			/* a candidate's member sought under a lexeme */
};

/* ---------------------------------------------------------------------
 * Cursors
 * --------------------------------------------------------------------- */

/*
 * Copy the leaf buf holds, locked SHARE, into the cursor and release it.
 * False when the page is no longer a leaf of the tree: the key's tree was
 * freed, every row it described dead (lion_vacuum.c frees a tree only with
 * its posting set).
 */
static bool
lion_poscursor_take_leaf(Relation index, LionPosCursor *cur, Buffer buf)
{
	Page		page = BufferGetPage(buf);
	OffsetNumber maxoff;
	OffsetNumber off;

	if (!lion_page_owns_positions(page, cur->root) ||
		!LionPageIsPostingLeaf(page))
	{
		UnlockReleaseBuffer(buf);
		return false;
	}
	memcpy(cur->leaf, page, BLCKSZ);
	cur->leafblk = BufferGetBlockNumber(buf);
	UnlockReleaseBuffer(buf);

	page = (Page) cur->leaf;
	cur->rightmost = LionPageIsRightmost(page);
	cur->rightlink = LionPageGetOpaque(page)->rightlink;
	maxoff = PageGetMaxOffsetNumber(page);
	cur->nchunks = 0;
	for (off = FirstOffsetNumber; off <= maxoff; off++)
		cur->chunks[cur->nchunks++] =
			lion_page_poschunk_fetch(index, page, cur->leafblk, off);
	cur->chunkno = 0;
	cur->itvalid = false;
	cur->mvalid = false;
	return true;
}

/* Descend to the leaf where the members of block start (lion_postree.c). */
static bool
lion_poscursor_descend(Relation index, LionPosCursor *cur, uint32 block)
{
	Buffer		buf;

	bool		flat = false;

	buf = lion_posting_search_ext(index, NULL, cur->hash, cur->root, block,
								  BUFFER_LOCK_SHARE, false, true, &flat);
	cur->flat = cur->flat || flat;
	if (!BufferIsValid(buf))
		return false;
	if (BufferGetBlockNumber(buf) == cur->leafblk)
	{
		/* where it already stands: keep its place in the leaf */
		UnlockReleaseBuffer(buf);
		return true;
	}
	return lion_poscursor_take_leaf(index, cur, buf);
}

/*
 * The leaf after the one the cursor ran off.  The right link, unless that
 * leaf ends below block too and is not the last - then the candidate is
 * further right than one step, and a descent goes straight to it.  A tree
 * without an internal level (cur->flat, DESIGN.md §17) has nothing to
 * descend through: its leaves are stepped over instead, each one whose
 * members all lie below block (maxckey) skipped undecoded.
 */
static bool
lion_poscursor_next_leaf(Relation index, LionPosCursor *cur, uint32 block)
{
	Buffer		buf;
	LionRightWalk walk;

	lion_rightwalk_init(&walk);
	for (;;)
	{
		if (cur->rightmost || !BlockNumberIsValid(cur->rightlink))
			return false;
		lion_rightwalk_step(index, &walk, cur->leafblk, cur->rightlink);
		buf = ReadBuffer(index, cur->rightlink);
		LockBuffer(buf, BUFFER_LOCK_SHARE);
		if (!lion_poscursor_take_leaf(index, cur, buf))
			return false;
		if (cur->rightmost || cur->nchunks == 0 ||
			cur->chunks[cur->nchunks - 1]->ckey >= block)
			return true;
		if (!cur->flat)
			return lion_poscursor_descend(index, cur, block);
		if (LionPageGetOpaque((Page) cur->leaf)->maxckey >= block)
			return true;
		CHECK_FOR_INTERRUPTS();
	}
}

/* Back to the key's first member, for a walk that starts again. */
static void
lion_poscursor_rewind(LionPosCursor *cur)
{
	cur->mvalid = false;
	cur->itvalid = false;
	cur->chunkno = 0;
	if (cur->inl == NULL)
	{
		cur->leafblk = InvalidBlockNumber;
		cur->nchunks = 0;
	}
	cur->done = cur->absent;
}

/*
 * Move the cursor to the first member at or after code, and say whether that
 * member is code's.  Codes only ever grow from one seek to the next, until
 * the cursor is rewound.
 */
static bool
lion_poscursor_seek(Relation index, LionPosCursor *cur, uint64 code)
{
	uint32		block = lion_pos_block(code);

	if (cur->mvalid && cur->m.code >= code)
		return cur->m.code == code;
	cur->mvalid = false;
	if (cur->done)
		return false;

	if (cur->inl == NULL && !BlockNumberIsValid(cur->leafblk))
	{
		if (!lion_poscursor_descend(index, cur, block))
		{
			cur->done = true;
			return false;
		}
	}

	for (;;)
	{
		/*
		 * The rest of the chunk the cursor reads is skipped undecoded, as the
		 * chunks after it are below, when the next chunk's header is below
		 * block: by (P2) none of its members reaches past that header.  A
		 * cursor that stands where the last candidate was would otherwise
		 * decode on to the end of the chunk before it skipped a single one -
		 * half a chunk for every candidate, where the candidates are a few
		 * rows of every part of the heap, as a count per key of a join or
		 * per group takes them.
		 */
		if (cur->itvalid && cur->chunkno + 1 < cur->nchunks &&
			cur->chunks[cur->chunkno + 1]->ckey < block)
		{
			cur->itvalid = false;
			cur->chunkno++;
		}

		if (cur->itvalid)
		{
			while (cur->countsonly ?
				   lion_poschunk_iter_next_npos(&cur->it, &cur->m) :
				   lion_poschunk_iter_next(&cur->it, &cur->m))
			{
				cur->ndecoded++;
				if (cur->m.code >= code)
				{
					cur->mvalid = true;
					return cur->m.code == code;
				}
			}
			cur->itvalid = false;
			cur->chunkno++;
		}

		/*
		 * By (P2) a member of block is in the last chunk whose header is
		 * below it or in one whose header is block: every chunk before that
		 * last one is skipped undecoded, and a header above block means the
		 * candidate has no member here.
		 */
		while (cur->chunkno < cur->nchunks)
		{
			if (cur->chunks[cur->chunkno]->ckey > block)
				return false;
			if (cur->chunkno + 1 < cur->nchunks &&
				cur->chunks[cur->chunkno + 1]->ckey < block)
			{
				cur->chunkno++;
				continue;
			}
			lion_poschunk_iter_init(&cur->it, cur->chunks[cur->chunkno]);
			cur->itvalid = true;
			break;
		}
		if (cur->itvalid)
			continue;

		if (cur->inl != NULL || !lion_poscursor_next_leaf(index, cur, block))
		{
			cur->done = true;
			return false;
		}
		CHECK_FOR_INTERRUPTS();
	}
}

/*
 * Locate key's positions.  False when the entry does not say where they are -
 * an index whose entries predate them should never get here - and the
 * filter is then not used at all.  What the cursor keeps is allocated in
 * keep; the lookup's scratch in the current context.
 */
static bool
lion_poscursor_init(Relation index, LionState *col, Datum key,
					LionPosCursor *cur, MemoryContext keep)
{
	Buffer		buf = InvalidBuffer;
	OffsetNumber off;
	bool		ok = true;
	bool		moved;

	memset(cur, 0, sizeof(LionPosCursor));
	cur->root = InvalidBlockNumber;
	cur->leafblk = InvalidBlockNumber;
	cur->rightlink = InvalidBlockNumber;

	key = PointerGetDatum(PG_DETOAST_DATUM(key));
	if (!lion_find_entry_ext(index, col, BUFFER_LOCK_SHARE, key,
							 lion_hash_key(col, key), NULL, NULL, InvalidOid,
							 &buf, &off, &moved))
	{
		/* no row has the lexeme */
		cur->absent = true;
		lion_reader_lock_absent(index, buf, moved);	/* DESIGN.md §9 */
	}
	else
	{
		Page		page = BufferGetPage(buf);
		BlockNumber blkno = BufferGetBlockNumber(buf);
		LionEntryTuple *e = lion_page_entry_fetch(index, page, blkno, off);
		const LionEntryPosExt *x = lion_entry_posext(e);

		/*
		 * The rows of the lexeme are what a ranking reads (and a phrase
		 * filter, the positions of rows its sets already hold): §9.
		 */
		lion_reader_lock_entry(index, blkno, e);
		if (x == NULL)
			ok = false;
		else if ((e->flags & LION_ENTRY_CHAIN) != 0)
		{
			cur->hash = lion_entry_owner(e);
			cur->root = x->pos_root;
			if (!BlockNumberIsValid(cur->root))
				cur->absent = true;
			else
			{
				cur->leaf = MemoryContextAlloc(keep, BLCKSZ);
				cur->chunks = (LionContainer **)
					MemoryContextAlloc(keep, LION_POSCURSOR_CHUNKS_BYTES);
			}
		}
		else
		{
			cur->inl = (LionContainer *) MemoryContextAlloc(keep,
															LION_CONTAINER_MAX_SIZE);
			if (lion_entry_inline_poschunk(index, e,
										   ItemIdGetLength(PageGetItemId(page, off)),
										   blkno, off, cur->inl))
			{
				/* its one chunk: no allocation of a pointer per lexeme */
				cur->chunks = &cur->inl;
				cur->nchunks = 1;
				cur->rightmost = true;
			}
			else
			{
				/* an empty INLINE entry: no members */
				pfree(cur->inl);
				cur->inl = NULL;
				cur->absent = true;
			}
		}
	}
	if (BufferIsValid(buf))
		UnlockReleaseBuffer(buf);
	cur->done = cur->absent;

	return ok;
}

/* ---------------------------------------------------------------------
 * The filter
 * --------------------------------------------------------------------- */

/*
 * TS_execute()'s callback: checkclass_str() (tsvector_op.c) over the
 * candidate's member under the operand's lexeme.  No member is TS_NO; a
 * member with no positions - a stripped tsvector's - matches anything that
 * needs no positions and is a MAYBE to a phrase, which TS_execute() turns
 * into the NO the heap's operator answers.
 */
static TSTernaryValue
lion_posfilter_check(void *arg, QueryOperand *val, ExecPhraseData *data)
{
	LionPosFilter *pf = (LionPosFilter *) arg;
	int			item = (int) ((QueryItem *) val - pf->items);
	LionPosCursor *cur = &pf->cursors[pf->itemcur[item]];
	const LionPosMember *m;
	int			i;

	Assert(data == NULL || data->npos == 0);

	pf->nseeks++;
	if (!lion_poscursor_seek(pf->index, cur, pf->code))
		return TS_NO;
	m = &cur->m;

	if (m->npos == 0)
		return data != NULL ? TS_MAYBE : TS_YES;

	if (val->weight && data != NULL)
	{
		WordEntryPos *dptr;

		dptr = data->pos = palloc(sizeof(WordEntryPos) * m->npos);
		data->allocated = true;
		for (i = 0; i < m->npos; i++)
		{
			if (val->weight & (1 << WEP_GETWEIGHT(m->pos[i])))
				*dptr++ = WEP_GETPOS(m->pos[i]);
		}
		data->npos = dptr - data->pos;
		if (data->npos > 0)
			return TS_YES;
		pfree(data->pos);
		data->pos = NULL;
		data->allocated = false;
		return TS_NO;
	}
	if (val->weight)
	{
		for (i = 0; i < m->npos; i++)
		{
			if (val->weight & (1 << WEP_GETWEIGHT(m->pos[i])))
				return TS_YES;
		}
		return TS_NO;
	}
	if (data != NULL)
	{
		/* the cursor keeps m until its next seek, which is a later candidate */
		data->npos = m->npos;
		data->pos = (WordEntryPos *) m->pos;
		data->allocated = false;
	}
	return TS_YES;
}

/*
 * A LION_KN_POSFILTER node over child, the superset of query on the positions
 * column col of index, which the caller has made sure the filter can follow
 * (lion_tsquery_item_keys()).  Every cursor of the node begins a filter of
 * its own (lion_expr.c).
 */
LionKeyNode *
lion_posfilter_keynode(Relation index, LionState *col, Datum query,
					   StrategyNumber strategy, LionKeyNode *child)
{
	LionPosSpec *spec = (LionPosSpec *) palloc0(sizeof(LionPosSpec));
	LionKeyNode *n = (LionKeyNode *) palloc0(sizeof(LionKeyNode));

	spec->index = index;
	spec->col = col;
	spec->query = query;
	spec->strategy = strategy;
	spec->mem = lion_posfilter_mem(query);

	n->kind = LION_KN_POSFILTER;
	n->nargs = 1;
	n->args = (LionKeyNode **) palloc(sizeof(LionKeyNode *));
	n->args[0] = child;
	n->pos = spec;
	return n;
}

/*
 * A filter for query against the positions column col of index, or NULL when
 * the query has an operand it cannot follow (lion_tsquery_item_keys()); the
 * caller then rechecks the candidates in the heap, as it always did.
 */
LionPosFilter *
lion_posfilter_begin(Relation index, LionState *col, Datum query,
					 StrategyNumber strategy)
{
	MemoryContext cxt = CurrentMemoryContext;
	MemoryContext hold;
	MemoryContext setup;
	LionPosFilter *pf;
	Datum	   *itemkeys;
	Datum	   *curkeys;
	int32		noperands = 0;
	int32		i;
	bool		ok;

	Assert(col->positions);

	/*
	 * The filter is in a context of its own (LION_POSFILTER_CXT_SIZES), and
	 * what finding the cursors' keys takes and nothing after it - the
	 * opclass's extracted keys and their arrays, each lexeme's detoasted
	 * copy, the lookups of their entries - in one below it, deleted before
	 * the filter is used.  It was left in the caller's, once a filter, for as
	 * long as that lasted (2026-10 review).
	 */
	hold = AllocSetContextCreate(cxt, "lion position filter",
								 LION_POSFILTER_CXT_SIZES);
	setup = AllocSetContextCreate(hold, "lion position filter setup",
								  ALLOCSET_SMALL_SIZES);
	MemoryContextSwitchTo(setup);
	ok = lion_tsquery_item_keys(col, query, strategy, &itemkeys);
	MemoryContextSwitchTo(hold);
	if (!ok)
	{
		MemoryContextSwitchTo(cxt);
		MemoryContextDelete(hold);
		return NULL;
	}

	pf = (LionPosFilter *) palloc0(sizeof(LionPosFilter));
	pf->hold = hold;
	pf->index = index;
	pf->query = DatumGetTSQuery(query);
	pf->items = GETQUERY(pf->query);
	for (i = 0; i < pf->query->size; i++)
	{
		if (pf->items[i].type == QI_VAL)
			noperands++;
	}
	pf->itemcur = (int *) palloc(sizeof(int) * pf->query->size);
	/* a cursor per distinct operand at most: none for an operator */
	pf->cursors = (LionPosCursor *)
		palloc(sizeof(LionPosCursor) * Max(noperands, 1));
	curkeys = (Datum *) MemoryContextAlloc(setup,
										   sizeof(Datum) * Max(noperands, 1));

	for (i = 0; i < pf->query->size; i++)
	{
		int			k;

		pf->itemcur[i] = -1;
		if (pf->items[i].type != QI_VAL)
			continue;

		/* one cursor per distinct key: `a <-> b <-> a` reads a once */
		for (k = 0; k < pf->ncursors; k++)
		{
			if (datumIsEqual(curkeys[k], itemkeys[i], false, -1))
				break;
		}
		if (k == pf->ncursors)
		{
			MemoryContextSwitchTo(setup);
			ok = lion_poscursor_init(index, col, itemkeys[i],
									 &pf->cursors[k], hold);
			MemoryContextSwitchTo(hold);
			if (!ok)
				break;
			curkeys[k] = itemkeys[i];
			pf->ncursors++;
		}
		pf->itemcur[i] = k;
	}
	MemoryContextDelete(setup);
	if (!ok)
	{
		MemoryContextSwitchTo(cxt);
		lion_posfilter_end(pf);
		return NULL;
	}

	pf->los = (uint16 *) palloc(sizeof(uint16) * LION_CONTAINER_RANGE);
	pf->keep = (uint16 *) palloc(sizeof(uint16) * LION_CONTAINER_RANGE);
	pf->work = (LionContainer *) palloc(LION_CONTAINER_MAX_SIZE);
	pf->cxt = AllocSetContextCreate(hold,
									"lion position filter candidate",
									LION_POSFILTER_CXT_SIZES);
	MemoryContextSwitchTo(cxt);
	return pf;
}

/*
 * What a filter for query holds while it is open: the charge of a
 * LION_KN_POSFILTER node against the open budget (lion_plan_node(),
 * DESIGN.md §15 "Bounded cursors").  Of what lion_posfilter_begin()
 * allocates it is an upper bound, which its own contexts make one
 * (LION_POSFILTER_CXT_SIZES); of what lion_posfilter_container() allocates
 * for a candidate, and resets after it, an estimate.  Most of it grows with
 * the query.  Each distinct lexeme's cursor holds a copy of a leaf of its
 * position tree and a pointer per item that leaf may have, 24 kB, or an
 * INLINE entry's chunk, 4 kB; which of them a lexeme has is not known before
 * its entry is read, so each operand is charged a tree's.  The callback's
 * allocations for one candidate are about a list of positions an item.
 *
 * The charge used to be a flat 160 kB - the member arrays and four leaves -
 * and a phrase of a thousand lexemes allocated 24 MB under it, inside the
 * very budget meant to decide whether an OR of such filters can be opened at
 * once (2026-10 review).
 */
Size
lion_posfilter_mem(Datum query)
{
	TSQuery		q = DatumGetTSQuery(query);
	Size		nitems = (Size) Max(q->size, 0);
	Size		noperands = 0;
	Size		percursor;
	Size		mem;
	int32		i;

	for (i = 0; i < q->size; i++)
	{
		if (GETQUERY(q)[i].type == QI_VAL)
			noperands++;
	}

	/* the first block of the filter's context and of its candidates' */
	mem = 2 * LION_POSFILTER_CXT_FIRST;

	/* the filter, a copy of the query, itemcur and the cursors */
	mem += lion_posfilter_alloc_cost(sizeof(LionPosFilter)) +
		lion_posfilter_alloc_cost(VARSIZE(q)) +
		lion_posfilter_alloc_cost(sizeof(int) * Max(nitems, 1)) +
		lion_posfilter_alloc_cost(sizeof(LionPosCursor) * Max(noperands, 1));

	/* what each cursor copies: a tree's leaf and its items, or a chunk */
	percursor = Max(lion_posfilter_alloc_cost(BLCKSZ) +
					lion_posfilter_alloc_cost(LION_POSCURSOR_CHUNKS_BYTES),
					lion_posfilter_alloc_cost(LION_CONTAINER_MAX_SIZE));
	mem += noperands * percursor;

	/* los, keep and work */
	mem += 2 * lion_posfilter_alloc_cost(sizeof(uint16) * LION_CONTAINER_RANGE) +
		lion_posfilter_alloc_cost(LION_CONTAINER_MAX_SIZE);

	/* what one candidate allocates past its context's first block */
	mem += nitems *
		lion_posfilter_alloc_cost(sizeof(WordEntryPos) * LION_POS_MAX_NPOS);

	if ((Pointer) q != DatumGetPointer(query))
		pfree(q);
	return mem;
}

/*
 * Decide every member of the container c, ascending: the ones the query
 * matches go to keep[] (as many as LION_CONTAINER_RANGE), and the count is
 * returned.  Containers come in ascending key order.
 */
int
lion_posfilter_container(LionPosFilter *pf, const LionContainer *c,
						 uint16 *keep)
{
	MemoryContext oldcxt;
	uint32		n;
	uint32		i;
	int			nkeep = 0;

	Assert(c->type != LION_CT_SPARSE);
	n = lion_container_to_array(c, pf->los);
	if (n == 0)
		return 0;

	/*
	 * A caller that walks the containers again - a count per group, per key
	 * of a join, both over the same candidates - starts the cursors again.
	 */
	if (lion_make_code(c->ckey, pf->los[0]) < pf->nextcode)
	{
		int			k;

		for (k = 0; k < pf->ncursors; k++)
			lion_poscursor_rewind(&pf->cursors[k]);
	}
	pf->nextcode = lion_make_code(c->ckey, pf->los[n - 1]);

	/*
	 * What TS_execute() and the callback allocate - a phrase operator's
	 * positions, an operand's weighted copy of them - lasts one candidate:
	 * reset after each, it is one candidate's evaluation at most, which is
	 * what lion_posfilter_mem() charges, where a container's thousands of
	 * candidates used to pile theirs up.  A reset of a context nothing was
	 * allocated in does nothing.
	 */
	oldcxt = MemoryContextSwitchTo(pf->cxt);
	for (i = 0; i < n; i++)
	{
		pf->code = lion_make_code(c->ckey, pf->los[i]);
		if (TS_execute(GETQUERY(pf->query), pf, TS_EXEC_EMPTY,
					   lion_posfilter_check))
			keep[nkeep++] = pf->los[i];
		MemoryContextReset(pf->cxt);
	}
	MemoryContextSwitchTo(oldcxt);

	pf->nchecked += n;
	pf->nremoved += n - nkeep;
	return nkeep;
}

/*
 * lion_posfilter_container() for a caller that wants a container back: c's
 * members the query matches, in a container of the filter's that stays valid
 * until the next call.
 */
const LionContainer *
lion_posfilter_apply(LionPosFilter *pf, const LionContainer *c)
{
	int			nkeep = lion_posfilter_container(pf, c, pf->keep);
	int			i;

	lion_container_init(pf->work, c->ckey);
	for (i = 0; i < nkeep; i++)
		lion_container_append_sorted(pf->work, pf->keep[i]);
	lion_container_optimize(pf->work);
	return pf->work;
}

/* What the filter did: candidates decided, and those it turned down. */
void
lion_posfilter_counts(const LionPosFilter *pf, int64 *nchecked,
					  int64 *nremoved)
{
	*nchecked = pf->nchecked;
	*nremoved = pf->nremoved;
}

/*
 * The work it took, which a join's probing counts are charged
 * (lion_join_count_key()): the members sought, a candidate's under a lexeme
 * each, and the members decoded getting to them.
 */
void
lion_posfilter_work(const LionPosFilter *pf, int64 *nseeks, int64 *ndecoded)
{
	int64		n = 0;
	int			k;

	for (k = 0; k < pf->ncursors; k++)
		n += pf->cursors[k].ndecoded;
	*nseeks = pf->nseeks;
	*ndecoded = n;
}

void
lion_posfilter_end(LionPosFilter *pf)
{
	/* the filter, its cursors' copies and its candidates' context */
	MemoryContextDelete(pf->hold);
}

/* ---------------------------------------------------------------------
 * A key's members in TID order, for a caller other than the filter
 * (lion_bm25.c)
 * --------------------------------------------------------------------- */

/*
 * A cursor over key's members, or NULL when the column's entry for it does
 * not say where its positions are.  A key no row has gives a cursor with no
 * members.
 */
LionPosCursor *
lion_poscursor_open(Relation index, LionState *col, Datum key,
					bool countsonly)
{
	LionPosCursor *cur = (LionPosCursor *) palloc(sizeof(LionPosCursor));

	if (!lion_poscursor_init(index, col, key, cur, CurrentMemoryContext))
	{
		pfree(cur);
		return NULL;
	}
	cur->countsonly = countsonly;
	return cur;
}

/*
 * The first member whose code is at least code, or NULL when there is none;
 * it stays valid until the next call.  Codes only grow from one call to the
 * next.  A countsonly cursor's member has no positions, only their number.
 */
const LionPosMember *
lion_poscursor_next(Relation index, LionPosCursor *cur, uint64 code)
{
	for (;;)
	{
		(void) lion_poscursor_seek(index, cur, code);
		if (cur->mvalid)
			return &cur->m;
		if (cur->done)
			return NULL;

		/*
		 * The seek stopped at a chunk whose block is past code's, which only
		 * says code has no member: the next member is that chunk's first.
		 */
		Assert(!cur->itvalid && cur->chunkno < cur->nchunks);
		code = Max(code + 1,
				   lion_pos_block_base(cur->chunks[cur->chunkno]->ckey));
	}
}

void
lion_poscursor_close(LionPosCursor *cur)
{
	if (cur->leaf != NULL)
		pfree(cur->leaf);
	if (cur->inl != NULL)
		pfree(cur->inl);
	if (cur->chunks != NULL && cur->chunks != &cur->inl)
		pfree(cur->chunks);
	pfree(cur);
}

/* ---------------------------------------------------------------------
 * lion_debug_posfilter_mem(): what a filter is charged, and what it holds
 * --------------------------------------------------------------------- */

/*
 * What a filter of query on col of index adds to cxt, the context it is
 * begun in, and the contexts under it.
 */
static int64
lion_debug_posfilter_held(Relation index, LionState *col, Datum query,
						  MemoryContext cxt)
{
	MemoryContext oldcxt;
	LionPosFilter *pf;
	int64		before = (int64) MemoryContextMemAllocated(cxt, true);
	int64		held;

	oldcxt = MemoryContextSwitchTo(cxt);
	pf = lion_posfilter_begin(index, col, query, LION_STRAT_MATCH);
	MemoryContextSwitchTo(oldcxt);
	if (pf == NULL)
		elog(ERROR, "a position filter cannot follow that query");
	held = (int64) MemoryContextMemAllocated(cxt, true) - before;
	lion_posfilter_end(pf);
	return held;
}

/*
 * lion_debug_posfilter_mem(idx regclass, query tsquery, OUT charged bigint,
 * OUT held bigint, OUT held_elsewhere bigint): what lion_posfilter_mem()
 * charges a filter of query against the open budget, and what one begun on
 * the first column of idx holds - the bytes it added to a small context it
 * was begun in, and to a default-sized one - so that a test can see the
 * charge cover what the filter takes, whatever context it is begun in.  The
 * two are the same: the filter is in contexts of its own
 * (LION_POSFILTER_CXT_SIZES).  A test function like
 * lion_debug_key_positions(): the tests create it from the library.
 * Superuser only.
 */
PG_FUNCTION_INFO_V1(lion_debug_posfilter_mem);

Datum
lion_debug_posfilter_mem(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	Datum		query = PG_GETARG_DATUM(1);
	TupleDesc	tupdesc;
	Relation	index;
	LionState  *state;
	MemoryContext small;
	MemoryContext large;
	Datum		values[3];
	bool		nulls[3] = {false, false, false};

	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("lion_debug_posfilter_mem() is for superusers")));
	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");

	index = lion_open_index(relid, AccessShareLock);
	state = lion_get_state(index);
	if (!state->positions)
		elog(ERROR, "index \"%s\" does not store positions",
			 RelationGetRelationName(index));

	small = AllocSetContextCreate(CurrentMemoryContext,
								  "lion debug position filter",
								  ALLOCSET_SMALL_SIZES);
	large = AllocSetContextCreate(CurrentMemoryContext,
								  "lion debug position filter, large",
								  ALLOCSET_DEFAULT_SIZES);
	values[0] = Int64GetDatum((int64) lion_posfilter_mem(query));
	values[1] = Int64GetDatum(lion_debug_posfilter_held(index, state, query,
														small));
	values[2] = Int64GetDatum(lion_debug_posfilter_held(index, state, query,
														large));
	MemoryContextDelete(large);
	MemoryContextDelete(small);
	index_close(index, AccessShareLock);

	PG_RETURN_DATUM(HeapTupleGetDatum(heap_form_tuple(tupdesc, values,
													  nulls)));
}
