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

#include "miscadmin.h"
#include "storage/bufmgr.h"
#include "storage/bufpage.h"
#include "tsearch/ts_type.h"
#include "tsearch/ts_utils.h"
#include "utils/datum.h"
#include "utils/memutils.h"
#include "utils/rel.h"

#include "lion.h"
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
	LionPosMember m;
} LionPosCursor;

struct LionPosFilter
{
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
	MemoryContext cxt;			/* reset per container: the callback's pallocs */
	int64		nchecked;
	int64		nremoved;
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

	buf = lion_posting_search_before(index, NULL, cur->hash, cur->root, block,
									 BUFFER_LOCK_SHARE, false);
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
 * further right than one step, and a descent goes straight to it.
 */
static bool
lion_poscursor_next_leaf(Relation index, LionPosCursor *cur, uint32 block)
{
	Buffer		buf;

	if (cur->rightmost || !BlockNumberIsValid(cur->rightlink))
		return false;
	buf = ReadBuffer(index, cur->rightlink);
	LockBuffer(buf, BUFFER_LOCK_SHARE);
	if (!lion_poscursor_take_leaf(index, cur, buf))
		return false;
	if (!cur->rightmost && cur->nchunks > 0 &&
		cur->chunks[cur->nchunks - 1]->ckey < block)
		return lion_poscursor_descend(index, cur, block);
	return true;
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
		if (cur->itvalid)
		{
			while (lion_poschunk_iter_next(&cur->it, &cur->m))
			{
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
 * filter is then not used at all.
 */
static bool
lion_poscursor_init(Relation index, LionState *col, Datum key,
					LionPosCursor *cur)
{
	Buffer		buf = InvalidBuffer;
	OffsetNumber off;
	bool		ok = true;

	memset(cur, 0, sizeof(LionPosCursor));
	cur->root = InvalidBlockNumber;
	cur->leafblk = InvalidBlockNumber;
	cur->rightlink = InvalidBlockNumber;

	key = PointerGetDatum(PG_DETOAST_DATUM(key));
	if (!lion_find_entry(index, col, BUFFER_LOCK_SHARE, key,
						 lion_hash_key(col, key), &buf, &off))
	{
		/* no row has the lexeme */
		cur->absent = true;
	}
	else
	{
		Page		page = BufferGetPage(buf);
		BlockNumber blkno = BufferGetBlockNumber(buf);
		LionEntryTuple *e = lion_page_entry_fetch(index, page, blkno, off);
		const LionEntryPosExt *x = lion_entry_posext(e);

		if (x == NULL)
			ok = false;
		else if ((e->flags & LION_ENTRY_CHAIN) != 0)
		{
			cur->hash = e->hash;
			cur->root = x->pos_root;
			if (!BlockNumberIsValid(cur->root))
				cur->absent = true;
			else
			{
				cur->leaf = palloc(BLCKSZ);
				cur->chunks = (LionContainer **)
					palloc(sizeof(LionContainer *) * MaxOffsetNumber);
			}
		}
		else
		{
			cur->inl = (LionContainer *) palloc(LION_CONTAINER_MAX_SIZE);
			if (lion_entry_inline_poschunk(index, e,
										   ItemIdGetLength(PageGetItemId(page, off)),
										   blkno, off, cur->inl))
			{
				cur->chunks = (LionContainer **) palloc(sizeof(LionContainer *));
				cur->chunks[0] = cur->inl;
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
	LionPosFilter *pf;
	Datum	   *itemkeys;
	Datum	   *curkeys;
	int32		i;

	Assert(col->positions);
	if (!lion_tsquery_item_keys(col, query, strategy, &itemkeys))
		return NULL;

	pf = (LionPosFilter *) palloc0(sizeof(LionPosFilter));
	pf->index = index;
	pf->query = DatumGetTSQuery(query);
	pf->items = GETQUERY(pf->query);
	pf->itemcur = (int *) palloc(sizeof(int) * pf->query->size);
	pf->cursors = (LionPosCursor *)
		palloc(sizeof(LionPosCursor) * pf->query->size);
	curkeys = (Datum *) palloc(sizeof(Datum) * pf->query->size);

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
			if (!lion_poscursor_init(index, col, itemkeys[i],
									 &pf->cursors[k]))
			{
				lion_posfilter_end(pf);
				return NULL;
			}
			curkeys[k] = itemkeys[i];
			pf->ncursors++;
		}
		pf->itemcur[i] = k;
	}

	pf->los = (uint16 *) palloc(sizeof(uint16) * LION_CONTAINER_RANGE);
	pf->keep = (uint16 *) palloc(sizeof(uint16) * LION_CONTAINER_RANGE);
	pf->work = (LionContainer *) palloc(LION_CONTAINER_MAX_SIZE);
	pf->cxt = AllocSetContextCreate(CurrentMemoryContext,
									"lion position filter",
									ALLOCSET_SMALL_SIZES);
	return pf;
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

	oldcxt = MemoryContextSwitchTo(pf->cxt);
	for (i = 0; i < n; i++)
	{
		pf->code = lion_make_code(c->ckey, pf->los[i]);
		if (TS_execute(GETQUERY(pf->query), pf, TS_EXEC_EMPTY,
					   lion_posfilter_check))
			keep[nkeep++] = pf->los[i];
	}
	MemoryContextSwitchTo(oldcxt);
	MemoryContextReset(pf->cxt);

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

void
lion_posfilter_end(LionPosFilter *pf)
{
	int			k;

	for (k = 0; k < pf->ncursors; k++)
	{
		LionPosCursor *cur = &pf->cursors[k];

		if (cur->leaf != NULL)
			pfree(cur->leaf);
		if (cur->inl != NULL)
			pfree(cur->inl);
		if (cur->chunks != NULL)
			pfree(cur->chunks);
	}
	if (pf->cxt != NULL)
		MemoryContextDelete(pf->cxt);
	if (pf->los != NULL)
		pfree(pf->los);
	if (pf->keep != NULL)
		pfree(pf->keep);
	if (pf->work != NULL)
		pfree(pf->work);
	pfree(pf->cursors);
	pfree(pf->itemcur);
	pfree(pf);
}
