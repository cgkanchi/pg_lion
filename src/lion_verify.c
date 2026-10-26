/*-------------------------------------------------------------------------
 *
 * lion_verify.c
 *		lion_index_verify(): the check's entry point, its pages, and the
 *		posting sets of the directory's entries.
 *
 * Part of the SQL-callable helpers of the lion index; lion_funcs.h
 * describes them and declares what their files share.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/xlogrecovery.h"
#include "commands/tablespace.h"

#include "lion_funcs.h"

PG_FUNCTION_INFO_V1(lion_index_verify);

/* The name a message gives a reserved entry of kind (DESIGN.md §14, §17). */
static const char *
lion_reserved_name(int kind)
{
	switch (kind)
	{
		case LION_KIND_NULL:
			return "null";
		case LION_KIND_EMPTY:
			return "empty";
		case LION_KIND_EMPTYDIM:
			return "empty-dimension";
		case LION_KIND_EMPTYINNER:
			return "empty-inner";
		case LION_KIND_NULLELEMS:
			return "null-elements";
	}
	elog(ERROR, "lion index: %d is not the kind of a reserved entry", kind);
	return NULL;				/* keep the compiler quiet */
}

/* ---------------------------------------------------------------------
 * Sequences of records (lion_funcs.h, LionVerifySeq)
 * --------------------------------------------------------------------- */

/*
 * An empty sequence of records of recsize bytes, which keeps its memory and
 * its file in the current memory context and counts them in acct.  Nothing
 * is allocated until the first record comes.
 */
void
lion_verify_seq_init(LionVerifySeq *s, LionVerifySeqAcct *acct, Size recsize)
{
	Assert(recsize > 0 && recsize <= acct->cap);
	memset(s, 0, sizeof(*s));
	s->acct = acct;
	s->cxt = CurrentMemoryContext;
	s->recsize = recsize;
	s->fileat = -1;
}

/*
 * Append a record.  The records stay in memory while they are a prefix of
 * the sequence and their share of work_mem allows (the buffer doubles up to
 * it), and every record after the first that does not goes to the file -
 * made the first time, in temp_tablespaces, which lion_index_verify() had
 * PrepareTempTablespaces() look up before the walk began: the lookup reads
 * catalogs, and that must not happen in the middle of a walk.  The caller
 * holds no buffer lock: a write to the file is I/O.
 */
void
lion_verify_seq_put(LionVerifySeq *s, const void *rec)
{
	LionVerifySeqAcct *acct = s->acct;

	if (s->nmem == s->n && s->nmem == s->memalloc)
	{
		int64		capn = (int64) (acct->cap / s->recsize);

		if (s->memalloc < capn)
		{
			int64		grow = Min(Max(s->memalloc * 2, 64), capn);
			MemoryContext oldcxt = MemoryContextSwitchTo(s->cxt);

			s->mem = (s->mem == NULL) ?
				(char *) palloc(s->recsize * grow) :
				(char *) repalloc(s->mem, s->recsize * grow);
			MemoryContextSwitchTo(oldcxt);
			acct->mem += s->recsize * (grow - s->memalloc);
			acct->peak = Max(acct->peak, acct->mem);
			s->memalloc = grow;
		}
	}

	if (s->nmem == s->n && s->nmem < s->memalloc)
	{
		memcpy(s->mem + s->recsize * s->nmem, rec, s->recsize);
		s->nmem++;
		s->n++;
		return;
	}

	/* past what memory may keep: onto the end of the file */
	if (s->file == NULL)
	{
		MemoryContext oldcxt = MemoryContextSwitchTo(s->cxt);

		s->file = BufFileCreateTemp(false);
		s->fileat = 0;
		MemoryContextSwitchTo(oldcxt);
	}
	if (s->fileat != s->n - s->nmem)
	{
		if (BufFileSeek(s->file, 0, (off_t) (s->n - s->nmem) * s->recsize,
						SEEK_SET) != 0)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not seek in the temporary file of a lion index check")));
		s->fileat = s->n - s->nmem;
	}
	BufFileWrite(s->file, rec, s->recsize);
	s->fileat++;
	s->n++;
	acct->spilled++;
}

/* Read the sequence again from its first record. */
void
lion_verify_seq_rewind(LionVerifySeq *s)
{
	s->next = 0;
}

/* The next record into rec, or false when there is none left. */
bool
lion_verify_seq_get(LionVerifySeq *s, void *rec)
{
	int64		k;

	if (s->next >= s->n)
		return false;
	if (s->next < s->nmem)
	{
		memcpy(rec, s->mem + s->recsize * s->next, s->recsize);
		s->next++;
		return true;
	}

	Assert(s->file != NULL);
	k = s->next - s->nmem;
	if (s->fileat != k)
	{
		if (BufFileSeek(s->file, 0, (off_t) k * s->recsize, SEEK_SET) != 0)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not seek in the temporary file of a lion index check")));
		s->fileat = k;
	}
	BufFileReadExact(s->file, rec, s->recsize);
	s->fileat++;
	s->next++;
	return true;
}

/*
 * Empty the sequence for another walk, keeping its buffer and its file:
 * the records written next overwrite the old ones, and nothing reads past
 * the count.
 */
void
lion_verify_seq_reset(LionVerifySeq *s)
{
	s->n = 0;
	s->nmem = 0;
	s->next = 0;
}

/* Give back the sequence's memory and close its file. */
void
lion_verify_seq_free(LionVerifySeq *s)
{
	if (s->mem != NULL)
	{
		Assert(s->acct->mem >= s->recsize * s->memalloc);
		s->acct->mem -= s->recsize * s->memalloc;
		pfree(s->mem);
	}
	if (s->file != NULL)
		BufFileClose(s->file);
	s->mem = NULL;
	s->memalloc = 0;
	s->file = NULL;
	s->fileat = -1;
	lion_verify_seq_reset(s);
}

/* ---------------------------------------------------------------------
 * lion_index_verify()
 * --------------------------------------------------------------------- */

/* Take the index's length again, and grow the per-block array with it. */
void
lion_verify_refresh_nblocks(LionVerifyState *vs)
{
	BlockNumber n = RelationGetNumberOfBlocks(vs->index);

	if (n > vs->nblocks)
	{
		vs->refs = (uint8 *) repalloc0(vs->refs, Max(vs->nblocks, 1),
									   sizeof(uint8) * n);
		vs->nblocks = n;
	}
}

/*
 * Is blk a block of the index?
 *
 * vs->nblocks starts as the count taken when the check began, and the index
 * grows while it runs: INSERTs go on beside it on a primary, and replay does
 * on a standby.  A split takes its new page from the end of the relation and
 * links it in the record that initialises it, so a link to a block past the
 * count the check started with is the commonest thing a concurrent writer
 * shows it.  The count is therefore read again before a block is called out of
 * range, and only a block that is past the end NOW is: that needs no waiting,
 * since a page is linked in the same record that makes it exist.
 * InvalidBlockNumber is never a block (and must never reach ReadBuffer(),
 * which takes it for P_NEW and extends the relation on 16 to 18).
 */
bool
lion_verify_block_exists(LionVerifyState *vs, BlockNumber blk)
{
	if (blk < vs->nblocks)
		return true;
	if (!BlockNumberIsValid(blk))
		return false;

	lion_verify_refresh_nblocks(vs);
	return blk < vs->nblocks;
}

/*
 * Record that blk is referenced by something, and refuse to look at it twice
 * (which also stops a corrupt rightlink cycle from looping forever).
 */
void
lion_verify_visit(LionVerifyState *vs, BlockNumber blk, const char *what)
{
	if (!lion_verify_block_exists(vs, blk))
		lion_corrupt("lion index \"%s\": %s points at block %u, but the index has only %u blocks",
					RelationGetRelationName(vs->index), what, blk, vs->nblocks);

	if (vs->refs[blk] != 0)
		lion_corrupt("lion index \"%s\": block %u is referenced more than once (reached again as %s)",
					RelationGetRelationName(vs->index), blk, what);

	vs->refs[blk]++;

	/* (the setvisits sequence keeps its memory and file outside setcxt) */
	if (vs->settrack)
		lion_verify_seq_put(&vs->setvisits, &blk);
}

/*
 * A walk of a posting set that may have to be repeated keeps a list of the
 * blocks it marks (lion_verify_set()); a walk that is kept leaves them marked,
 * one that is thrown away unmarks them for the next.
 */
static void
lion_verify_track_begin(LionVerifyState *vs)
{
	vs->settrack = true;
	lion_verify_seq_reset(&vs->setvisits);
}

static void
lion_verify_track_end(LionVerifyState *vs, bool keep)
{
	BlockNumber blk;

	if (!keep)
	{
		lion_verify_seq_rewind(&vs->setvisits);
		while (lion_verify_seq_get(&vs->setvisits, &blk))
		{
			if (blk >= vs->nblocks || vs->refs[blk] == 0)
				elog(ERROR, "lion index \"%s\": block %u was unmarked twice",
					 RelationGetRelationName(vs->index), blk);
			vs->refs[blk]--;
		}
	}
	vs->settrack = false;
	lion_verify_seq_reset(&vs->setvisits);
}

/* Record a candidate for lion_verify_recheck(); only on a primary. */
void
lion_verify_add_cand(LionVerifyState *vs, const LionVerifyCand *cand)
{
	Assert(vs->concurrent);

	if (vs->ncands >= vs->maxcands)
	{
		vs->maxcands = Max(vs->maxcands * 2, 16);
		vs->cands = (vs->cands == NULL) ?
			(LionVerifyCand *) palloc(sizeof(LionVerifyCand) * vs->maxcands) :
			(LionVerifyCand *) repalloc(vs->cands,
										sizeof(LionVerifyCand) * vs->maxcands);
	}
	vs->cands[vs->ncands++] = *cand;
}

int
lion_verify_blkcmp(const void *a, const void *b)
{
	BlockNumber x = *(const BlockNumber *) a;
	BlockNumber y = *(const BlockNumber *) b;

	return (x < y) ? -1 : (x > y) ? 1 : 0;
}

/* A sorted copy of n block numbers, for lion_verify_has_block(). */
BlockNumber *
lion_verify_sorted_blocks(const BlockNumber *blocks, int n)
{
	BlockNumber *s = (BlockNumber *) palloc(sizeof(BlockNumber) * Max(n, 1));

	if (n > 0)
	{
		memcpy(s, blocks, sizeof(BlockNumber) * n);
		qsort(s, n, sizeof(BlockNumber), lion_verify_blkcmp);
	}
	return s;
}

bool
lion_verify_has_block(const BlockNumber *sorted, int n, BlockNumber blk)
{
	return n > 0 &&
		bsearch(&blk, sorted, n, sizeof(BlockNumber), lion_verify_blkcmp) != NULL;
}

/*
 * Read blk with a SHARE lock and check its page header and kind.
 */
/*
 * The header bounds everything else on the page: the line pointer array ends
 * at pd_lower, the items live in [pd_upper, pd_special).  A page read from
 * disk had this checked by PageIsVerified(); one that was damaged in shared
 * buffers did not.  Every reader of a page nothing has checked yet asks this
 * before it counts the page's line pointers.
 */
void
lion_verify_page_bounds(LionVerifyState *vs, BlockNumber blk, Page page)
{
	PageHeader	phdr = (PageHeader) page;

	if (phdr->pd_lower < SizeOfPageHeaderData ||
		phdr->pd_lower > phdr->pd_upper ||
		phdr->pd_upper > phdr->pd_special ||
		phdr->pd_special > BLCKSZ)
		lion_corrupt("lion index \"%s\": block %u has a page header with lower %u, upper %u and special %u",
					RelationGetRelationName(vs->index), blk,
					phdr->pd_lower, phdr->pd_upper, phdr->pd_special);
}

/*
 * The flag bits a page of each kind may carry (lion.h): its kind, and the
 * states a page of that kind goes through.  A directory page may be the
 * root, the left half of an unfinished split, or freed, and only a leaf is
 * ever HALF_DEAD (DESIGN.md §21).  A container page may be the left half of
 * an unfinished split, freed, a page of a position tree (§17) rather than of
 * a posting set, and - a posting leaf only - UNLINKED (§22).  The meta page
 * is the meta page and nothing else.
 */
static uint16
lion_verify_kind_flags(uint16 kind)
{
	switch (kind)
	{
		case LION_PAGE_META:
			return LION_PAGE_META;
		case LION_PAGE_BUCKET:
			return LION_PAGE_BUCKET | LION_PAGE_ROOT |
				LION_PAGE_INCOMPLETE_SPLIT | LION_PAGE_DELETED |
				LION_PAGE_HALF_DEAD;
		case LION_PAGE_DIR:
			return LION_PAGE_DIR | LION_PAGE_ROOT |
				LION_PAGE_INCOMPLETE_SPLIT | LION_PAGE_DELETED;
		case LION_PAGE_CONTAINER:
			return LION_PAGE_CONTAINER | LION_PAGE_INCOMPLETE_SPLIT |
				LION_PAGE_DELETED | LION_PAGE_POSITIONS | LION_PAGE_UNLINKED;
	}
	return 0;
}

/*
 * Check the flags of a page whose header and special area are known to be
 * lion's, and return its kind: exactly one kind, no bit its kind never
 * carries, and the one combination the bits alone do not rule out - UNLINKED
 * on anything but a posting leaf.
 *
 * A bit outside its kind's set is one no writer sets, and it is not
 * harmless: every reader of a posting set refuses a page flagged
 * LION_PAGE_POSITIONS (lion_page_owns()) and ends the set there, while the
 * tree code, which serves both kinds, does not look at the flag
 * (lion_page_owns_tree()).  A posting leaf with that one bit set costs the
 * counts and the scans every row from that leaf on, while a lookup through
 * the tree code still finds them - which is why heapallindexed holds the
 * leaf its lookup lands on to the readers' test (lion_verify_tid_present()).
 */
uint16
lion_verify_page_flags(LionVerifyState *vs, BlockNumber blk, Page page)
{
	LionPageOpaque opaque = LionPageGetOpaque(page);
	uint16		kind = opaque->flags & LION_PAGE_KINDS;

	if (kind != LION_PAGE_META && kind != LION_PAGE_BUCKET &&
		kind != LION_PAGE_CONTAINER && kind != LION_PAGE_DIR)
		lion_corrupt("lion index \"%s\": block %u has flags 0x%04X, expected exactly one page kind",
					RelationGetRelationName(vs->index), blk, opaque->flags);

	if ((opaque->flags & ~lion_verify_kind_flags(kind)) != 0)
		lion_corrupt("lion index \"%s\": block %u is a %s page with flags 0x%04X, which no %s page carries",
					RelationGetRelationName(vs->index), blk,
					kind == LION_PAGE_META ? "meta" :
					kind == LION_PAGE_BUCKET ? "leaf" :
					kind == LION_PAGE_DIR ? "directory" : "container",
					opaque->flags,
					kind == LION_PAGE_META ? "meta" :
					kind == LION_PAGE_BUCKET ? "leaf" :
					kind == LION_PAGE_DIR ? "directory" : "container");

	if ((opaque->flags & LION_PAGE_UNLINKED) != 0 &&
		(opaque->level != 0 || (opaque->flags & LION_PAGE_POSITIONS) != 0))
		lion_corrupt("lion index \"%s\": block %u is marked unlinked but is not a posting leaf",
					RelationGetRelationName(vs->index), blk);

	return kind;
}

Page
lion_verify_read_page(LionVerifyState *vs, BlockNumber blk, uint16 kind,
					 Buffer *bufp)
{
	Buffer		buf;
	Page		page;
	LionPageOpaque opaque;
	uint16		flags;

	/*
	 * Every block number that reaches this function was read off a page, and
	 * ReadBuffer() must not see one past the end: on 16 to 18 it treats
	 * InvalidBlockNumber as P_NEW and EXTENDS the relation, and a check that
	 * writes to what it checks is worse than one that crashes.
	 */
	if (!lion_verify_block_exists(vs, blk))
		lion_corrupt("lion index \"%s\": a link points at block %u, but the index has only %u blocks",
					RelationGetRelationName(vs->index), blk, vs->nblocks);

	buf = ReadBuffer(vs->index, blk);
	LockBuffer(buf, BUFFER_LOCK_SHARE);
	page = BufferGetPage(buf);
	*bufp = buf;

	/* a page replay changed since the walk on a standby began (lsntrack) */
	if (vs->lsntrack && BufferGetLSNAtomic(buf) > vs->lsnhorizon)
		vs->lsnpast = true;

	if (PageIsNew(page))
		lion_corrupt("lion index \"%s\": block %u has never been initialised",
					RelationGetRelationName(vs->index), blk);

	lion_verify_page_bounds(vs, blk, page);

	if (PageGetSpecialSize(page) != LION_SPECIAL_SIZE)
		lion_corrupt("lion index \"%s\": block %u has a special area of %u bytes, expected %zu",
					RelationGetRelationName(vs->index), blk,
					(unsigned) PageGetSpecialSize(page),
					(Size) LION_SPECIAL_SIZE);

	opaque = LionPageGetOpaque(page);

	if (opaque->page_id != LION_PAGE_ID)
		lion_corrupt("lion index \"%s\": block %u has page id 0x%04X, expected 0x%04X",
					RelationGetRelationName(vs->index), blk,
					opaque->page_id, LION_PAGE_ID);

	flags = lion_verify_page_flags(vs, blk, page);
	if (flags != kind)
		lion_corrupt("lion index \"%s\": block %u is a %s page, expected a %s page",
					RelationGetRelationName(vs->index), blk,
					flags == LION_PAGE_META ? "meta" :
					flags == LION_PAGE_BUCKET ? "leaf" :
					flags == LION_PAGE_DIR ? "directory" : "container",
					kind == LION_PAGE_META ? "meta" :
					kind == LION_PAGE_BUCKET ? "leaf" :
					kind == LION_PAGE_DIR ? "directory" : "container");

	return page;
}

/*
 * The line pointer of item off, checked before anything reads the item it
 * points at, which a corrupt one could otherwise place anywhere on the page
 * or past its end (amcheck's PageGetItemIdCareful()).
 *
 * An UNUSED line pointer is handed back as it is, for the caller to skip or
 * refuse: VACUUM leaves them on directory leaves, whose entry offsets never
 * move (DESIGN.md §18).  lion never marks an item dead or redirects one, and
 * a used item has to lie wholly inside the item space [pd_upper, pd_special)
 * at a MAXALIGNed offset - the same test bufpage.c applies before it moves
 * one.  The header bounds were checked by lion_verify_read_page().
 */
ItemId
lion_verify_itemid(LionVerifyState *vs, BlockNumber blk, Page page,
				   OffsetNumber off)
{
	PageHeader	phdr = (PageHeader) page;
	ItemId		iid = PageGetItemId(page, off);
	unsigned	lpoff;
	unsigned	lplen;

	if (!ItemIdIsUsed(iid))
		return iid;

	if (!ItemIdIsNormal(iid))
		lion_corrupt("lion index \"%s\": line pointer %u on block %u is %s, which lion never makes",
					RelationGetRelationName(vs->index), off, blk,
					ItemIdIsDead(iid) ? "dead" : "a redirect");

	lpoff = ItemIdGetOffset(iid);
	lplen = ItemIdGetLength(iid);
	if (lplen == 0 || lpoff < phdr->pd_upper ||
		lpoff + lplen > phdr->pd_special || lpoff != MAXALIGN(lpoff))
		lion_corrupt("lion index \"%s\": line pointer %u on block %u points at %u bytes at offset %u, outside the item space %u .. %u",
					RelationGetRelationName(vs->index), off, blk, lplen, lpoff,
					phdr->pd_upper, phdr->pd_special);

	return iid;
}

/*
 * Check that the stored key of an entry has the length its type calls for,
 * so that hashing it cannot run off the end of the item - which is the form
 * the readers require of a key before an opclass function sees it
 * (lion_key_is_valid()), so that an index this passes they can read.
 */
static void
lion_verify_keylen(LionVerifyState *vs, LionState *state, BlockNumber blk,
				  OffsetNumber off, const LionEntryTuple *entry)
{
	Size		keylen = entry->keylen;
	const char *key = LionEntryGetKey(entry);

	if (keylen == 0 || keylen > LION_MAX_KEY_SIZE)
		lion_corrupt("lion index \"%s\": entry %u on block %u has key length %zu, expected 1 .. %d",
					RelationGetRelationName(vs->index), off, blk, keylen,
					LION_MAX_KEY_SIZE);

	/* a by-value key too: at its type's width (lion_entryfmt.h) */
	if (state->typlen > 0)
	{
		if (keylen != (Size) state->typlen)
			lion_corrupt("lion index \"%s\": entry %u on block %u has key length %zu, expected %d",
						RelationGetRelationName(vs->index), off, blk, keylen,
						state->typlen);
	}
	else if (state->typlen == -1)
	{
		/*
		 * A plain 4-byte header, as lion_store_key() writes it: a short one
		 * is no longer accepted either, because no reader does.  Or a
		 * compressed one, for a key too long to store plain (DESIGN.md §4,
		 * "Long keys"), which lion_key_is_valid() checks as the readers do.
		 */
		if (keylen < VARHDRSZ ||
			(VARATT_IS_4B_U(key) ? VARSIZE(key) != keylen :
			 !lion_key_is_valid(state, key, keylen)))
			lion_corrupt("lion index \"%s\": entry %u on block %u has a malformed varlena key of %zu bytes",
						RelationGetRelationName(vs->index), off, blk, keylen);
	}
	else
	{
		if (strnlen(key, keylen) != keylen - 1)
			lion_corrupt("lion index \"%s\": entry %u on block %u has a malformed cstring key of %zu bytes",
						RelationGetRelationName(vs->index), off, blk, keylen);
	}

	Assert(lion_key_is_valid(state, key, keylen));
}

/*
 * The parts of a directory item - an entry, a high key or a downlink - that
 * the rest of the check READS before it could check them: a line pointer that
 * stays on the page, a whole header, a key column the index has, and key
 * bytes that lie inside the item and have the length the column's key type
 * calls for.  The order checks compare keys through the opclass, the
 * duplicate check calls its equality, the entry check hashes the key, and
 * lion_column() of a column the index does not have indexes past its array:
 * each of those would read wherever a lying header sent it.  So every item of
 * a directory page goes through this before any of them looks at it.
 *
 * Returns NULL for an unused line pointer, which the caller skips.
 */
LionEntryTuple *
lion_verify_dir_item(LionVerifyState *vs, BlockNumber blk, Page page,
					 OffsetNumber off)
{
	ItemId		iid = lion_verify_itemid(vs, blk, page, off);
	LionEntryTuple *item;
	Size		itemsz;
	int			kind;

	if (!ItemIdIsUsed(iid))
		return NULL;

	itemsz = ItemIdGetLength(iid);
	if (itemsz < LION_ENTRY_SHORT_HDRSZ)
		lion_corrupt("lion index \"%s\": item %u on block %u is only %zu bytes, less than an entry header",
					RelationGetRelationName(vs->index), off, blk, itemsz);

	item = (LionEntryTuple *) PageGetItem(page, iid);
	if (itemsz < LionEntryPayloadOffset(item))
		lion_corrupt("lion index \"%s\": item %u on block %u is %zu bytes, too small for its %u byte key",
					RelationGetRelationName(vs->index), off, blk, itemsz,
					item->keylen);

	kind = lion_entry_kind(item);

	/* The minus-infinity downlink has no column and no key (§21, §24). */
	if (kind == LION_KIND_MINF)
	{
		if (item->attno != 0 || item->keylen != 0)
			lion_corrupt("lion index \"%s\": the minus-infinity item %u on block %u has key column %u and a key of %u bytes",
						RelationGetRelationName(vs->index), off, blk,
						item->attno, item->keylen);
		return item;
	}

	if (item->attno < 1 || item->attno > vs->ix->ncolumns)
		lion_corrupt("lion index \"%s\": entry %u on block %u belongs to key column %u, but the index has %d",
					RelationGetRelationName(vs->index), off, blk, item->attno,
					vs->ix->ncolumns);

	/*
	 * A pivot is never of the open summary bucket's kind
	 * (lion_pivot_kindflags(), DESIGN.md §32).  Before 2026-09-28 a split or
	 * the build could make one, and once that bucket closed in place it sorted
	 * below the separator that routes to it: every later insert filed its row
	 * into that one closed bucket and range counts summed it where it did not
	 * belong.  An index with one is damaged or about to be.
	 */
	if (LionEntryIsPivot(item) && kind == LION_KIND_SUMLAST)
		lion_corrupt_reindex("lion index \"%s\": %s %u on block %u has the kind of an open summary bucket, which puts the summaries of key column %u out of order once that bucket closes",
							 RelationGetRelationName(vs->index),
							 LionEntryIsHighKey(item) ? "high key" : "downlink",
							 off, blk, item->attno);

	if (LION_KIND_IS_RESERVED(kind))
	{
		/* the reserved entries, or pivots made from them (§14, §17) */
		if (item->keylen != 0)
			lion_corrupt("lion index \"%s\": %s item %u on block %u has a key of %u bytes",
						RelationGetRelationName(vs->index),
						lion_reserved_name(kind), off, blk, item->keylen);
		return item;
	}

	/*
	 * A slice, or a pivot made from one (§51): its key is one byte, a bit
	 * position of its column's width or the existence slice's, and its
	 * column is one the meta page says is sliced - which is what the order
	 * checks and the rest of the check can then rely on.
	 */
	if (kind == LION_KIND_SLICE)
	{
		LionState  *col = lion_column(vs->ix, (AttrNumber) item->attno);
		int			pos;

		if (item->keylen != LION_SLICE_KEYLEN)
			lion_corrupt("lion index \"%s\": slice item %u on block %u has a key of %u bytes, expected %d",
						RelationGetRelationName(vs->index), off, blk,
						item->keylen, LION_SLICE_KEYLEN);
		pos = lion_entry_slice_pos(item);
		if (!col->sliced)
			lion_corrupt("lion index \"%s\": item %u on block %u is a slice of key column %u, which has none",
						RelationGetRelationName(vs->index), off, blk,
						item->attno);
		if (pos != LION_SLICE_EXISTS && pos >= col->slicebits)
			lion_corrupt("lion index \"%s\": slice item %u on block %u names bit %d of key column %u, whose values are %d bits wide",
						RelationGetRelationName(vs->index), off, blk, pos,
						item->attno, col->slicebits);
		if ((item->flags & (LION_ENTRY_RESERVED | LION_ENTRY_SUMKINDS |
							LION_ENTRY_POSITIONS | LION_ENTRY_ALIGNKEY)) != 0 ||
			(item->flags & ~(uint16) LION_ENTRY_ALLFLAGS) != 0)
			lion_corrupt("lion index \"%s\": slice item %u on block %u has flags 0x%04X, which no slice has",
						RelationGetRelationName(vs->index), off, blk,
						item->flags);
		if (LionEntryIsPivot(item) &&
			((item->flags & (LION_ENTRY_INLINE | LION_ENTRY_CHAIN)) != 0 ||
			 item->sh_ncontainers != 0))
			lion_corrupt("lion index \"%s\": %s %u on block %u has flags 0x%04X, which no pivot of key column %u has",
						RelationGetRelationName(vs->index),
						LionEntryIsHighKey(item) ? "high key" : "downlink",
						off, blk, item->flags, item->attno);
		return item;
	}

	/*
	 * A pivot (lion_entryfmt.h): the short header, no counts, and the key
	 * where the key column keeps it.  Leaf entries are checked in full by
	 * lion_verify_entry().
	 */
	if (LionEntryIsPivot(item))
	{
		LionState  *col = lion_column(vs->ix, (AttrNumber) item->attno);

		if ((item->flags & (LION_ENTRY_INLINE | LION_ENTRY_CHAIN |
							LION_ENTRY_POSITIONS)) != 0 ||
			(item->flags & ~(uint16) LION_ENTRY_ALLFLAGS) != 0 ||
			(item->flags & LION_ENTRY_ALIGNKEY) != lion_key_layout_flags(col))
			lion_corrupt("lion index \"%s\": %s %u on block %u has flags 0x%04X, which no pivot of key column %u has",
						RelationGetRelationName(vs->index),
						LionEntryIsHighKey(item) ? "high key" : "downlink",
						off, blk, item->flags, item->attno);
		if (item->sh_ncontainers != 0)
			lion_corrupt("lion index \"%s\": %s %u on block %u has a non-zero reserved header field",
						RelationGetRelationName(vs->index),
						LionEntryIsHighKey(item) ? "high key" : "downlink",
						off, blk);
	}

	lion_verify_keylen(vs, lion_column(vs->ix, (AttrNumber) item->attno),
					   blk, off, item);
	return item;
}

/*
 * An item may be allotted MORE bytes on a container page than its header
 * needs: growth slack the insert path adds a member into without moving
 * anything else on the page (DESIGN.md §4).  So the rule is not "the item
 * fills its space exactly" any more, but "it fills it to within one slack
 * allowance": avail is the allocated length (ItemIdGetLength, or the exact
 * size of an item inside an INLINE payload, which never has slack).
 */
static void
lion_verify_item_slack(LionVerifyState *vs, BlockNumber blk, OffsetNumber off,
					  const LionContainer *item, Size avail, const char *what)
{
	Size		size = lion_item_size(item);

	if (avail < size)
		lion_corrupt("lion index \"%s\": %s %u on block %u occupies %zu bytes but needs %zu",
					RelationGetRelationName(vs->index), what, off, blk, avail,
					size);

	if (avail > (Size) LION_CONTAINER_MAX_SIZE)
		lion_corrupt("lion index \"%s\": %s %u on block %u occupies %zu bytes, more than an item may ever take",
					RelationGetRelationName(vs->index), what, off, blk, avail);

	if (avail - size > LION_ITEM_SLACK_BOUND)
		lion_corrupt("lion index \"%s\": %s %u on block %u occupies %zu bytes, %zu more than its %zu bytes need (at most %d bytes of slack)",
					RelationGetRelationName(vs->index), what, off, blk, avail,
					avail - size, size, LION_ITEM_SLACK_BOUND);
}

/*
 * Check one sparse segment (DESIGN.md §13).
 *
 * Besides its own structure, a segment has to respect the two rules that
 * make the rest of the index able to ignore it: its range must start above
 * everything before it (the caller keeps the last ckey of the previous item
 * in *prevckey, so this also proves the ranges do not overlap or interleave,
 * within a page and across a chain), and no container key inside it may have
 * reached LION_SPARSE_THRESHOLD members, because such a key belongs in a
 * container of its own.
 */
static void
lion_verify_segment(LionVerifyState *vs, BlockNumber blk, OffsetNumber off,
				   const LionContainer *c, Size avail, bool *haveprev,
				   uint32 *prevckey)
{
	const char *detail = NULL;
	LionSparseIter it;
	uint32		n = c->cardinality;
	uint32		run = 0;
	uint32		runckey = 0;

	if (!lion_sparse_check(c, avail, &detail) ||
		!lion_sparse_check_offsets(c, MaxHeapTuplesPerPage, &detail))
		lion_corrupt("lion index \"%s\": sparse segment %u on block %u is corrupt: %s",
					RelationGetRelationName(vs->index), off, blk, detail);

	lion_verify_item_slack(vs, blk, off, c, avail, "sparse segment");

	if (n == 0)
		lion_corrupt("lion index \"%s\": sparse segment %u on block %u is empty",
					RelationGetRelationName(vs->index), off, blk);

	if (*haveprev && c->ckey <= *prevckey)
		lion_corrupt("lion index \"%s\": sparse segment %u on block %u starts at container key %u, not above the previous key %u",
					RelationGetRelationName(vs->index), off, blk, c->ckey,
					*prevckey);

	/* lion_sparse_check() has decoded the stream and found it sound */
	lion_sparse_iter_init(&it, c);
	while (lion_sparse_iter_next(&it))
	{
		if (run > 0 && lion_sparse_iter_ckey(&it) == runckey)
			run++;
		else
		{
			runckey = lion_sparse_iter_ckey(&it);
			run = 1;
		}
		if (run >= LION_SPARSE_THRESHOLD)
			lion_corrupt("lion index \"%s\": sparse segment %u on block %u holds %u members of container key %u, which needs a container of its own",
						RelationGetRelationName(vs->index), off, blk, run,
						runckey);
	}

	*prevckey = lion_item_last_ckey(c);
	*haveprev = true;
}

/*
 * Check one item that is on a container page or inside an INLINE payload:
 * a container, or a sparse segment.
 */
static void
lion_verify_container(LionVerifyState *vs, BlockNumber blk, OffsetNumber off,
					 const LionContainer *c, Size avail, bool *haveprev,
					 uint32 *prevckey)
{
	const char *detail = NULL;

	/* the header has to be there before its type can say what follows */
	if (avail < LION_CONTAINER_HDRSZ)
		lion_corrupt("lion index \"%s\": item %u on block %u is only %zu bytes, less than an item header",
					RelationGetRelationName(vs->index), off, blk, avail);

	if (c->type == LION_CT_SPARSE)
	{
		lion_verify_segment(vs, blk, off, c, avail, haveprev, prevckey);
		return;
	}

	/*
	 * Its structure, and then its members as heap TIDs (DESIGN.md §2): a
	 * member at offset 0 or past MaxHeapTuplesPerPage used to pass, and every
	 * query that turned the key into TIDs failed on it.
	 */
	if (!lion_container_check(c, avail, &detail) ||
		!lion_container_check_offsets(c, MaxHeapTuplesPerPage, &detail))
		lion_corrupt("lion index \"%s\": container %u on block %u is corrupt: %s",
					RelationGetRelationName(vs->index), off, blk, detail);

	lion_verify_item_slack(vs, blk, off, c, avail, "container");

	if (c->cardinality == 0)
		lion_corrupt("lion index \"%s\": container %u on block %u is empty",
					RelationGetRelationName(vs->index), off, blk);

	if (*haveprev && c->ckey <= *prevckey)
		lion_corrupt("lion index \"%s\": container %u on block %u has container key %u, not above the previous key %u",
					RelationGetRelationName(vs->index), off, blk, c->ckey,
					*prevckey);

	*prevckey = c->ckey;
	*haveprev = true;
}

/*
 * One page of a level of a posting set or a position tree, as the walk of
 * that level found it (DESIGN.md §22): what the comparison with the level
 * above needs of it (lion_verify_match_downlink()).  A walk writes its
 * level's pages into a sequence (LionVerifySeq) and the walk of the level
 * above reads them back in order, so that it keeps neither level in memory.
 */
typedef struct LionVerifyPage
{
	BlockNumber blk;
	uint32		firstkey;		/* a leaf's first key, or the first separator */
	uint32		upper;			/* a leaf's last key, or the high key */
	uint16		flags;			/* LION_VPAGE_* */
} LionVerifyPage;

#define LION_VPAGE_FIRST		0x01	/* firstkey is there: not an empty leaf */
#define LION_VPAGE_UPPER		0x02	/* upper is there: a leaf with keys, or
										 * an internal page with a high key */
#define LION_VPAGE_INCOMPLETE	0x04	/* flagged LION_PAGE_INCOMPLETE_SPLIT */

/*
 * What the walk of a tree's leaves counts for the checks of a tree without
 * an internal level (lion_verify_flat()).
 */
typedef struct LionVerifyLeafSum
{
	int64		npages;
	BlockNumber flagged;		/* the first leaf flagged as an unfinished
								 * split, or InvalidBlockNumber */
} LionVerifyLeafSum;

/*
 * The tree a walk is of: the posting set of a chain entry, or the entry's
 * position tree (DESIGN.md §17).  A position tree is a posting tree in all
 * but three things - every page of it carries LION_PAGE_POSITIONS and the
 * tree's own root as its owner head, its leaves hold position chunks, and its
 * keys are heap blocks, which repeat across leaves (lion_postree.c) - so the
 * two are walked, and their levels compared, by the same code.
 */
typedef struct LionVerifyTree
{
	BlockNumber eblk;			/* the entry, for the messages */
	OffsetNumber eoff;
	BlockNumber root;			/* every page's owner_head */
	uint32		owner;			/* ... and owner_hash */
	bool		positions;		/* a position tree */
	const char *what;			/* "posting" or "position" */
} LionVerifyTree;

static void
lion_verify_tree_init(LionVerifyTree *tree, BlockNumber eblk,
					  OffsetNumber eoff, const LionEntryTuple *entry,
					  bool positions)
{
	tree->eblk = eblk;
	tree->eoff = eoff;
	tree->root = positions ? lion_entry_posext(entry)->pos_root :
		lion_entry_head(entry);
	tree->owner = lion_entry_owner(entry);
	tree->positions = positions;
	tree->what = positions ? "position" : "posting";
}

/* What a flagged page is a page of, for its WARNING (LION_VFLAG_*). */
static const char *
lion_verify_flag_what(uint16 what)
{
	switch (what)
	{
		case LION_VFLAG_DIR:
			return "directory";
		case LION_VFLAG_POSTING:
			return "posting";
		case LION_VFLAG_POSITION:
			return "position";
	}
	elog(ERROR, "lion index: %u is not the kind of a flagged page", what);
	return NULL;				/* keep the compiler quiet */
}

/*
 * The unfinished-split WARNING (DESIGN.md §21, §22).  A split writes its new
 * right sibling and the flag in one record, the sibling's downlink in the
 * parent in the next, and clears the flag in a third: a crash or an error
 * after the first or the second leaves the flag, with or without the
 * downlink, and the next writer whose descent passes through the page does
 * what is missing (lion_dir_downlink_present(),
 * lion_posting_downlink_present()).  Only a descent through the page does
 * it: an append that fits a posting set's last leaf goes there without one
 * (lion_insert.c), so a flag on a leaf below the inserts' range can outlive
 * any number of inserts into the key.
 */
static void
lion_verify_warn_incomplete(LionVerifyState *vs, uint16 what,
							BlockNumber blk)
{
	ereport(WARNING,
			(errmsg("lion index \"%s\": %s page %u has an unfinished split",
					RelationGetRelationName(vs->index),
					lion_verify_flag_what(what), blk),
			 errdetail("The split that flagged it did not finish: the downlink of its right sibling, or the record that clears the flag after it, was never written."),
			 errhint("The next INSERT that descends to this page repairs it.")));
}

/*
 * A page a walk that is kept found flagged as an unfinished split, read with
 * LSN lsn.
 *
 * On a primary that is a split that was abandoned, never one in progress: a
 * writer holds the page it flags EXCLUSIVE from the record that sets the
 * flag to the one that clears it, and the walk read the page under a SHARE
 * lock.  So the WARNING is given at once.
 *
 * On a standby it is not: replay locks each record's pages for that record
 * alone, so a split the primary is in the middle of - between its first
 * record and its last - shows the flag there to whoever reads the page.  So
 * the page is noted, with the next transaction id at the time - every
 * transaction that can have written the flag has a lower one - and looked at
 * again at the end of the check (lion_verify_flags_recheck()).
 */
static void
lion_verify_flag_add(LionVerifyState *vs, const LionVerifyFlag *f)
{
	if (vs->concurrent)
		lion_verify_warn_incomplete(vs, f->what, f->blk);
	else
		lion_verify_seq_put(&vs->flags, f);
}

void
lion_verify_flagged(LionVerifyState *vs, uint16 what, BlockNumber blk,
					XLogRecPtr lsn)
{
	LionVerifyFlag f;

	memset(&f, 0, sizeof(f));
	f.blk = blk;
	f.what = what;
	f.lsn = lsn;
	f.next = ReadNextFullTransactionId();
	lion_verify_flag_add(vs, &f);
}

/*
 * A standby's flagged pages, at the end of the check: the WARNING for each
 * that still carries the flag, with the LSN it had - no record has changed
 * the page since, the one that clears the flag among them - and whose
 * flag no running transaction can still be about to clear: every
 * transaction older than the next transaction id noted with the page has
 * ended (a fresh snapshot's xmin is past it), so the split that flagged it
 * was abandoned, by an error or by a crash of the primary.  Every other
 * flag is a split that finished since, or may yet, and is not warned of.
 * The pages are read with nothing else held, and one that is not a lion
 * page of the kind it was any more - freed, or reused - is not flagged.
 */
void
lion_verify_flags_recheck(LionVerifyState *vs)
{
	LionVerifyFlag f;
	TransactionId xmin = InvalidTransactionId;

	if (vs->concurrent || vs->flags.n == 0)
		return;

	/*
	 * Test hook: a standby's check has walked everything and is about to
	 * look at the flagged pages it found again.  test/recovery/run.sh phase
	 * 3 parks here with a split it saw half done on the primary, and lets
	 * the split finish first.
	 */
	LION_INJECTION_POINT("lion-verify-flags-recheck");

	lion_verify_seq_rewind(&vs->flags);
	while (lion_verify_seq_get(&vs->flags, &f))
	{
		Buffer		buf;
		Page		page;
		bool		still;

		CHECK_FOR_INTERRUPTS();
		vs->nflags++;
		if (!lion_verify_block_exists(vs, f.blk))
			continue;
		buf = ReadBuffer(vs->index, f.blk);
		LockBuffer(buf, BUFFER_LOCK_SHARE);
		page = BufferGetPage(buf);
		still = !PageIsNew(page) &&
			PageGetSpecialSize(page) == LION_SPECIAL_SIZE &&
			LionPageGetOpaque(page)->page_id == LION_PAGE_ID &&
			LionPageIncompleteSplit(page) &&
			BufferGetLSNAtomic(buf) == f.lsn;
		UnlockReleaseBuffer(buf);
		if (!still)
			continue;

		/* one snapshot for every page: its xmin only grows */
		if (!TransactionIdIsValid(xmin))
			xmin = GetLatestSnapshot()->xmin;
		if (TransactionIdPrecedes(xmin, XidFromFullTransactionId(f.next)))
			continue;
		lion_verify_warn_incomplete(vs, f.what, f.blk);
	}
	lion_verify_seq_reset(&vs->flags);
}

/* A walk's result, empty, and what it keeps given back. */
static void
lion_verify_result_init(LionVerifyState *vs, LionVerifySetResult *res)
{
	memset(res, 0, sizeof(*res));
	lion_verify_seq_init(&res->incomplete, &vs->seqacct,
						 sizeof(LionVerifyFlag));
}

static void
lion_verify_result_free(LionVerifySetResult *res)
{
	lion_verify_seq_free(&res->incomplete);
}

/*
 * A page a walk that may be thrown away found flagged, read with LSN lsn,
 * for lion_verify_flagged() once the walk is kept
 * (lion_verify_warn_flagged()); an exact walk has given its WARNING already
 * (lion_verify_tree_page()).  Called with the page let go: the list may be
 * in a file.
 */
static void
lion_verify_note_flagged(LionVerifySetResult *res, const LionVerifyTree *tree,
						 BlockNumber blk, XLogRecPtr lsn, bool exact)
{
	LionVerifyFlag f;

	if (exact)
		return;
	memset(&f, 0, sizeof(f));
	f.blk = blk;
	f.what = tree->positions ? LION_VFLAG_POSITION : LION_VFLAG_POSTING;
	f.lsn = lsn;
	f.next = ReadNextFullTransactionId();
	lion_verify_seq_put(&res->incomplete, &f);
}

/* The flagged pages of a walk that is kept (lion_verify_flagged()). */
static void
lion_verify_warn_flagged(LionVerifyState *vs, LionVerifySetResult *res)
{
	LionVerifyFlag f;

	lion_verify_seq_rewind(&res->incomplete);
	while (lion_verify_seq_get(&res->incomplete, &f))
		lion_verify_flag_add(vs, &f);
}

/*
 * The page kind and owner checks every page of a posting set or a position
 * tree goes through.
 *
 * With `exact` false the walk is one lion_verify_set() may throw away and
 * repeat, because writers of the key can be changing the set under it; then
 * a check here may be a reason to walk again instead of an error, and NULL
 * comes back with the buffer released.  On a primary that is one check: the
 * ROOT at another level than the descent found it at, which is what a
 * push-down of the root does (DESIGN.md §22: the root keeps its block and
 * becomes the level above).  Every other check here is exact there whatever
 * writers do: VACUUM, the only thing that frees a page or takes a leaf out
 * of its set, is locked out, so a page reached through a link of the set
 * belongs to the set for as long as the check runs, and no page but the root
 * ever changes level.
 *
 * Not on a standby, where replay applies VACUUM beside the walk: a leaf
 * taken out of its set (§22, "Empty leaves") or a whole set freed after the
 * walk read the link that leads here leaves that link pointing at a page
 * that is UNLINKED or DELETED by the time the walk gets to it.  There every
 * check of this page against the tree is a reason to walk again once the
 * walk has read a page past its replay position, this one included
 * (lion_verify_moved()), and damage only when it has not: then the walk read
 * the link and this page as they were at one point of the WAL.
 */
static Page
lion_verify_tree_page(LionVerifyState *vs, BlockNumber blk,
					  const LionVerifyTree *tree, uint16 level, bool exact,
					  Buffer *bufp)
{
	Page		page;
	LionPageOpaque opaque;
	bool		moved;

	lion_verify_visit(vs, blk, tree->positions ? "a position tree page" :
					  "a posting tree");
	page = lion_verify_read_page(vs, blk, LION_PAGE_CONTAINER, bufp);
	opaque = LionPageGetOpaque(page);
	moved = !exact && lion_verify_moved(vs);

	/*
	 * DESIGN.md §18.  A live entry must not reach a freed page at all, and
	 * every page of a posting set has to name that set: readers rely on both
	 * to tell a set they still hold a link to from one whose pages have been
	 * handed to somebody else.  A position tree's pages name its root, with
	 * the entry's own stamp.
	 */
	if (LionPageIsDeleted(page))
	{
		if (moved)
			goto again;
		lion_corrupt("lion index \"%s\": block %u is reachable from chain entry %u on block %u but is marked deleted",
					RelationGetRelationName(vs->index), blk, tree->eoff,
					tree->eblk);
	}

	/*
	 * DESIGN.md §22, "Empty leaves": a leaf VACUUM took out of the set is
	 * linked to by nothing of it, and will be handed out again.  (No page
	 * of a position tree is ever taken out: lion_verify_page_flags().)
	 */
	if (LionPageIsUnlinked(page))
	{
		if (moved)
			goto again;
		lion_corrupt("lion index \"%s\": block %u is reachable from chain entry %u on block %u but was taken out of the posting set",
					RelationGetRelationName(vs->index), blk, tree->eoff,
					tree->eblk);
	}

	if (opaque->owner_head != tree->root || opaque->owner_hash != tree->owner)
	{
		if (moved)
			goto again;
		lion_corrupt("lion index \"%s\": block %u of chain entry %u on block %u is owned by stamp %u at head %u, expected stamp %u at head %u",
					RelationGetRelationName(vs->index), blk, tree->eoff,
					tree->eblk, opaque->owner_hash, opaque->owner_head,
					tree->owner, tree->root);
	}

	/*
	 * ... and is a page of the tree's own kind (DESIGN.md §17).  The owner
	 * stamp alone tells the two apart on every page a writer made, a position
	 * tree being stamped with its own root, but the readers go by the flag
	 * too: a reader of a posting set refuses a page that has it
	 * (lion_page_owns()), one of a position tree a page that lacks it
	 * (lion_page_owns_positions()), and either ends its walk there - a
	 * damaged flag is as much a lost leaf as a damaged link.
	 */
	if (((opaque->flags & LION_PAGE_POSITIONS) != 0) != tree->positions)
	{
		if (moved)
			goto again;
		if (tree->positions)
			lion_corrupt("lion index \"%s\": block %u of the position tree of chain entry %u on block %u is not flagged as a page of a position tree",
						RelationGetRelationName(vs->index), blk, tree->eoff,
						tree->eblk);
		lion_corrupt("lion index \"%s\": block %u of chain entry %u on block %u is flagged as a page of a position tree",
					RelationGetRelationName(vs->index), blk, tree->eoff,
					tree->eblk);
	}

	if (opaque->level != level)
	{
		if ((!exact && blk == tree->root) || moved)
			goto again;
		lion_corrupt("lion index \"%s\": %s page %u of chain entry %u on block %u is at level %u, expected %u",
					RelationGetRelationName(vs->index), tree->what, blk,
					tree->eoff, tree->eblk, opaque->level, level);
	}

	/*
	 * A flag seen under a SHARE lock on a primary is a split that was
	 * abandoned, never one in progress: a writer holds the flagged page
	 * EXCLUSIVE from the record that sets the flag to the one that clears it
	 * (DESIGN.md §22).  A walk that may be repeated says so only once it is
	 * kept, which lion_verify_chain_totals() does (lion_verify_postree(), for
	 * a position tree); its caller notes the page for that once it has let it
	 * go (lion_verify_note_flagged()), and a standby looks at it again at the
	 * end of the check (lion_verify_flagged()).
	 *
	 * The flag is only ever on the LEFT half of a split, which the record
	 * that sets it links to the split's new page, and which keeps that right
	 * sibling while the flag lasts: VACUUM takes no empty leaf whose left
	 * neighbour is flagged, nor a flagged one (§22, "Empty leaves"), and a
	 * root pushed down flags nothing.  So a rightmost page with the flag - a
	 * root above the leaves among them - is damage in any walk, on a standby
	 * too, where it is one page as one record left it: not the state the
	 * next INSERT repairs, which finds no sibling to post.
	 */
	if (LionPageIncompleteSplit(page) && LionPageIsRightmost(page))
		lion_corrupt("lion index \"%s\": %s page %u of chain entry %u on block %u is flagged as an unfinished split, but it is the rightmost page of its level, which no split leaves flagged",
					RelationGetRelationName(vs->index), tree->what, blk,
					tree->eoff, tree->eblk);
	if (LionPageIncompleteSplit(page) && exact)
		lion_verify_warn_incomplete(vs, tree->positions ? LION_VFLAG_POSITION :
									LION_VFLAG_POSTING, blk);

	return page;

again:
	UnlockReleaseBuffer(*bufp);
	*bufp = InvalidBuffer;
	return NULL;
}

/*
 * Walk the LEAF chain of a posting set, left to right: the items of each page
 * and the ascending ckey order within and across pages.  Returns false when
 * the walk has to be repeated (see lion_verify_tree_page()).
 *
 * The order across pages is exact even with the key's writers running: a
 * split moves items only onto a page it links immediately right of the page
 * they came from, so everything the walk read on one page is below everything
 * on the page it read that page's right link from.  So is the set of items
 * the walk sees: an item that moves right in a split the walk has not reached
 * yet is met on its new page, one that moves after the walk read its page was
 * seen there.  Both hold beside the VACUUM a standby replays too, which only
 * ever takes items out, and a leaf it empties out of the set - which a walk
 * that read the link to it before is sent round again for
 * (lion_verify_tree_page()).  What is NOT exact while writers run is how
 * many items there are and which leaf is the last, so those are only
 * collected here and compared by lion_verify_chain_totals() - except in an
 * exact walk, which checks the tail at once, as it always did.
 *
 * Each page's LionVerifyPage goes into out for the comparison with the level
 * above; an exact walk passes none, and that comparison reads the leaves
 * again instead (LionVerifyBelow).
 */
static bool
lion_verify_posting_leaves(LionVerifyState *vs, const LionVerifyTree *tree,
						   const LionEntryTuple *entry, BlockNumber first,
						   LionVerifySeq *out, LionVerifyLeafSum *sum,
						   bool exact, LionVerifySetResult *res)
{
	BlockNumber blk = first;
	BlockNumber last = InvalidBlockNumber;
	bool		haveprev = false;
	uint32		prevckey = 0;

	sum->npages = 0;
	sum->flagged = InvalidBlockNumber;
	while (BlockNumberIsValid(blk))
	{
		Buffer		buf;
		Page		page;
		LionPageOpaque opaque;
		OffsetNumber maxoff;
		OffsetNumber off;
		OffsetNumber firstused = InvalidOffsetNumber;
		OffsetNumber lastused = InvalidOffsetNumber;
		uint32		minckey = 0;
		uint32		maxckey = 0;
		LionVerifyPage pg;
		XLogRecPtr	lsn;

		page = lion_verify_tree_page(vs, blk, tree, 0, exact, &buf);
		if (page == NULL)
			return false;
		opaque = LionPageGetOpaque(page);
		maxoff = PageGetMaxOffsetNumber(page);

		for (off = FirstOffsetNumber; off <= maxoff; off++)
		{
			ItemId		iid = lion_verify_itemid(vs, blk, page, off);
			LionContainer *c;

			if (!ItemIdIsUsed(iid))
				continue;

			c = (LionContainer *) PageGetItem(page, iid);
			lion_verify_container(vs, blk, off, c, ItemIdGetLength(iid),
								 &haveprev, &prevckey);

			res->card += c->cardinality;
			res->ncontainers++;

			if (firstused == InvalidOffsetNumber)
				firstused = off;
			lastused = off;
		}

		/*
		 * No two items overlap: each is inside the page and within its slack,
		 * and yet one may reach into the next, which is what a writer then
		 * overwrites - so every writer refuses such a page
		 * (lion_page_check_items()), and this says so first.
		 */
		lion_page_check_items(vs->index, page, blk);

		/* min/max in the special area must describe the items */
		if (firstused == InvalidOffsetNumber)
		{
			if (opaque->minckey != 0 || opaque->maxckey != 0)
				lion_corrupt("lion index \"%s\": empty container page %u has minckey %u and maxckey %u, expected 0 and 0",
							RelationGetRelationName(vs->index), blk,
							opaque->minckey, opaque->maxckey);
		}
		else
		{
			minckey =
				lion_item_first_ckey((LionContainer *)
									PageGetItem(page, PageGetItemId(page, firstused)));
			maxckey =
				lion_item_last_ckey((LionContainer *)
								   PageGetItem(page, PageGetItemId(page, lastused)));

			if (opaque->minckey != minckey || opaque->maxckey != maxckey)
				lion_corrupt("lion index \"%s\": container page %u has minckey %u and maxckey %u, but holds %u .. %u",
							RelationGetRelationName(vs->index), blk,
							opaque->minckey, opaque->maxckey, minckey, maxckey);
		}

		pg.blk = blk;
		pg.firstkey = minckey;
		pg.upper = maxckey;
		pg.flags = (firstused != InvalidOffsetNumber) ?
			(LION_VPAGE_FIRST | LION_VPAGE_UPPER) : 0;
		if (LionPageIncompleteSplit(page))
			pg.flags |= LION_VPAGE_INCOMPLETE;
		lsn = BufferGetLSNAtomic(buf);

		last = blk;
		blk = opaque->rightlink;
		UnlockReleaseBuffer(buf);

		/* written with the page let go: the sequence may be in a file */
		if (out != NULL)
			lion_verify_seq_put(out, &pg);
		sum->npages++;
		if ((pg.flags & LION_VPAGE_INCOMPLETE) != 0)
		{
			if (!BlockNumberIsValid(sum->flagged))
				sum->flagged = pg.blk;
			lion_verify_note_flagged(res, tree, pg.blk, lsn, exact);
		}

		/*
		 * Test hook: a leaf of the set is read, and nothing is held.
		 * test/recovery/run.sh phase 3 parks a standby's check here and has
		 * replay change the set's other leaves and leave its entry as it was
		 * (lion_verify_set_walks()).  Never in an exact walk, which holds a
		 * directory leaf.
		 */
		if (!exact)
			LION_INJECTION_POINT("lion-verify-set-leaf-read");

		CHECK_FOR_INTERRUPTS();
	}

	res->last = last;
	if (exact && last != lion_entry_tail(entry))
		lion_corrupt("lion index \"%s\": chain entry %u on block %u ends at block %u, but its tail is block %u",
					RelationGetRelationName(vs->index), tree->eoff,
					tree->eblk, last, lion_entry_tail(entry));
	return true;
}

/*
 * Walk the LEAF chain of a position tree, left to right (DESIGN.md §17):
 * every chunk well formed, the members ascending across the whole chain and
 * each chunk's reaching no further than the next one's header ((P1) and (P2)
 * of lion_postree.c), and every leaf's min and max what its chunks make them
 * (lion_page_update_minmax()) - a tree without an internal level is routed
 * by its leaves' mins (lion_posting_flat_route()).  The members are counted
 * in res->card.  Returns false when the walk has to be repeated
 * (lion_verify_tree_page()); the order across pages is exact for the reason
 * lion_verify_posting_leaves() gives.
 */
static bool
lion_verify_pos_leaves(LionVerifyState *vs, const LionVerifyTree *tree,
					   BlockNumber first, LionVerifySeq *out,
					   LionVerifyLeafSum *sum, bool exact,
					   LionVerifySetResult *res)
{
	BlockNumber blk = first;
	LionPosMember *m = (LionPosMember *) palloc(sizeof(LionPosMember));
	char		err[256];
	bool		any = false;
	uint64		lastcode = 0;
	uint32		lastblock = 0;
	bool		havechunk = false;
	uint32		lastheader = 0;

	sum->npages = 0;
	sum->flagged = InvalidBlockNumber;
	while (BlockNumberIsValid(blk))
	{
		Buffer		buf;
		Page		page;
		LionPageOpaque opaque;
		OffsetNumber maxoff;
		OffsetNumber o;
		LionContainer *c = NULL;
		uint32		minckey = 0;
		uint32		maxckey = 0;
		LionVerifyPage pg;
		XLogRecPtr	lsn;

		page = lion_verify_tree_page(vs, blk, tree, 0, exact, &buf);
		if (page == NULL)
		{
			pfree(m);
			return false;
		}
		opaque = LionPageGetOpaque(page);
		maxoff = PageGetMaxOffsetNumber(page);

		for (o = FirstOffsetNumber; o <= maxoff; o++)
		{
			LionPosIter it;
			bool		firstmember = true;

			c = lion_page_poschunk_fetch(vs->index, page, blk, o);
			if (!lion_poschunk_check(c, ItemIdGetLength(PageGetItemId(page, o)),
									 err, sizeof(err)))
				lion_corrupt("lion index \"%s\": chunk %u on position page %u: %s",
							RelationGetRelationName(vs->index), o, blk, err);
			if ((havechunk && c->ckey < lastheader) ||
				(any && lastblock > c->ckey))
				lion_corrupt("lion index \"%s\": chunk %u on position page %u, of header block %u, is out of order",
							RelationGetRelationName(vs->index), o, blk,
							c->ckey);
			havechunk = true;
			lastheader = c->ckey;
			lion_poschunk_iter_init(&it, c);
			while (lion_poschunk_iter_next(&it, m))
			{
				if ((firstmember && lion_pos_block(m->code) < c->ckey) ||
					(any && m->code <= lastcode))
					lion_corrupt("lion index \"%s\": the members of position page %u are out of order",
								RelationGetRelationName(vs->index), blk);
				firstmember = false;
				any = true;
				lastcode = m->code;
				lastblock = lion_pos_block(m->code);
				res->card++;
			}
		}

		/* No two chunks overlap, as on every page a writer splits. */
		lion_page_check_items(vs->index, page, blk);

		if (c != NULL)
		{
			uint64		code;

			minckey = lion_page_poschunk_fetch(vs->index, page, blk,
											   FirstOffsetNumber)->ckey;
			maxckey = lion_poschunk_last_code(c, &code) ?
				lion_pos_block(code) : c->ckey;
		}
		if (opaque->minckey != minckey || opaque->maxckey != maxckey)
			lion_corrupt("lion index \"%s\": position page %u has minckey %u and maxckey %u, but its chunks make them %u and %u",
						RelationGetRelationName(vs->index), blk,
						opaque->minckey, opaque->maxckey, minckey, maxckey);

		pg.blk = blk;
		pg.firstkey = minckey;
		pg.upper = maxckey;
		pg.flags = (c != NULL) ? (LION_VPAGE_FIRST | LION_VPAGE_UPPER) : 0;
		if (LionPageIncompleteSplit(page))
			pg.flags |= LION_VPAGE_INCOMPLETE;
		lsn = BufferGetLSNAtomic(buf);

		blk = opaque->rightlink;
		UnlockReleaseBuffer(buf);

		/* written with the page let go: the sequence may be in a file */
		if (out != NULL)
			lion_verify_seq_put(out, &pg);
		sum->npages++;
		if ((pg.flags & LION_VPAGE_INCOMPLETE) != 0)
		{
			if (!BlockNumberIsValid(sum->flagged))
				sum->flagged = pg.blk;
			lion_verify_note_flagged(res, tree, pg.blk, lsn, exact);
		}

		CHECK_FOR_INTERRUPTS();
	}

	pfree(m);
	return true;
}

/*
 * The comparison of level i of a tree with the level below it, made as the
 * walk of level i meets its downlinks (lion_verify_tree_level()), against
 * the pages of level i - 1 in the order ITS walk found them, read back from
 * their sequence: neither level is kept in memory.
 *
 * Every downlink but the first carries the separator of its child: at or
 * below the child's first key, and above everything the page to its left
 * holds.  A position tree's keys are heap blocks, and its separators repeat
 * (lion_postree.c): there a leaf's separator IS the header of its first
 * chunk - (P3), which a split keeps by posting that header and VACUUM by
 * never deleting a leaf's first chunk - and the leaf to its left may hold
 * members of that very block, but none above it, since a reader of block X
 * lands on the leaf of the last separator below X and looks for X's members
 * from there on, never to its left.
 *
 * Each of them is bounded above by the high key of its parent as well, when
 * the parent has one: the next downlink, the first of the parent's right
 * sibling, gives the bound the level as a whole keeps, and the parent's high
 * key the one a reader that comes down through the parent relies on.  The two
 * are one key while the parent's level is sound - an internal split gives the
 * left half the right half's first separator as its high key - and a parent
 * whose high key was lowered under its last child's keys, with the separator
 * above it that routes to the parent's right sibling, hides those keys from
 * every reader while the level, read as one sequence, stays in order.
 *
 * One page may lack a downlink without being damage: the right sibling a
 * split made, while the split is unfinished.  The split writes the sibling
 * in one record and its downlink in the next, and leaves the LEFT page
 * flagged LION_PAGE_INCOMPLETE_SPLIT in between; a crash - or an error -
 * there leaves it so until the next writer that descends to the left page
 * finishes it (DESIGN.md §22).  The sibling is then reachable through its
 * left neighbour's right link and from nothing above, and the walk has
 * already warned about the flag.  A three-way split flags both of its first
 * two pages, so each missing downlink is covered by the page to its own
 * left.  Such a page lies in its left neighbour's key range until the split
 * finishes, and is bounded above by the next downlink's separator like any
 * other.
 *
 * In a walk that may be repeated, a downlink to a page the walk of the
 * level below did not see is a split made after that walk passed: the set
 * is walked again (see the header of lion_verify_chain()), and nothing else
 * the comparison found stands, so the first mismatch is only reported once
 * the walk of level i has met every downlink and none of them was such a
 * page - which is also why a mismatch waits until the walk of the level is
 * done, as the damage that walk finds on a page of its own is reported
 * first.  Every page the walk of the level below saw is marked in refs, and
 * so is every page any walk saw (lion_verify_visit()), which no downlink of
 * the tree may name either: a downlink to an unmarked page is the one thing
 * a split behind the walk shows.  In an exact walk nothing can have split,
 * and the comparison reports such a downlink as the mismatch it is.
 *
 * On a standby the same holds of a walk that read a page past its replay
 * position (lion_verify_moved()): replay may have split a page, or unlinked
 * an empty leaf - which retargets its downlink to its right sibling - behind
 * either walk, so a downlink to an unmarked page and any mismatch mean
 * walking again.  A walk that read every page at or below its position read
 * both levels at one point of the WAL, where they agree on a sound index,
 * and is held to them as an exact one is.
 */

/*
 * The pages of level i - 1, as the comparison with level i reads them back
 * in the order the walk of level i - 1 found them: from that walk's
 * sequence, or - in an exact walk - from the index itself, along the level's
 * right links from its leftmost page.  An exact walk holds the entry's
 * directory leaf, so it must not wait for file I/O, which a sequence past its
 * share of work_mem costs (2026-10-23 review), and it need not: the writers
 * of the set wait for the leaf, and VACUUM for the check, so the pages are
 * what that walk read, and reading one again costs a buffer hit.
 */
typedef struct LionVerifyBelow
{
	LionVerifySeq *seq;			/* a walk that may be thrown away, or NULL */
	BlockNumber next;			/* an exact walk: the next page to read */
	uint16		level;			/* ... of this level */
	bool		positions;		/* ... of a position tree */
	int64		n;				/* the pages of the level */
} LionVerifyBelow;

/*
 * What the comparison needs of one page of level `level`, read from the page
 * itself: what the walk of that level collected of it, from the same fields
 * it checked them against (lion_verify_posting_leaves(),
 * lion_verify_pos_leaves(), lion_verify_tree_level()).
 */
static void
lion_verify_page_summary(LionVerifyState *vs, BlockNumber blk, Page page,
						 uint16 level, bool positions, LionVerifyPage *pg)
{
	LionPageOpaque opaque = LionPageGetOpaque(page);
	OffsetNumber maxoff = PageGetMaxOffsetNumber(page);

	memset(pg, 0, sizeof(*pg));
	pg->blk = blk;
	if (level == 0)
	{
		bool		any = false;
		OffsetNumber off;

		/* a posting leaf may keep unused line pointers; a position leaf not */
		for (off = FirstOffsetNumber; off <= maxoff && !any; off++)
			any = positions ||
				ItemIdIsUsed(lion_verify_itemid(vs, blk, page, off));
		if (any)
		{
			pg->firstkey = opaque->minckey;
			pg->upper = opaque->maxckey;
			pg->flags = LION_VPAGE_FIRST | LION_VPAGE_UPPER;
		}
	}
	else
	{
		OffsetNumber firstdata = lion_posting_first_data(page);

		if (!LionPageIsRightmost(page) && maxoff >= FirstOffsetNumber)
		{
			pg->upper = lion_posting_pivot(page, FirstOffsetNumber)->ckey;
			pg->flags |= LION_VPAGE_UPPER;
		}
		if (firstdata <= maxoff)
		{
			pg->firstkey = lion_posting_pivot(page, firstdata)->ckey;
			pg->flags |= LION_VPAGE_FIRST;
		}
	}
	if (LionPageIncompleteSplit(page))
		pg->flags |= LION_VPAGE_INCOMPLETE;
}

/* The next page of the level below into pg, or false when there is none. */
static bool
lion_verify_below_get(LionVerifyState *vs, LionVerifyBelow *b,
					  LionVerifyPage *pg)
{
	Buffer		buf;
	Page		page;

	if (b->seq != NULL)
		return lion_verify_seq_get(b->seq, pg);
	if (!BlockNumberIsValid(b->next))
		return false;
	page = lion_verify_read_page(vs, b->next, LION_PAGE_CONTAINER, &buf);
	if (LionPageGetOpaque(page)->level != b->level)
		elog(ERROR, "lion index \"%s\": block %u changed level under an exact walk",
			 RelationGetRelationName(vs->index), b->next);
	lion_verify_page_summary(vs, b->next, page, b->level, b->positions, pg);
	b->next = LionPageGetOpaque(page)->rightlink;
	UnlockReleaseBuffer(buf);
	return true;
}

typedef struct LionVerifyMatch
{
	LionVerifyState *vs;
	const LionVerifyTree *tree;
	uint32		level;			/* i, the level whose downlinks are matched */
	bool		exact;
	LionVerifyBelow *below;		/* the pages of level i - 1, in order */
	int64		ndownlinks;		/* downlinks met so far */
	LionVerifyPage open;		/* the page the last downlink named, whose */
	bool		haveopen;		/* ... upper bound the next one gives */
	BlockNumber parent;			/* the page of level i that named it, once */
	uint32		parenthigh;		/* ... that page's walk is over and it has a */
								/* high key, until the next downlink matches */
	LionVerifyPage prev;		/* the page of level i - 1 read last */
	bool		haveprev;
	bool		unseen;			/* a downlink named a page no walk reached */
	bool		countoff;		/* the downlinks and the pages do not pair */
	char	   *failure;		/* the first mismatch, reported at the end */
} LionVerifyMatch;

static void
lion_verify_match_init(LionVerifyMatch *m, LionVerifyState *vs,
					   const LionVerifyTree *tree, uint32 level,
					   LionVerifyBelow *below, bool exact)
{
	memset(m, 0, sizeof(*m));
	m->vs = vs;
	m->tree = tree;
	m->level = level;
	m->exact = exact;
	m->below = below;
	m->parent = InvalidBlockNumber;
	if (below->seq != NULL)
		lion_verify_seq_rewind(below->seq);
}

/* Keep the first mismatch's message, for lion_verify_match_end(). */
static void lion_verify_match_fail(LionVerifyMatch *m, const char *fmt,...) pg_attribute_printf(2, 3);

static void
lion_verify_match_fail(LionVerifyMatch *m, const char *fmt,...)
{
	StringInfoData buf;

	if (m->failure != NULL)
		return;
	initStringInfo(&buf);
	for (;;)
	{
		va_list		args;
		int			needed;

		va_start(args, fmt);
		needed = appendStringInfoVA(&buf, fmt, args);
		va_end(args);
		if (needed == 0)
			break;
		enlargeStringInfo(&buf, needed);
	}
	m->failure = buf.data;
}

/*
 * The page's own upper bound - a leaf's last key, an internal page's high
 * key - against the high key of its parent, when the parent's walk is over
 * and it has one, and against `next`, the separator of the downlink after
 * the one that names it.  False once there is a mismatch.
 */
static bool
lion_verify_match_upper(LionVerifyMatch *m, const LionVerifyPage *pg,
						uint32 next)
{
	const LionVerifyTree *tree = m->tree;
	const char *rel = RelationGetRelationName(m->vs->index);

	if ((pg->flags & LION_VPAGE_UPPER) == 0)
		return true;
	if (BlockNumberIsValid(m->parent))
	{
		uint32		high = m->parenthigh;

		if (m->level == 1 && tree->positions)
		{
			if (pg->upper > high)
				lion_verify_match_fail(m, "lion index \"%s\": position leaf %u reaches block %u, above the high key %u of its parent %u",
									   rel, pg->blk, pg->upper, high, m->parent);
		}
		else if (m->level == 1)
		{
			if (pg->upper >= high)
				lion_verify_match_fail(m, "lion index \"%s\": leaf %u holds container key %u, at or above the high key %u of its parent %u",
									   rel, pg->blk, pg->upper, high, m->parent);
		}
		else if (pg->upper > high)
			lion_verify_match_fail(m, "lion index \"%s\": the high key %u of %s block %u sorts after the high key %u of its parent %u",
								   rel, pg->upper, tree->what, pg->blk, high,
								   m->parent);
	}
	if (m->failure != NULL)
		return false;
	if (m->level == 1 && tree->positions)
	{
		if (pg->upper > next)
			lion_verify_match_fail(m, "lion index \"%s\": position leaf %u reaches block %u, above the separator %u of its right sibling",
								   rel, pg->blk, pg->upper, next);
	}
	else if (m->level == 1)
	{
		if (pg->upper >= next)
			lion_verify_match_fail(m, "lion index \"%s\": leaf %u holds container key %u, at or above the separator %u of its right sibling",
								   rel, pg->blk, pg->upper, next);
	}
	else if (pg->upper > next)
		lion_verify_match_fail(m, "lion index \"%s\": the high key %u of %s block %u sorts after the separator %u of its right sibling",
							   rel, pg->upper, tree->what, pg->blk, next);
	return m->failure == NULL;
}

/*
 * The walk of level i is done with page blk: the page its last downlink
 * named, and the right halves of unfinished splits after it, are bounded by
 * blk's high key (highkey, when hashigh) until the next downlink matches.
 */
static void
lion_verify_match_parent_done(LionVerifyMatch *m, BlockNumber blk,
							  bool hashigh, uint32 highkey)
{
	m->parent = hashigh ? blk : InvalidBlockNumber;
	m->parenthigh = highkey;
}

/*
 * The walk of level i met a downlink to child with separator sep: match it
 * with the next page of level i - 1 that has one.
 */
static void
lion_verify_match_downlink(LionVerifyMatch *m, BlockNumber child, uint32 sep)
{
	LionVerifyState *vs = m->vs;
	const LionVerifyTree *tree = m->tree;
	const char *rel = RelationGetRelationName(vs->index);
	int64		j = m->ndownlinks++;
	LionVerifyPage pg;

	/*
	 * A downlink to a page no walk reached, in a walk a writer - or, on a
	 * standby, replay - may have split a page under (above).  A standby's
	 * walk that has read nothing past its replay position so far compares
	 * the downlink as an exact walk does; one that reads such a page later
	 * is thrown away at the end all the same (lion_verify_match_end()).
	 */
	if (!m->exact && !m->unseen &&
		(vs->concurrent || lion_verify_moved(vs)) &&
		(!lion_verify_block_exists(vs, child) || vs->refs[child] == 0))
		m->unseen = true;
	if (m->unseen || m->failure != NULL || m->countoff)
		return;

	/* the page the last downlink named is bounded above by this one */
	if (m->haveopen)
	{
		m->haveopen = false;
		if (!lion_verify_match_upper(m, &m->open, sep))
			return;
	}

	while (lion_verify_below_get(vs, m->below, &pg))
	{
		bool		previncomplete = m->haveprev &&
			(m->prev.flags & LION_VPAGE_INCOMPLETE) != 0;

		m->prev = pg;
		m->haveprev = true;

		if (pg.blk == child)
		{
			/* a page of this downlink's own parent from here on */
			m->parent = InvalidBlockNumber;

			if (j == 0 && sep != 0)
				lion_verify_match_fail(m, "lion index \"%s\": the first downlink of %s level %u has separator %u, expected minus infinity",
									   rel, tree->what, m->level, sep);

			/* the separator is at or below its child's own first key */
			else if (tree->positions && m->level == 1)
			{
				if (j > 0 && (pg.flags & LION_VPAGE_FIRST) != 0 &&
					sep != pg.firstkey)
					lion_verify_match_fail(m, "lion index \"%s\": the separator %u of position leaf %u is not the header %u of its first chunk",
										   rel, sep, pg.blk, pg.firstkey);
			}
			else if ((pg.flags & LION_VPAGE_FIRST) != 0 && sep > pg.firstkey)
				lion_verify_match_fail(m, "lion index \"%s\": the separator %u of %s block %u sorts after its own first %s %u",
									   rel, sep, tree->what, pg.blk,
									   tree->positions ? "separator" : "container key",
									   pg.firstkey);
			m->open = pg;
			m->haveopen = true;
			return;
		}

		if (!previncomplete)
		{
			lion_verify_match_fail(m, "lion index \"%s\": downlink %lld of %s level %u points at block %u, but the next page of level %u is block %u",
								   rel, (long long) j, tree->what, m->level,
								   child, m->level - 1, pg.blk);
			return;
		}

		/* the right half of an unfinished split: no downlink yet */
		if (!lion_verify_match_upper(m, &pg, sep))
			return;
	}

	/* a downlink more than level i - 1 has pages */
	m->countoff = true;
}

/*
 * The walk of level i is over: what is left of level i - 1 can only be the
 * right halves of unfinished splits.  False when the walk has to be
 * repeated - a downlink named a page no walk reached, or, on a standby, the
 * comparison found a mismatch after the walk read a page past its replay
 * position - else the first mismatch is reported, if there is one.
 */
static bool
lion_verify_match_end(LionVerifyMatch *m)
{
	const LionVerifyTree *tree = m->tree;
	LionVerifyPage pg;

	if (m->unseen)
		return false;

	/*
	 * The page the last downlink named has no next one to be bounded by, nor
	 * the right halves of unfinished splits after it, and no parent's high
	 * key either: the last page of level i has no right link, and a high key
	 * there was reported as its walk read it.
	 */
	while (m->failure == NULL && !m->countoff &&
		   lion_verify_below_get(m->vs, m->below, &pg))
	{
		bool		previncomplete = m->haveprev &&
			(m->prev.flags & LION_VPAGE_INCOMPLETE) != 0;

		m->prev = pg;
		m->haveprev = true;
		if (!previncomplete)
			m->countoff = true;
	}

	if ((m->failure != NULL || m->countoff) && !m->exact &&
		lion_verify_moved(m->vs))
		return false;
	if (m->failure != NULL)
		lion_corrupt("%s", m->failure);
	if (m->countoff)
		lion_corrupt("lion index \"%s\": %s level %u of chain entry %u on block %u has %lld downlinks but level %u has %lld pages",
					RelationGetRelationName(m->vs->index), tree->what,
					m->level, tree->eoff, tree->eblk,
					(long long) m->ndownlinks, m->level - 1,
					(long long) m->below->n);
	return true;
}

/*
 * Walk one INTERNAL level of a posting set or a position tree, matching its
 * downlinks with the level below as it meets them (match), and writing its
 * own pages into out for the level above, when there is one and the walk
 * keeps sequences (LionVerifyBelow); *npagesp is how many pages it walked.
 * Returns false when the walk has to be repeated (lion_verify_tree_page()).
 */
static bool
lion_verify_tree_level(LionVerifyState *vs, const LionVerifyTree *tree,
					   BlockNumber first, uint16 level, LionVerifySeq *out,
					   LionVerifyMatch *match, bool exact,
					   LionVerifySetResult *res, int64 *npagesp)
{
	BlockNumber blk = first;
	Page		copy = (Page) palloc(BLCKSZ);

	*npagesp = 0;

	while (BlockNumberIsValid(blk))
	{
		Buffer		buf;
		Page		page;
		OffsetNumber maxoff;
		OffsetNumber off;
		OffsetNumber firstdata;
		uint32		highkey = 0;
		bool		hashigh = false;
		uint32		firstkey = 0;
		bool		hasfirst = false;
		uint32		prevkey = 0;
		LionVerifyPage pg;

		page = lion_verify_tree_page(vs, blk, tree, level, exact, &buf);
		if (page == NULL)
		{
			pfree(copy);
			return false;
		}
		maxoff = PageGetMaxOffsetNumber(page);

		/* Every item is one whole pivot before any of them is read. */
		for (off = FirstOffsetNumber; off <= maxoff; off++)
		{
			ItemId		iid = lion_verify_itemid(vs, blk, page, off);

			if (!ItemIdIsUsed(iid) ||
				ItemIdGetLength(iid) != LION_POSTING_PIVOT_SIZE)
				lion_corrupt("lion index \"%s\": item %u of internal %s page %u is %zu bytes, expected %zu",
							RelationGetRelationName(vs->index), off,
							tree->what, blk, (Size) ItemIdGetLength(iid),
							LION_POSTING_PIVOT_SIZE);
		}
		lion_page_check_items(vs->index, page, blk);

		/*
		 * The rest is read from a copy, with the page let go: the comparison
		 * with the level below reads that level's pages back - from a file
		 * once there are more of them than memory keeps, or from the index in
		 * an exact walk (LionVerifyBelow) - and no posting page is held while
		 * it does.  The page was read under its SHARE lock, so the copy is
		 * the page as a writer left it.
		 */
		memcpy(copy, page, BLCKSZ);
		UnlockReleaseBuffer(buf);
		page = copy;
		firstdata = lion_posting_first_data(page);

		if (!LionPageIsRightmost(page))
		{
			if (maxoff < FirstOffsetNumber)
				lion_corrupt("lion index \"%s\": internal %s page %u is not rightmost but has no high key",
							RelationGetRelationName(vs->index), tree->what,
							blk);
			if (BlockNumberIsValid(lion_posting_pivot(page, FirstOffsetNumber)->child))
				lion_corrupt("lion index \"%s\": the first item of internal %s page %u is a downlink, not a high key",
							RelationGetRelationName(vs->index), tree->what,
							blk);
			highkey = lion_posting_pivot(page, FirstOffsetNumber)->ckey;
			hashigh = true;
		}
		else if (maxoff >= FirstOffsetNumber &&
				 !BlockNumberIsValid(lion_posting_pivot(page, FirstOffsetNumber)->child))
			lion_corrupt("lion index \"%s\": rightmost internal %s page %u carries a high key",
						RelationGetRelationName(vs->index), tree->what, blk);

		if (firstdata > maxoff)
			lion_corrupt("lion index \"%s\": internal %s page %u has no downlink",
						RelationGetRelationName(vs->index), tree->what, blk);

		for (off = firstdata; off <= maxoff; off++)
		{
			LionPostingPivot *piv = lion_posting_pivot(page, off);

			if (!BlockNumberIsValid(piv->child))
				lion_corrupt("lion index \"%s\": item %u of internal %s page %u is a second high key",
							RelationGetRelationName(vs->index), off,
							tree->what, blk);

			/*
			 * Separators are non-decreasing rather than strictly increasing: a
			 * split whose two halves are both empty by the time its repair runs
			 * gives the right one the left one's separator, which is a range of
			 * zero keys and routes everything to the right page (DESIGN.md §22).
			 */
			if (hasfirst && piv->ckey < prevkey)
				lion_corrupt("lion index \"%s\": downlink %u of internal %s page %u has separator %u, below the one before it (%u)",
							RelationGetRelationName(vs->index), off,
							tree->what, blk, piv->ckey, prevkey);

			/*
			 * ... and below the page's high key - or at it, in a position
			 * tree.  Its separators repeat (lion_postree.c), and an internal
			 * split, which cuts a page in the middle and gives the left half
			 * the right half's first separator as its high key
			 * (lion_posting_split_internal()), can cut a run of equal ones:
			 * the left half then ends with its high key.  Nothing is lost
			 * by it, because the trees' readers and writers look for the
			 * last separator strictly below a block
			 * (lion_posting_search_before()).
			 */
			if (hashigh && (tree->positions ? piv->ckey > highkey :
							piv->ckey >= highkey))
				lion_corrupt("lion index \"%s\": downlink %u of internal %s page %u has separator %u, %s its high key %u",
							RelationGetRelationName(vs->index), off,
							tree->what, blk, piv->ckey,
							tree->positions ? "above" : "not below", highkey);

			if (!hasfirst)
			{
				firstkey = piv->ckey;
				hasfirst = true;
			}
			prevkey = piv->ckey;

			lion_verify_match_downlink(match, piv->child, piv->ckey);

			CHECK_FOR_INTERRUPTS();
		}
		lion_verify_match_parent_done(match, blk, hashigh, highkey);

		pg.blk = blk;
		pg.firstkey = firstkey;
		pg.upper = highkey;
		pg.flags = (hasfirst ? LION_VPAGE_FIRST : 0) |
			(hashigh ? LION_VPAGE_UPPER : 0) |
			(LionPageIncompleteSplit(page) ? LION_VPAGE_INCOMPLETE : 0);
		if (out != NULL)
			lion_verify_seq_put(out, &pg);
		(*npagesp)++;
		if ((pg.flags & LION_VPAGE_INCOMPLETE) != 0)
			lion_verify_note_flagged(res, tree, blk, PageGetLSN(page), exact);

		blk = LionPageGetOpaque(page)->rightlink;

		CHECK_FOR_INTERRUPTS();
	}

	pfree(copy);
	return true;
}

/*
 * The leftmost page of every level of a tree, from the root down, into
 * leftmost[], and the root's level into *height.  The pages are read, not
 * visited: the level walks visit them.  An ERROR raised with a buffer locked
 * releases it on the way out, as everywhere else in this file.
 *
 * False on a standby when a page of the descent is not what its parent
 * promises and the walk has read a page past its replay position by then: a
 * set freed, or its root pushed down, under the descent (lion_verify_moved(),
 * lion_verify_tree_page()), and the walk is made again.
 */
static bool
lion_verify_tree_leftmost(LionVerifyState *vs, const LionVerifyTree *tree,
						  BlockNumber *leftmost, uint32 *height)
{
	BlockNumber blk = tree->root;
	int			steps = 0;
	uint16		expect = 0;
	uint32		i;

	for (i = 0; i <= LION_POSTING_MAX_HEIGHT; i++)
		leftmost[i] = InvalidBlockNumber;
	*height = 0;

	for (;;)
	{
		Buffer		buf;
		Page		page;
		uint16		level;
		OffsetNumber firstdata;
		ItemId		iid;

		if (steps++ > LION_POSTING_MAX_HEIGHT)
			lion_corrupt("lion index \"%s\": the %s tree of chain entry %u on block %u is deeper than %d levels",
						RelationGetRelationName(vs->index), tree->what,
						tree->eoff, tree->eblk, LION_POSTING_MAX_HEIGHT);

		/* the block's range, the header and the page kind */
		page = lion_verify_read_page(vs, blk, LION_PAGE_CONTAINER, &buf);

		/* what a set freed under the walk leaves, which the walk would meet */
		if (lion_verify_moved(vs) &&
			(LionPageIsDeleted(page) || LionPageIsUnlinked(page) ||
			 LionPageGetOpaque(page)->owner_head != tree->root ||
			 LionPageGetOpaque(page)->owner_hash != tree->owner))
		{
			UnlockReleaseBuffer(buf);
			return false;
		}

		level = LionPageGetOpaque(page)->level;
		if (level > LION_POSTING_MAX_HEIGHT)
			lion_corrupt("lion index \"%s\": %s page %u claims level %u",
						RelationGetRelationName(vs->index), tree->what, blk,
						level);
		if (blk == tree->root)
			*height = level;
		else if (level != expect)
		{
			if (lion_verify_moved(vs))
			{
				UnlockReleaseBuffer(buf);
				return false;
			}
			lion_corrupt("lion index \"%s\": %s page %u is at level %u, but its parent is at level %u",
						RelationGetRelationName(vs->index), tree->what, blk,
						level, expect + 1);
		}

		/*
		 * A root at level 0 is the first leaf of a tree without an internal
		 * level, and its right siblings are the tree's other leaves; one
		 * above level 0 is the root of a tree, which has none.
		 */
		if (!LionPageIsRightmost(page) && blk == tree->root && level > 0)
		{
			if (tree->positions)
				lion_corrupt("lion index \"%s\": the root %u of the position tree of chain entry %u on block %u has a right sibling",
							RelationGetRelationName(vs->index), blk,
							tree->eoff, tree->eblk);
			lion_corrupt("lion index \"%s\": the root %u of chain entry %u on block %u has a right sibling",
						RelationGetRelationName(vs->index), blk, tree->eoff,
						tree->eblk);
		}
		leftmost[level] = blk;
		if (level == 0)
		{
			UnlockReleaseBuffer(buf);
			break;
		}

		firstdata = lion_posting_first_data(page);
		if (firstdata > PageGetMaxOffsetNumber(page))
			lion_corrupt("lion index \"%s\": internal %s page %u has no downlink",
						RelationGetRelationName(vs->index), tree->what, blk);
		iid = lion_verify_itemid(vs, blk, page, firstdata);
		if (!ItemIdIsUsed(iid) ||
			ItemIdGetLength(iid) != LION_POSTING_PIVOT_SIZE)
			lion_corrupt("lion index \"%s\": item %u of internal %s page %u is %zu bytes, expected %zu",
						RelationGetRelationName(vs->index), firstdata,
						tree->what, blk, (Size) ItemIdGetLength(iid),
						LION_POSTING_PIVOT_SIZE);

		/* checked against the index's length when it is read */
		blk = lion_posting_pivot(page, firstdata)->child;
		expect = level - 1;
		UnlockReleaseBuffer(buf);
	}
	return true;
}

/*
 * The checks of a tree without an internal level (DESIGN.md §22, §17): at
 * most LION_POSTING_FLAT_LEAVES leaves, and none of them flagged, since such
 * a tree's splits post no downlink, and the one that would make it longer
 * pushes the root down first.  A walk that may be repeated can have seen
 * that push-down happen behind it, and the leaves and the split that came
 * after it, so it walks again instead (false).
 */
static bool
lion_verify_flat(LionVerifyState *vs, const LionVerifyTree *tree,
				 const LionVerifyLeafSum *sum, bool exact)
{
	if (sum->npages > LION_POSTING_FLAT_LEAVES)
	{
		if (!exact)
			return false;
		if (tree->positions)
			lion_corrupt("lion index \"%s\": the position tree at %u of entry %u on block %u has more than %d leaves and no internal level",
						RelationGetRelationName(vs->index), tree->root,
						tree->eoff, tree->eblk, LION_POSTING_FLAT_LEAVES);
		lion_corrupt("lion index \"%s\": the posting set of chain entry %u on block %u has %lld leaves and no internal level, more than %d",
					RelationGetRelationName(vs->index), tree->eoff,
					tree->eblk, (long long) sum->npages,
					LION_POSTING_FLAT_LEAVES);
	}
	if (BlockNumberIsValid(sum->flagged))
	{
		if (!exact)
			return false;
		if (tree->positions)
			lion_corrupt("lion index \"%s\": leaf %u of the position tree at %u, which has no internal level, is flagged as an unfinished split",
						RelationGetRelationName(vs->index), sum->flagged,
						tree->root);
		lion_corrupt("lion index \"%s\": leaf %u of the posting set of chain entry %u on block %u, which has no internal level, is flagged as an unfinished split",
					RelationGetRelationName(vs->index), sum->flagged,
					tree->eoff, tree->eblk);
	}
	return true;
}

/*
 * Walk the levels of a tree above its leaves, from the bottom up, each
 * matched with the level below as its walk meets its downlinks
 * (LionVerifyMatch).  lv[0] holds the leaves' pages, as their walk found
 * them - nleaves of them; lv[1] is empty, and the two take turns as the
 * level below and the level being walked.  An exact walk keeps no sequence
 * and reads each level below again instead (LionVerifyBelow), and lv is
 * NULL.  Returns false when the walk has to be repeated.
 */
static bool
lion_verify_tree_levels(LionVerifyState *vs, const LionVerifyTree *tree,
						const BlockNumber *leftmost, uint32 height,
						LionVerifySeq *lv, int64 nleaves, bool exact,
						LionVerifySetResult *res)
{
	int64		nbelow = nleaves;
	uint32		i;

	Assert(exact == (lv == NULL));
	for (i = 1; i <= height; i++)
	{
		LionVerifySeq *above = exact ? NULL : &lv[i % 2];
		LionVerifyBelow below;
		LionVerifyMatch match;
		int64		nlevel;

		memset(&below, 0, sizeof(below));
		below.seq = exact ? NULL : &lv[(i - 1) % 2];
		below.next = leftmost[i - 1];
		below.level = (uint16) (i - 1);
		below.positions = tree->positions;
		below.n = nbelow;

		/* the root's level has no level above it to be matched with */
		if (above != NULL)
			lion_verify_seq_reset(above);
		lion_verify_match_init(&match, vs, tree, i, &below, exact);
		if (!lion_verify_tree_level(vs, tree, leftmost[i], (uint16) i,
									(i < height) ? above : NULL, &match,
									exact, res, &nlevel) ||
			!lion_verify_match_end(&match))
			return false;
		nbelow = nlevel;
	}
	return true;
}

/*
 * Walk and check the posting tree of a CHAIN entry (DESIGN.md §22).
 *
 * The tree is checked level by level from the leaves up, exactly as §21's
 * directory is: each level is walked along its right links, which proves the
 * sibling chain and the key order within and across its pages, and the level
 * above is then checked against what that walk collected, which proves that
 * its downlinks name exactly those pages in that order - which is the same
 * statement as "the leaf right-link chain equals the in-order leaf sequence".
 *
 * `entry` is the caller's copy.  With `exact` the set cannot change while it
 * is walked - its entry's directory leaf is held, which only a primary does
 * (a standby must not: lion_verify_set_walks()) - and everything is checked
 * and reported here but the totals, which lion_verify_chain_totals()
 * compares with the entry.
 *
 * Without it, writers of the key may be changing the set, and this is one
 * attempt of lion_verify_set(), which reads the entry again afterwards and
 * throws the attempt away when anything changed.  Walking the LOWER level
 * FIRST is what makes a concurrent split harmless to the comparison of two
 * levels: a split puts its new page into the level's right-link chain in its
 * first record and the downlink into the parent in a later one, and holds the
 * page to the left of the new one EXCLUSIVE - and flagged - in between, where
 * no walk can read it.  So a page the child walk found either has a downlink
 * by the time the parent level is walked, or its left neighbour was read with
 * the flag of a split that was ABANDONED (an error, a crash), which the
 * comparison has always accepted.  The one thing a split between the two
 * walks can show is a downlink to a page the child walk never saw, and that
 * is a reason to walk again, not a finding - the insert that made it moved
 * the entry's counters anyway - as is a root pushed down under the walk.
 * Everything else the comparison checks is exact.
 *
 * That order is also why the walk of a level needs the whole level below it,
 * read before it began: what it keeps of each page is one LionVerifyPage, in
 * a sequence that goes to a temporary file past its share of work_mem
 * (LionVerifySeq), so that a set of any size is walked in the same memory.
 * An exact walk keeps none: it holds the entry's leaf, and the writers of
 * every key on that leaf wait for it, so it reads the level below again from
 * the index instead, a buffer hit a page, and never waits for a file while
 * it holds the leaf (LionVerifyBelow).
 *
 * The position tree of the entry, which is a posting tree too, is walked the
 * same way by lion_verify_postree().
 */
static bool
lion_verify_chain(LionVerifyState *vs, BlockNumber eblk, OffsetNumber eoff,
				  const LionEntryTuple *entry, bool exact,
				  LionVerifySetResult *res)
{
	BlockNumber leftmost[LION_POSTING_MAX_HEIGHT + 1];
	LionVerifyTree tree;
	LionVerifySeq lv[2];
	LionVerifyLeafSum sum;
	uint32		height;
	bool		ok;

	if (!BlockNumberIsValid(lion_entry_head(entry)) || !BlockNumberIsValid(lion_entry_tail(entry)))
		lion_corrupt("lion index \"%s\": chain entry %u on block %u has head %u and tail %u",
					RelationGetRelationName(vs->index), eoff, eblk,
					lion_entry_head(entry), lion_entry_tail(entry));

	lion_verify_tree_init(&tree, eblk, eoff, entry, false);
	if (!lion_verify_tree_leftmost(vs, &tree, leftmost, &height))
		return false;

	res->height = height;
	lion_verify_seq_init(&lv[0], &vs->seqacct, sizeof(LionVerifyPage));
	lion_verify_seq_init(&lv[1], &vs->seqacct, sizeof(LionVerifyPage));

	ok = lion_verify_posting_leaves(vs, &tree, entry, leftmost[0],
									exact ? NULL : &lv[0], &sum, exact, res) &&
		(height > 0 || lion_verify_flat(vs, &tree, &sum, exact));

	/*
	 * Test hook: the leaves are walked, the levels above are not, and nothing
	 * is held.  test/isolation/verify_concurrent.spec parks here and has an
	 * INSERT split a leaf of this set or push its root down.  Never in an
	 * exact walk, which may hold a directory leaf that a writer would then
	 * wait for without isolationtester seeing it wait.
	 */
	if (ok && !exact)
		LION_INJECTION_POINT("lion-verify-set-leaves-walked");

	ok = ok && lion_verify_tree_levels(vs, &tree, leftmost, height,
									   exact ? NULL : lv, sum.npages, exact,
									   res);
	lion_verify_seq_free(&lv[0]);
	lion_verify_seq_free(&lv[1]);
	return ok;
}

/*
 * What a walk of a posting set is compared with its entry by: the last leaf
 * against `tail`, the TIDs and items against `ntids` and `ncontainers`.  Only
 * for a walk that is kept: an exact one, or one lion_verify_set() found the
 * entry unchanged around - or, with compare false, the one a standby keeps of
 * a set that replay would not leave alone, which is compared with nothing, or
 * settles with a counting pass (lion_verify_set_count()).  Also where a kept
 * walk's unfinished-split warnings are given, so that a walk that is thrown
 * away and repeated gives them once.
 */
static void
lion_verify_chain_compare(LionVerifyState *vs, BlockNumber eblk,
						  OffsetNumber eoff, const LionEntryTuple *entry,
						  const LionVerifySetResult *res)
{
	if (res->last != lion_entry_tail(entry))
		lion_corrupt("lion index \"%s\": chain entry %u on block %u ends at block %u, but its tail is block %u",
					RelationGetRelationName(vs->index), eoff, eblk, res->last,
					lion_entry_tail(entry));

	if (res->card != lion_entry_ntids(entry))
		lion_corrupt("lion index \"%s\": chain entry %u on block %u claims " UINT64_FORMAT " TIDs, but its containers hold " UINT64_FORMAT,
					RelationGetRelationName(vs->index), eoff, eblk,
					lion_entry_ntids(entry), res->card);

	if (res->ncontainers != lion_entry_ncontainers(entry))
		lion_corrupt("lion index \"%s\": chain entry %u on block %u claims %u containers, but its posting tree holds %u",
					RelationGetRelationName(vs->index), eoff, eblk,
					lion_entry_ncontainers(entry), res->ncontainers);
}

static void
lion_verify_chain_totals(LionVerifyState *vs, BlockNumber eblk,
						 OffsetNumber eoff, const LionEntryTuple *entry,
						 LionVerifySetResult *res, bool compare)
{
	lion_verify_warn_flagged(vs, res);

	if (res->height > vs->max_posting_height)
		vs->max_posting_height = res->height;

	if (compare)
		lion_verify_chain_compare(vs, eblk, eoff, entry, res);
}

/*
 * Does a walk of a posting set agree with its entry - the last leaf with
 * `tail`, the TIDs and items with `ntids` and `ncontainers`?  What
 * lion_verify_chain_totals() reports, without reporting it.
 */
static bool
lion_verify_chain_agrees(const LionEntryTuple *entry,
						 const LionVerifySetResult *res)
{
	return res->last == lion_entry_tail(entry) &&
		res->card == lion_entry_ntids(entry) &&
		res->ncontainers == lion_entry_ncontainers(entry);
}

/*
 * A walk on a standby holds every page it reads from here on to the replay
 * position it begins at: the end of the last record replayed, which is at
 * least the LSN of every page replay has changed so far and below that of
 * every page it changes next (lion_verify_read_page() notes one).
 */
void
lion_verify_lsn_begin(LionVerifyState *vs)
{
	vs->lsnhorizon = GetXLogReplayRecPtr(NULL);
	vs->lsnpast = false;
	vs->lsntrack = true;
}

/* Has every page the walk read so far been as it was at that position? */
static bool
lion_verify_lsn_at(const LionVerifyState *vs)
{
	return !vs->lsnpast;
}

void
lion_verify_lsn_end(LionVerifyState *vs)
{
	vs->lsntrack = false;
}

/*
 * A standby's walk has read its entry again since it noted its replay
 * position, as it does before it reads anything of the tree: the entry is as
 * it was at that position or as replay left it since, and either way names
 * the tree the walk is of - its root keeps its block for as long as the
 * entry is a CHAIN entry (DESIGN.md §22), and a set freed under the walk is
 * one whose pages the walk finds freed past the position
 * (lion_verify_tree_page()).  So the read of the entry's leaf is not held to
 * the position; the one after the walk is, for the totals.
 */
static void
lion_verify_lsn_forget(LionVerifyState *vs)
{
	vs->lsnpast = false;
}

/* Does the entry header read now say what the one read before said? */
static bool
lion_verify_same_entry(const LionEntryTuple *a, const LionEntryTuple *b)
{
	return a->flags == b->flags && lion_entry_head(a) == lion_entry_head(b) && lion_entry_tail(a) == lion_entry_tail(b) &&
		lion_entry_ntids(a) == lion_entry_ntids(b) && lion_entry_ncontainers(a) == lion_entry_ncontainers(b);
}

/*
 * Where the entry a walk is of was last seen: its directory leaf, its offset
 * there, and the leaf's LSN then - at first the directory walk's copy of the
 * leaf (lion_verify_entry()).
 */
typedef struct LionVerifySeen
{
	BlockNumber blk;
	OffsetNumber off;
	XLogRecPtr	lsn;
} LionVerifySeen;

/*
 * Find the entry `cur` names again, by its exact stored key, and copy its
 * header - flags, head, tail, counters - over cur's; the key, the hash and the
 * column cannot change.  seen->blk is the leaf it was last seen on and the
 * search starts there and goes right: an entry only ever leaves its leaf in a
 * split, for the page the split links immediately to the right (DESIGN.md
 * §21), and nothing deletes an entry while VACUUM is locked out.  So the leaf
 * whose high key is above the key is where the entry is, and its absence
 * there is corruption.  With leafp the leaf comes back locked SHARE in
 * *leafp (the last resort of lion_verify_set()), else it is released.
 *
 * On a standby replay applies VACUUM, which deletes the entry of a key whose
 * last row it took out - and frees the set with it - and turns a CHAIN entry
 * that it shrank back into an INLINE one.  There an entry that is not where
 * its key sorts, or not of the form it had, is reported only when the leaf it
 * was looked for on is the leaf it was last seen on, with the LSN it had then:
 * nothing changed the leaf in between, so the entry it held then is still
 * there.  Otherwise the entry has gone, and false comes back: there is nothing
 * left of it to check.
 */
static bool
lion_verify_refind(LionVerifyState *vs, LionEntryTuple *cur,
				   LionVerifySeen *seen, Buffer *leafp)
{
	LionSearchKey sk;
	BlockNumber blk = seen->blk;
	BlockNumber steps = 0;

	lion_search_key_exact(vs->ix, &sk, cur);

	for (;;)
	{
		Buffer		buf;
		Page		page;
		OffsetNumber maxoff;
		OffsetNumber off;
		ItemId		iid;
		LionEntryTuple *item;
		bool		gone;

		page = lion_verify_read_page(vs, blk, LION_PAGE_BUCKET, &buf);
		if (LionPageGetOpaque(page)->level != 0)
			lion_corrupt("lion index \"%s\": directory leaf %u is at level %u",
						RelationGetRelationName(vs->index), blk,
						LionPageGetOpaque(page)->level);

		/* a page split onto since the walk read it has not been checked */
		maxoff = PageGetMaxOffsetNumber(page);
		for (off = FirstOffsetNumber; off <= maxoff; off++)
			(void) lion_verify_dir_item(vs, blk, page, off);
		if (!LionPageIsRightmost(page) &&
			(maxoff < FirstOffsetNumber ||
			 !ItemIdIsUsed(PageGetItemId(page, FirstOffsetNumber)) ||
			 !LionEntryIsHighKey(lion_page_entry(page, FirstOffsetNumber))))
			lion_corrupt("lion index \"%s\": the first item of directory page %u is not a high key",
						RelationGetRelationName(vs->index), blk);

		if (!LionPageIsRightmost(page) &&
			lion_cmp_entry(lion_page_entry(page, FirstOffsetNumber), &sk) <= 0)
		{
			/* the entry went right with the upper half of a split */
			BlockNumber next = LionPageGetOpaque(page)->rightlink;

			UnlockReleaseBuffer(buf);
			if (++steps > vs->nblocks)
				lion_corrupt("lion index \"%s\": the right links of the directory leaves from block %u go round in a cycle",
							RelationGetRelationName(vs->index), seen->blk);
			blk = next;
			CHECK_FOR_INTERRUPTS();
			continue;
		}

		/* on a standby: has replay changed the leaf since the entry was seen? */
		gone = !vs->concurrent &&
			(blk != seen->blk || BufferGetLSNAtomic(buf) != seen->lsn);

		off = lion_dir_binsrch(page, &sk);
		iid = (off <= maxoff) ? PageGetItemId(page, off) : NULL;
		item = (iid != NULL && ItemIdIsUsed(iid)) ?
			(LionEntryTuple *) PageGetItem(page, iid) : NULL;
		if (item == NULL || lion_cmp_entry(item, &sk) != 0)
		{
			if (gone)
			{
				UnlockReleaseBuffer(buf);
				return false;
			}
			lion_corrupt("lion index \"%s\": entry %u on block %u is not on block %u, where its key sorts",
						RelationGetRelationName(vs->index), seen->off,
						seen->blk, blk);
		}
		if ((item->flags & LION_ENTRY_CHAIN) != 0 &&
			ItemIdGetLength(iid) != LionEntryPayloadOffset(item))
			lion_corrupt("lion index \"%s\": chain entry %u on block %u is %zu bytes, expected %zu",
						RelationGetRelationName(vs->index), off, blk,
						(Size) ItemIdGetLength(iid),
						LionEntryPayloadOffset(item));

		/*
		 * A CHAIN entry never goes back to INLINE beside the check on a
		 * primary, and its key and column are its own, so the header it has
		 * now has the form cur has: anything else is not the entry cur was
		 * copied from.
		 */
		if (((item->flags ^ cur->flags) &
			 (LION_ENTRY_CHAIN | LION_ENTRY_ALIGNKEY | LION_ENTRY_POSITIONS)) != 0)
		{
			if (gone)
			{
				UnlockReleaseBuffer(buf);
				return false;
			}
			lion_corrupt("lion index \"%s\": entry %u on block %u changed its header form from flags 0x%04X to 0x%04X",
						RelationGetRelationName(vs->index), off, blk,
						cur->flags, item->flags);
		}
		memcpy(cur, item, LionEntryHdrSize(item));

		seen->blk = blk;
		seen->off = off;
		seen->lsn = BufferGetLSNAtomic(buf);
		if (leafp != NULL)
			*leafp = buf;
		else
			UnlockReleaseBuffer(buf);
		return true;
	}
}

/*
 * Count the TIDs, the items and the last leaf of the posting set of cur, as
 * lion_verify_posting_leaves() does, and check nothing: the root, the first
 * downlink of every level down to the leaves, and the leaves along their
 * right links, each read under its SHARE lock and let go at once, and none
 * of them visited.  The walk before it has checked every page of the set; so
 * a page here that is not one of the set's - freed, taken out of it, at
 * another level than its parent says - is what replay did to the set since,
 * and false says so.  A leaf replay took out of the set under it is empty
 * and keeps its right link, and is walked through, as readers do (DESIGN.md
 * §22, "Empty leaves").
 */
static bool
lion_verify_set_tally(LionVerifyState *vs, const LionEntryTuple *cur,
					  LionVerifySetResult *cnt)
{
	BlockNumber root = lion_entry_head(cur);
	uint32		owner = lion_entry_owner(cur);
	BlockNumber blk = root;
	BlockNumber steps = 0;
	int			expect = -1;

	cnt->card = 0;
	cnt->ncontainers = 0;
	cnt->last = InvalidBlockNumber;

	for (;;)
	{
		Buffer		buf;
		Page		page;
		LionPageOpaque opaque;
		OffsetNumber firstdata;
		ItemId		iid;

		if (++steps > LION_POSTING_MAX_HEIGHT + 1)
			return false;
		page = lion_verify_read_page(vs, blk, LION_PAGE_CONTAINER, &buf);
		opaque = LionPageGetOpaque(page);
		if (LionPageIsDeleted(page) || LionPageIsUnlinked(page) ||
			opaque->owner_head != root || opaque->owner_hash != owner ||
			(opaque->flags & LION_PAGE_POSITIONS) != 0 ||
			opaque->level > LION_POSTING_MAX_HEIGHT ||
			(expect >= 0 && opaque->level != expect))
		{
			UnlockReleaseBuffer(buf);
			return false;
		}
		if (opaque->level == 0)
		{
			UnlockReleaseBuffer(buf);
			break;
		}
		firstdata = lion_posting_first_data(page);
		if (firstdata > PageGetMaxOffsetNumber(page))
		{
			UnlockReleaseBuffer(buf);
			return false;
		}
		iid = lion_verify_itemid(vs, blk, page, firstdata);
		if (!ItemIdIsUsed(iid) ||
			ItemIdGetLength(iid) != LION_POSTING_PIVOT_SIZE)
		{
			UnlockReleaseBuffer(buf);
			return false;
		}
		expect = opaque->level - 1;
		blk = lion_posting_pivot(page, firstdata)->child;
		UnlockReleaseBuffer(buf);
	}

	steps = 0;
	while (BlockNumberIsValid(blk))
	{
		Buffer		buf;
		Page		page;
		LionPageOpaque opaque;
		BlockNumber next;

		if (++steps > vs->nblocks)
			return false;
		page = lion_verify_read_page(vs, blk, LION_PAGE_CONTAINER, &buf);
		opaque = LionPageGetOpaque(page);
		if (LionPageIsDeleted(page) || opaque->owner_head != root ||
			opaque->owner_hash != owner ||
			(opaque->flags & LION_PAGE_POSITIONS) != 0 || opaque->level != 0)
		{
			UnlockReleaseBuffer(buf);
			return false;
		}
		if (!LionPageIsUnlinked(page))
		{
			OffsetNumber maxoff = PageGetMaxOffsetNumber(page);
			OffsetNumber off;

			for (off = FirstOffsetNumber; off <= maxoff; off++)
			{
				ItemId		iid = lion_verify_itemid(vs, blk, page, off);

				if (!ItemIdIsUsed(iid))
					continue;
				if (ItemIdGetLength(iid) < LION_CONTAINER_HDRSZ)
				{
					UnlockReleaseBuffer(buf);
					return false;
				}
				cnt->card += ((LionContainer *) PageGetItem(page, iid))->cardinality;
				cnt->ncontainers++;
			}
			cnt->last = blk;
		}
		next = opaque->rightlink;
		UnlockReleaseBuffer(buf);
		blk = next;
		CHECK_FOR_INTERRUPTS();
	}
	return true;
}

/*
 * Settle, on a standby, the totals of a set whose walk got to its end while
 * replay changed it, with at most LION_VERIFY_STANDBY_PASSES counting passes
 * (lion_verify_set_tally()), each between two reads of the entry, as a walk
 * is.  A pass that read every page - the entry's leaf after it among them -
 * at or below the replay position it began at read the set and its entry at
 * one point of the primary's WAL, and its totals are compared with the
 * entry's there: a difference is damage.  One that agrees with an entry that
 * read the same both times is kept too: what it counted is there.  A pass
 * is a fraction of a walk, and so is far likelier to read one point under a
 * steady replay, which changes the set's last leaf and the entry's with
 * every row it adds.  True when a pass settled the totals, or the entry has
 * gone (*gonep); false when none did.
 */
static bool
lion_verify_set_count(LionVerifyState *vs, LionEntryTuple *cur,
					  LionVerifySeen *seen, bool *gonep)
{
	int			pass;

	*gonep = false;
	for (pass = 0; pass < LION_VERIFY_STANDBY_PASSES; pass++)
	{
		union
		{
			LionEntryTuple e;
			LionEntryChainHdr c;	/* room for either header form */
		}			before;
		LionVerifySetResult cnt;
		bool		counted;

		CHECK_FOR_INTERRUPTS();
		vs->nsetcounts++;
		memset(&cnt, 0, sizeof(cnt));
		lion_verify_lsn_begin(vs);
		if (!lion_verify_refind(vs, cur, seen, NULL))
		{
			lion_verify_lsn_end(vs);
			*gonep = true;
			return true;
		}
		lion_verify_lsn_forget(vs);
		memcpy(&before, cur, LionEntryHdrSize(cur));
		counted = lion_verify_set_tally(vs, cur, &cnt);
		if (!lion_verify_refind(vs, cur, seen, NULL))
		{
			lion_verify_lsn_end(vs);
			*gonep = true;
			return true;
		}
		lion_verify_lsn_end(vs);
		if (!counted)
			continue;
		if (lion_verify_lsn_at(vs))
		{
			lion_verify_chain_compare(vs, seen->blk, seen->off, cur, &cnt);
			return true;
		}
		if (lion_verify_same_entry(&before.e, cur) &&
			lion_verify_chain_agrees(cur, &cnt))
			return true;
	}
	return false;
}

/*
 * Walk and check the posting set of the CHAIN entry `entry`, which the walk
 * of the directory found at (eblk, eoff) - on its copy of the leaf, whose LSN
 * is elsn, with the leaf itself no longer locked (DESIGN.md §7).
 *
 * The counters of a set that writers are adding to cannot be compared with
 * its containers by a walk that holds nothing, and holding the entry's leaf
 * for every walk would keep the writers of every key on that leaf waiting
 * for it.  So the entry is read again, the set walked with nothing held, and
 * the entry read again afterwards, and the walk is kept when the entry reads
 * the same both times.  That is exact, for two reasons.  Every writer of a
 * key holds the directory leaf of its entry EXCLUSIVE from its first record
 * to its last (§5, §21), and the entry was read under a SHARE lock both
 * times, so a writer that changed the set in between did all of it in
 * between.  And every change an INSERT makes to a set moves the entry: an
 * item added, grown or split off adds a TID to `ntids` in the record that
 * places it, a push-down of the root moves `tail`; the one kind of writer
 * that adds no TID - one that finishes an abandoned split on its way down and
 * then finds its TID already there, or fails - only adds a downlink and
 * clears a flag, which the walk accepts either way, and whatever internal
 * split that makes shows up as a downlink the walk of the level below never
 * saw, which throws the walk away too.  VACUUM, the one writer that changes
 * containers without adding TIDs, is locked out.  The entry is read before
 * the first walk as before every other, and not taken from the directory
 * walk's copy, which writers have usually moved on from by the time the set
 * is walked (2026-10-23 review).
 *
 * A set that keeps changing - a hot key - is walked at most
 * LION_VERIFY_SET_ATTEMPTS times that way, and then once more with the leaf
 * held SHARE throughout.  That is the lock order every writer uses
 * (directory page before posting page, §5), and it keeps waiting only the
 * writers of the keys on that one leaf, for one walk of one set.
 *
 * On a standby the set is walked the same way, and NEVER with its leaf held
 * (DESIGN.md §25).  Replay takes the blocks of a record in the order the
 * writer registered them and holds every one to the end of the record, and
 * every record that changes a set registers the set's pages before the
 * entry's leaf - so a walk holding the leaf waits for a posting page the
 * startup process holds while the startup process waits for the leaf, on two
 * buffer content locks, which no deadlock detector watches and no cancel
 * reaches: replay stopped for good, and so did this backend (2026-09-29
 * review; test/recovery/run.sh phase 4).  Every record that changes a
 * set's items, its counters, its last leaf or the level of a leaf root
 * carries the entry, and the records that do not - a downlink, a flag
 * cleared, an internal page split, an internal root pushed down - change
 * nothing the entry counts and are what the walk accepts from a writer
 * between two levels, or throws away.  But the entry reading the same both
 * times does not settle a walk there as it does on a primary: replay
 * applies VACUUM too, which the primary's check locks out, and a TID a
 * VACUUM took out of one leaf and another an INSERT put into another leave
 * the entry as it was, while a walk that read the first leaf before the one
 * and the second after the other counts a TID more than the entry.  So a
 * walk on a standby is kept, and compared with the entry, when it agrees
 * with an entry that read the same both times - what it found is there - or
 * when it read every page, the entry's leaf after it among them, as it was
 * at the replay position it began at (lion_verify_lsn_begin()): then it read
 * the set and the entry at one point of the primary's WAL, where they agree
 * unless the index is damaged, and a difference is reported.  A walk that
 * replay changed a page of under it is walked again: what it found against
 * the tree may be replay's doing (lion_verify_tree_page(),
 * lion_verify_match_end()).  One that got to the end and is neither of the
 * two is kept for what it checked, and its totals are settled by counting
 * passes (lion_verify_set_count()) - and when none of those settles them
 * either, it is kept without the comparison, which a WARNING says.  The
 * VACUUM replay applies can take the entry away too, and the set with it
 * (lion_verify_refind()): then the check of the set ends there.  Only a set
 * that replay keeps from being walked to its end LION_VERIFY_STANDBY_WALKS
 * times is an ERROR asking for replay to be paused.
 */
static void
lion_verify_set_walks(LionVerifyState *vs, BlockNumber eblk, OffsetNumber eoff,
					  XLogRecPtr elsn, const LionEntryTuple *entry)
{
	Size		sz = LionEntryPayloadOffset(entry);
	LionEntryTuple *cur = (LionEntryTuple *) palloc(sz);
	LionVerifySeen seen;
	LionVerifySetResult res;
	Buffer		leaf;
	int			attempt;

	memcpy(cur, entry, sz);
	seen.blk = eblk;
	seen.off = eoff;
	seen.lsn = elsn;

	for (attempt = 0;; attempt++)
	{
		union
		{
			LionEntryTuple e;
			LionEntryChainHdr c;	/* room for either header form */
		}			before;
		bool		ok;
		bool		gone;

		/*
		 * Test hook: the last walk was thrown away, the entry is read again,
		 * and nothing is held.  test/isolation/verify_concurrent.spec parks
		 * here and at lion-verify-set-leaves-walked in turn: the
		 * isolationtester cannot tell a session that parks at a point again
		 * from one still to wake from the point, but it can tell two points
		 * apart.
		 */
		if (attempt > 0)
			LION_INJECTION_POINT("lion-verify-set-rewalk");

		lion_verify_result_init(vs, &res);
		lion_verify_track_begin(vs);
		vs->nsetwalks++;
		if (!vs->concurrent)
			lion_verify_lsn_begin(vs);
		if (!lion_verify_refind(vs, cur, &seen, NULL))
		{
			/* replay took the entry away before the walk began */
			lion_verify_lsn_end(vs);
			lion_verify_track_end(vs, false);
			lion_verify_result_free(&res);
			pfree(cur);
			return;
		}
		if (!vs->concurrent)
			lion_verify_lsn_forget(vs);
		memcpy(&before, cur, LionEntryHdrSize(cur));
		ok = lion_verify_chain(vs, seen.blk, seen.off, cur, false, &res);
		if (!lion_verify_refind(vs, cur, &seen, NULL))
		{
			/*
			 * ... or during it: what the walk read of the set is kept, the
			 * pages marked as reached, and nothing is compared.
			 */
			lion_verify_lsn_end(vs);
			lion_verify_track_end(vs, true);
			lion_verify_result_free(&res);
			pfree(cur);
			return;
		}
		if (!vs->concurrent)
			lion_verify_lsn_end(vs);

		if (ok && (vs->concurrent ?
				   lion_verify_same_entry(&before.e, cur) :
				   (lion_verify_lsn_at(vs) ||
					(lion_verify_same_entry(&before.e, cur) &&
					 lion_verify_chain_agrees(cur, &res)))))
		{
			lion_verify_track_end(vs, true);
			lion_verify_chain_totals(vs, seen.blk, seen.off, cur, &res, true);
			lion_verify_result_free(&res);
			pfree(cur);
			return;
		}

		/*
		 * A standby's walk that got to the end of the set while replay
		 * changed it: kept for what it checked, and its totals settled by
		 * counting passes, or compared with nothing (above).
		 */
		if (!vs->concurrent && ok)
		{
			bool		settled;

			lion_verify_track_end(vs, true);
			lion_verify_chain_totals(vs, seen.blk, seen.off, cur, &res, false);
			lion_verify_result_free(&res);
			settled = lion_verify_set_count(vs, cur, &seen, &gone);
			if (!settled)
			{
				vs->nsetuncompared++;
				ereport(WARNING,
						(errmsg("lion index \"%s\": the totals of chain entry %u on block %u were not compared with its posting set",
								RelationGetRelationName(vs->index), seen.off,
								seen.blk),
						 errdetail("Recovery changed the set during the walk made of it and during each of the %d passes that counted it again; every page of the walk was checked.",
								   LION_VERIFY_STANDBY_PASSES),
						 errhint("Run the check with replay paused (pg_wal_replay_pause(), then pg_wal_replay_resume()) to compare them.")));
			}
			pfree(cur);
			return;
		}

		/* A writer got in; forget what this walk marked, and walk again. */
		lion_verify_track_end(vs, false);
		lion_verify_result_free(&res);
		vs->nsetretries++;
		if (attempt + 1 >= (vs->concurrent ? LION_VERIFY_SET_ATTEMPTS :
							LION_VERIFY_STANDBY_WALKS))
			break;
	}

	if (!vs->concurrent)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("lion index \"%s\": could not walk the posting set of chain entry %u on block %u to its end",
						RelationGetRelationName(vs->index), seen.off,
						seen.blk),
				 errdetail("Recovery changed the set during each of the %d walks made of it.",
						   attempt + 1),
				 errhint("Run the check with replay paused (pg_wal_replay_pause(), then pg_wal_replay_resume()).")));

	vs->nsetwalks++;
	vs->nsetholds++;
	(void) lion_verify_refind(vs, cur, &seen, &leaf);

	/*
	 * Test hook: the leaf is held and the exact walk is about to begin.
	 * test/isolation/verify_concurrent.spec attaches 'notice' here to show
	 * that a set a writer kept changing got this walk.
	 */
	LION_INJECTION_POINT("lion-verify-set-held");

	if ((cur->flags & LION_ENTRY_CHAIN) == 0)
		lion_corrupt("lion index \"%s\": chain entry %u on block %u is an inline entry now",
					RelationGetRelationName(vs->index), seen.off, seen.blk);
	lion_verify_result_init(vs, &res);
	(void) lion_verify_chain(vs, seen.blk, seen.off, cur, true, &res);
	lion_verify_chain_totals(vs, seen.blk, seen.off, cur, &res, true);
	lion_verify_result_free(&res);
	UnlockReleaseBuffer(leaf);
	pfree(cur);
}

/*
 * lion_verify_set_walks() in a memory context of its own, emptied after every
 * set: what a walk allocates - and every walk thrown away - is garbage the
 * moment the set is settled, and an index has as many sets as it has keys
 * with more than an entry's worth of rows.
 */
static void
lion_verify_set(LionVerifyState *vs, BlockNumber eblk, OffsetNumber eoff,
				XLogRecPtr elsn, const LionEntryTuple *entry)
{
	MemoryContext oldcxt = MemoryContextSwitchTo(vs->setcxt);

	lion_verify_set_walks(vs, eblk, eoff, elsn, entry);
	MemoryContextSwitchTo(oldcxt);
	MemoryContextReset(vs->setcxt);
}

/*
 * P ⊇ C (DESIGN.md §17) for an INLINE entry: every TID of its payload has
 * positions, i.e. is among codes - the members of the entry's one chunk,
 * ascending, at most LION_POS_MAX_MEMBERS of them (lion_verify_entry()).  A
 * CHAIN entry's are compared by lion_verify_pos_covers(), which collects
 * nothing.
 */
typedef struct LionVerifyCover
{
	const uint64 *codes;
	uint64		ncodes;
	uint64		missing;
	uint64		first_missing;
	uint32		ckey;
} LionVerifyCover;

static bool
lion_verify_cover_code(LionVerifyCover *cv, uint64 code)
{
	uint64		lo = 0;
	uint64		hi = cv->ncodes;

	while (lo < hi)
	{
		uint64		mid = lo + (hi - lo) / 2;

		if (cv->codes[mid] < code)
			lo = mid + 1;
		else
			hi = mid;
	}
	if (lo == cv->ncodes || cv->codes[lo] != code)
	{
		if (cv->missing++ == 0)
			cv->first_missing = code;
	}
	return true;
}

static bool
lion_verify_cover_lo(uint16 lo, void *arg)
{
	LionVerifyCover *cv = (LionVerifyCover *) arg;

	return lion_verify_cover_code(cv, lion_make_code(cv->ckey, lo));
}

static bool
lion_verify_cover_pair(uint32 ckey, uint16 lo, void *arg)
{
	return lion_verify_cover_code((LionVerifyCover *) arg,
								  lion_make_code(ckey, lo));
}

/*
 * A forward reader over the leaves of a posting set or a position tree, for
 * lion_verify_pos_covers(), which reads the two in step.  Each leaf is copied
 * under its SHARE lock and read from the copy, so a reader holds no lock
 * between two of its reads, and reading one tree never waits for a page of
 * the other while it holds one.
 */
typedef struct LionVerifyLeaves
{
	const LionVerifyTree *tree;
	Page		page;			/* the leaf being read, a private copy */
	BlockNumber blk;			/* ... its block */
	OffsetNumber off;			/* the next item of it to read */
	BlockNumber steps;			/* leaves read, against a cycle */
} LionVerifyLeaves;

/*
 * Copy blk, the next leaf of lv's tree, into lv->page.
 *
 * The tree's own walk has just checked every page of it, and no page leaves
 * a tree while VACUUM is locked out, so anything but a leaf of the tree here
 * is damage - with two exceptions, in a walk that may be repeated.  A
 * push-down of the root keeps its block and makes it the level above
 * (DESIGN.md §22), so the first leaf of a tree without an internal level,
 * which is its root, can be an internal page by now.  And on a standby,
 * where replay applies VACUUM, a leaf the tree's walk read can be taken out
 * of the tree or freed with it by now - which the walk can tell from damage
 * by the LSN it reads past the replay position it began at
 * (lion_verify_moved()).  False says so, and the walk is repeated.
 */
static bool
lion_verify_leaves_read(LionVerifyState *vs, LionVerifyLeaves *lv,
						BlockNumber blk, bool exact)
{
	const LionVerifyTree *tree = lv->tree;
	Buffer		buf;
	Page		page;
	LionPageOpaque opaque;

	if (++lv->steps > vs->nblocks)
		lion_corrupt("lion index \"%s\": the leaves of the %s tree of chain entry %u on block %u go round a cycle",
					RelationGetRelationName(vs->index), tree->what, tree->eoff,
					tree->eblk);

	page = lion_verify_read_page(vs, blk, LION_PAGE_CONTAINER, &buf);
	opaque = LionPageGetOpaque(page);
	if (opaque->level != 0 && !exact && blk == tree->root)
	{
		UnlockReleaseBuffer(buf);
		return false;
	}
	if (opaque->level != 0 || LionPageIsDeleted(page) ||
		LionPageIsUnlinked(page) || opaque->owner_head != tree->root ||
		opaque->owner_hash != tree->owner ||
		((opaque->flags & LION_PAGE_POSITIONS) != 0) != tree->positions)
	{
		if (!exact && lion_verify_moved(vs))
		{
			UnlockReleaseBuffer(buf);
			return false;
		}
		lion_corrupt("lion index \"%s\": block %u is not a leaf of the %s tree of chain entry %u on block %u",
					RelationGetRelationName(vs->index), blk, tree->what,
					tree->eoff, tree->eblk);
	}

	memcpy(lv->page, page, BLCKSZ);
	UnlockReleaseBuffer(buf);
	lv->blk = blk;
	lv->off = FirstOffsetNumber;
	return true;
}

/*
 * P ⊇ C (DESIGN.md §17) for a CHAIN entry: every TID of its posting set has
 * positions in its position tree.  Both trees hold their members in
 * ascending TID order along their leaves, so the two leaf chains are read in
 * step, the position tree a member at a time - its code, with the positions
 * stepped over - and nothing is collected: two leaves and one member are all
 * the comparison keeps, whatever the size of the key.
 */
typedef struct LionVerifyCovers
{
	LionVerifyState *vs;
	bool		exact;
	LionVerifyLeaves pos;		/* the position tree's leaves */
	LionPosIter it;				/* ... the chunk being read on pos.page */
	bool		inchunk;
	LionPosMember *m;			/* the member the reader stands on */
	bool		have;			/* ... which there is one of */
	bool		done;			/* the tree has no member left */
	bool		changed;		/* the tree changed: the walk is repeated */
	uint64		nmembers;		/* members stepped onto */
	uint32		ckey;			/* the posting container being read */
	uint64		missing;		/* TIDs without positions */
	uint64		first_missing;
} LionVerifyCovers;

/* Step to the next member of the position tree, reading its next leaf. */
static void
lion_verify_covers_step(LionVerifyCovers *cv)
{
	LionVerifyState *vs = cv->vs;

	cv->have = false;
	for (;;)
	{
		if (cv->inchunk && lion_poschunk_iter_next_npos(&cv->it, cv->m))
		{
			cv->have = true;
			cv->nmembers++;
			return;
		}
		cv->inchunk = false;
		if (cv->pos.off <= PageGetMaxOffsetNumber(cv->pos.page))
		{
			lion_poschunk_iter_init(&cv->it,
									lion_page_poschunk_fetch(vs->index,
															 cv->pos.page,
															 cv->pos.blk,
															 cv->pos.off));
			cv->pos.off++;
			cv->inchunk = true;
			continue;
		}
		if (LionPageIsRightmost(cv->pos.page))
		{
			cv->done = true;
			return;
		}
		if (!lion_verify_leaves_read(vs, &cv->pos,
									 LionPageGetOpaque(cv->pos.page)->rightlink,
									 cv->exact))
		{
			cv->changed = true;
			return;
		}
		CHECK_FOR_INTERRUPTS();
	}
}

/* Is the TID code among the members?  False stops the posting set's read. */
static bool
lion_verify_covers_code(LionVerifyCovers *cv, uint64 code)
{
	while (!cv->done && (!cv->have || cv->m->code < code))
	{
		lion_verify_covers_step(cv);
		if (cv->changed)
			return false;
	}
	if (!cv->have || cv->m->code != code)
	{
		if (cv->missing++ == 0)
			cv->first_missing = code;
	}
	return true;
}

static bool
lion_verify_covers_lo(uint16 lo, void *arg)
{
	LionVerifyCovers *cv = (LionVerifyCovers *) arg;

	return lion_verify_covers_code(cv, lion_make_code(cv->ckey, lo));
}

static bool
lion_verify_covers_pair(uint32 ckey, uint16 lo, void *arg)
{
	return lion_verify_covers_code((LionVerifyCovers *) arg,
								   lion_make_code(ckey, lo));
}

/*
 * Compare the posting set of the CHAIN entry `entry` with its position tree
 * ptree, as above, and leave in cv how many of the set's TIDs have no
 * positions, and the first of them; lion_verify_postree_walks() decides
 * whether that stands.  With count, the members after the set's last TID
 * are read too, and cv->nmembers is all of the tree's: the count a standby's
 * covers pass compares with the entry, as a walk's res->card is
 * (lion_verify_postree_count()).  Returns false when the walk has to be
 * repeated (lion_verify_leaves_read()), which only a walk that is not exact
 * does.
 */
static bool
lion_verify_pos_covers(LionVerifyState *vs, const LionEntryTuple *entry,
					   const LionVerifyTree *ptree, bool exact, bool count,
					   LionVerifyCovers *cv)
{
	BlockNumber leftmost[LION_POSTING_MAX_HEIGHT + 1];
	uint32		height;
	LionVerifyTree stree;
	LionVerifyLeaves set;

	memset(cv, 0, sizeof(*cv));
	cv->vs = vs;
	cv->exact = exact;
	cv->m = (LionPosMember *) palloc(sizeof(LionPosMember));
	cv->pos.tree = ptree;
	cv->pos.page = (Page) palloc(BLCKSZ);
	if (!lion_verify_tree_leftmost(vs, ptree, leftmost, &height) ||
		!lion_verify_leaves_read(vs, &cv->pos, leftmost[0], exact))
		return false;

	lion_verify_tree_init(&stree, ptree->eblk, ptree->eoff, entry, false);
	memset(&set, 0, sizeof(set));
	set.tree = &stree;
	set.page = (Page) palloc(BLCKSZ);
	if (!lion_verify_tree_leftmost(vs, &stree, leftmost, &height) ||
		!lion_verify_leaves_read(vs, &set, leftmost[0], exact))
		return false;

	for (;;)
	{
		OffsetNumber maxoff = PageGetMaxOffsetNumber(set.page);

		for (; set.off <= maxoff; set.off++)
		{
			LionContainer *c = lion_page_item_fetch(vs->index, set.page,
													set.blk, set.off);

			if (c->type == LION_CT_SPARSE)
				lion_sparse_iterate(c, lion_verify_covers_pair, cv);
			else
			{
				cv->ckey = c->ckey;
				lion_container_iterate(c, lion_verify_covers_lo, cv);
			}
			if (cv->changed)
				return false;
		}
		if (LionPageIsRightmost(set.page))
			break;
		if (!lion_verify_leaves_read(vs, &set,
									 LionPageGetOpaque(set.page)->rightlink,
									 exact))
			return false;
		CHECK_FOR_INTERRUPTS();
	}

	while (count && !cv->done)
	{
		lion_verify_covers_step(cv);
		if (cv->changed)
			return false;
	}
	return true;
}

/*
 * One walk of a position tree, level by level from the leaves up as
 * lion_verify_chain() walks a posting set, and with the same reasoning about
 * a walk that is not exact.  Returns false when the walk has to be repeated,
 * which only such a walk does.
 */
static bool
lion_verify_postree_walk(LionVerifyState *vs, const LionVerifyTree *tree,
						 bool exact, LionVerifySetResult *res)
{
	BlockNumber leftmost[LION_POSTING_MAX_HEIGHT + 1];
	LionVerifySeq lv[2];
	LionVerifyLeafSum sum;
	uint32		height;
	bool		ok;

	if (!lion_verify_tree_leftmost(vs, tree, leftmost, &height))
		return false;
	res->height = height;
	lion_verify_seq_init(&lv[0], &vs->seqacct, sizeof(LionVerifyPage));
	lion_verify_seq_init(&lv[1], &vs->seqacct, sizeof(LionVerifyPage));

	ok = lion_verify_pos_leaves(vs, tree, leftmost[0], exact ? NULL : &lv[0],
								&sum, exact, res) &&
		(height > 0 || lion_verify_flat(vs, tree, &sum, exact)) &&
		lion_verify_tree_levels(vs, tree, leftmost, height,
								exact ? NULL : lv, sum.npages, exact, res);
	lion_verify_seq_free(&lv[0]);
	lion_verify_seq_free(&lv[1]);
	return ok;
}

/*
 * What a walk of a position tree and the comparison with its posting set
 * found, against what the entry says: as many members as TIDs at least, and
 * no TID without positions.  With report false nothing is reported, and the
 * answer is whether there is anything to report.  With count false the
 * members are not counted against the entry, which a standby has not read
 * at the replay position it read the trees at (lion_verify_postree_walks()).
 */
static bool
lion_verify_postree_totals(LionVerifyState *vs, BlockNumber blk,
						   OffsetNumber off, const LionEntryTuple *cur,
						   const LionVerifySetResult *res,
						   const LionVerifyCovers *cv, bool report,
						   bool count)
{
	if (!report)
		return (!count || res->card >= lion_entry_ntids(cur)) &&
			cv->missing == 0;

	if (count && res->card < lion_entry_ntids(cur))
		lion_corrupt("lion index \"%s\": chain entry %u on block %u has " UINT64_FORMAT " TIDs but positions for " UINT64_FORMAT " rows",
					RelationGetRelationName(vs->index), off, blk,
					lion_entry_ntids(cur), res->card);
	if (cv->missing > 0)
	{
		ItemPointerData tid;

		lion_code_to_tid(cv->first_missing, &tid);
		lion_corrupt("lion index \"%s\": " UINT64_FORMAT " TIDs of chain entry %u on block %u have no positions, the first (%u,%u)",
					RelationGetRelationName(vs->index), cv->missing, off, blk,
					ItemPointerGetBlockNumber(&tid),
					ItemPointerGetOffsetNumber(&tid));
	}
	return true;
}

/*
 * Settle, on a standby, the comparison of a position tree with its posting
 * set that a walk of it found a difference in while replay changed a page
 * it read, with at most LION_VERIFY_STANDBY_PASSES covers passes - the
 * comparison of lion_verify_pos_covers() alone, with the tree's members
 * counted - each between two reads of the entry, as a walk is.  A pass that
 * found no difference settles it; so does one that read every page of the
 * two trees at or below the replay position it began at, which reports what
 * it found, the count against the entry only when it read the entry's leaf
 * at that position too.  A pass reads the leaves of the two trees and one
 * page a level, a fraction of a walk, and is far likelier to read one point
 * of the WAL under a steady replay.  True when a pass settled it, or the
 * entry has gone (*gonep); false when none did.
 */
static bool
lion_verify_postree_count(LionVerifyState *vs, LionEntryTuple *cur,
						  const LionVerifyTree *tree, LionVerifySeen *seen,
						  bool *gonep)
{
	int			pass;

	*gonep = false;
	for (pass = 0; pass < LION_VERIFY_STANDBY_PASSES; pass++)
	{
		LionVerifySetResult cnt;
		LionVerifyCovers cv;
		bool		ok;
		bool		treesat;
		bool		entryat;

		CHECK_FOR_INTERRUPTS();
		vs->nposcovers++;
		lion_verify_lsn_begin(vs);
		if (!lion_verify_refind(vs, cur, seen, NULL))
		{
			lion_verify_lsn_end(vs);
			*gonep = true;
			return true;
		}
		lion_verify_lsn_forget(vs);
		ok = lion_verify_pos_covers(vs, cur, tree, false, true, &cv);
		treesat = lion_verify_lsn_at(vs);
		if (!lion_verify_refind(vs, cur, seen, NULL))
		{
			lion_verify_lsn_end(vs);
			*gonep = true;
			return true;
		}
		entryat = lion_verify_lsn_at(vs);
		lion_verify_lsn_end(vs);
		if (!ok)
			continue;

		memset(&cnt, 0, sizeof(cnt));
		cnt.card = cv.nmembers;
		if (lion_verify_postree_totals(vs, seen->blk, seen->off, cur, &cnt,
									   &cv, false, true))
			return true;
		if (treesat)
		{
			(void) lion_verify_postree_totals(vs, seen->blk, seen->off, cur,
											  &cnt, &cv, true, entryat);
			if (entryat)
				return true;
		}
	}
	return false;
}

/*
 * The position tree of a chain entry that stores positions (DESIGN.md §17),
 * checked as its posting set is: every page visited once, a position page of
 * this tree, stamped as the entry's, at the level its link promises; every
 * level's keys and links, and its downlinks matched with the level below
 * (lion_verify_tree_levels()); every chunk and member
 * (lion_verify_pos_leaves()); and at least as many members as the entry has
 * TIDs and positions for every one of those TIDs (lion_verify_pos_covers()),
 * since a row's positions are written before its TID and removed after it.
 *
 * On a primary the tree is walked as lion_verify_set_walks() walks a posting
 * set: the entry read, the tree walked with nothing held, the entry read
 * again afterwards, and the walk kept when the entry reads the same both
 * times.  Every writer of the key holds the entry's leaf EXCLUSIVE from its
 * first record to its last, the positions it writes among them (W1), and the
 * entry was read under a SHARE lock both times, so a writer that changed the
 * tree in between did all of it in between - and every INSERT that gets to
 * its TID changes the entry.  One that fails between its two writes does
 * not, and leaves members no TID of the set has (lion_vac_delete_group()); a
 * walk it changed the tree under can have missed members, which it moves
 * right in a split of a leaf the walk had read.  So such a walk is exact
 * about what it found - the set the covers pass read is the entry's,
 * unchanged, and no member the walk saw is one a writer took away - but not
 * about what it did not find: a walk that found every TID of the set among
 * the members is kept, and one that found too few members or a TID without
 * them is a suspicion, which a walk with the leaf held SHARE settles
 * (DESIGN.md §7, "suspect, wait, recheck").  So is a tree whose writers keep
 * changing it, after LION_VERIFY_SET_ATTEMPTS walks, as a posting set is.
 * That hold is the lock order every writer uses (directory page before
 * position page), and it keeps waiting only the writers of the keys on that
 * one leaf, for one walk of one tree.
 *
 * Neither walk keeps more of the tree than its levels' sequences
 * (LionVerifySeq): the comparison reads the two trees' leaves in step and
 * collects nothing (lion_verify_pos_covers()), so a key of any size costs it
 * two pages.
 *
 * On a standby the leaf is never held while a tree is walked
 * (lion_verify_set_walks() says why), and the entry reading the same is no
 * settlement either: replay applies a writer's records one at a time, with
 * nothing held between them, and VACUUM, which the primary's check locks
 * out, is replayed beside a standby's - a TID taken out of the set, its
 * positions after it (V1), and another row's TID put in can leave the entry
 * as it was while the walk read the set before the first and the tree after
 * the second.  What does settle a walk there is the pages' LSNs.  Replay
 * sets the LSN of every page a record changes to the end of the record, and
 * GetXLogReplayRecPtr() is the end of the last record replayed: a walk that
 * notes that position before it reads anything, and reads no page - of the
 * tree, of the posting set, the entry's directory leaf - whose LSN is past
 * it, read the index exactly as it was at that one point of the primary's
 * WAL.  There every TID has its positions, since W1 and V1 order the records
 * that way and an INSERT or a VACUUM stopped between two of them leaves only
 * members without TIDs; so a difference such a walk finds is damage, and is
 * reported, the count only when the entry's leaf was read at that point too.
 * A walk that found no difference is kept whatever its pages' LSNs, as on a
 * primary: what it found is there.  One that found a difference after replay
 * changed a page it read is kept for what it checked, and the comparison
 * settled by covers passes (lion_verify_postree_count()) - and when none of
 * those settles it either, it is left unsettled, which a WARNING says; with
 * replay paused nothing changes a page, and the first walk settles it.  A
 * walk replay changed the tree's shape under - split it, pushed its root
 * down, freed it - is thrown away and repeated, at most
 * LION_VERIFY_STANDBY_WALKS times, and an entry replay took away ends the
 * check of its tree (lion_verify_refind()).
 */
static void
lion_verify_postree_walks(LionVerifyState *vs, BlockNumber eblk,
						  OffsetNumber eoff, XLogRecPtr elsn,
						  const LionEntryTuple *entry)
{
	Size		sz = LionEntryPayloadOffset(entry);
	LionEntryTuple *cur = (LionEntryTuple *) palloc(sz);
	LionVerifySeen seen;
	Buffer		leaf;
	LionVerifyTree tree;
	LionVerifySetResult res;
	LionVerifyCovers cv;
	int			attempt;

	/*
	 * The tree's root is in the entry's extension, which the entry keeps
	 * from the spill that made it a CHAIN entry for as long as it lives: a
	 * push-down keeps the root's block (DESIGN.md §22).
	 */
	memcpy(cur, entry, sz);
	seen.blk = eblk;
	seen.off = eoff;
	seen.lsn = elsn;
	lion_verify_tree_init(&tree, eblk, eoff, cur, true);

	if (!vs->concurrent)
	{
		for (attempt = 0;; attempt++)
		{
			bool		ok;
			bool		treesat;
			bool		entryat;
			bool		gone;

			lion_verify_result_init(vs, &res);
			lion_verify_track_begin(vs);
			vs->nposwalks++;

			lion_verify_lsn_begin(vs);
			if (!lion_verify_refind(vs, cur, &seen, NULL))
			{
				lion_verify_lsn_end(vs);
				lion_verify_track_end(vs, false);
				lion_verify_result_free(&res);
				pfree(cur);
				return;
			}
			lion_verify_lsn_forget(vs);
			ok = lion_verify_postree_walk(vs, &tree, false, &res) &&
				lion_verify_pos_covers(vs, cur, &tree, false, false, &cv);
			treesat = lion_verify_lsn_at(vs);
			if (!lion_verify_refind(vs, cur, &seen, NULL))
			{
				lion_verify_lsn_end(vs);
				lion_verify_track_end(vs, true);
				lion_verify_result_free(&res);
				pfree(cur);
				return;
			}
			entryat = lion_verify_lsn_at(vs);
			lion_verify_lsn_end(vs);

			if (ok)
			{
				lion_verify_track_end(vs, true);
				lion_verify_warn_flagged(vs, &res);
				if (lion_verify_postree_totals(vs, seen.blk, seen.off, cur,
											   &res, &cv, false, true))
				{
					lion_verify_result_free(&res);
					pfree(cur);
					return;
				}

				/* a difference, which stands if the walk read one replay point */
				if (treesat)
				{
					(void) lion_verify_postree_totals(vs, seen.blk, seen.off,
													  cur, &res, &cv, true,
													  entryat);
					if (entryat)
					{
						lion_verify_result_free(&res);
						pfree(cur);
						return;
					}
				}
				lion_verify_result_free(&res);

				if (!lion_verify_postree_count(vs, cur, &tree, &seen, &gone))
				{
					vs->nposuncompared++;
					ereport(WARNING,
							(errmsg("lion index \"%s\": the position tree of chain entry %u on block %u was not compared with its posting set",
									RelationGetRelationName(vs->index),
									seen.off, seen.blk),
							 errdetail("Recovery changed the two during the walk that found a difference between them and during each of the %d passes that compared them again; every page of the walk was checked.",
									   LION_VERIFY_STANDBY_PASSES),
							 errhint("Run the check with replay paused (pg_wal_replay_pause(), then pg_wal_replay_resume()) to compare them.")));
				}
				pfree(cur);
				return;
			}

			/* replay got in; forget what this walk marked, and walk again */
			lion_verify_track_end(vs, false);
			lion_verify_result_free(&res);
			vs->nposretries++;
			if (attempt + 1 >= LION_VERIFY_STANDBY_WALKS)
				ereport(ERROR,
						(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
						 errmsg("lion index \"%s\": could not walk the position tree of chain entry %u on block %u to its end",
								RelationGetRelationName(vs->index), seen.off,
								seen.blk),
						 errdetail("Recovery changed the tree during each of the %d walks made of it.",
								   attempt + 1),
						 errhint("Run the check with replay paused (pg_wal_replay_pause(), then pg_wal_replay_resume()).")));
		}
	}

	for (attempt = 0; attempt < LION_VERIFY_SET_ATTEMPTS; attempt++)
	{
		union
		{
			LionEntryTuple e;
			LionEntryChainHdr c;	/* room for either header form */
		}			before;
		bool		ok;

		/*
		 * Test hook: the last walk was thrown away, the entry is read again,
		 * and nothing is held.  test/isolation/verify_positions.spec parks
		 * here and at lion-verify-postree-walked in turn, as
		 * verify_concurrent.spec does at the posting set's two points.
		 */
		if (attempt > 0)
			LION_INJECTION_POINT("lion-verify-postree-rewalk");

		lion_verify_result_init(vs, &res);
		lion_verify_track_begin(vs);
		vs->nposwalks++;
		(void) lion_verify_refind(vs, cur, &seen, NULL);
		memcpy(&before, cur, LionEntryHdrSize(cur));
		ok = lion_verify_postree_walk(vs, &tree, false, &res) &&
			lion_verify_pos_covers(vs, cur, &tree, false, false, &cv);

		/* Test hook: a walk is done, and the entry not yet read again. */
		LION_INJECTION_POINT("lion-verify-postree-walked");

		(void) lion_verify_refind(vs, cur, &seen, NULL);
		if (ok && lion_verify_same_entry(&before.e, cur))
		{
			if (lion_verify_postree_totals(vs, seen.blk, seen.off, cur, &res,
										   &cv, false, true))
			{
				lion_verify_track_end(vs, true);
				lion_verify_warn_flagged(vs, &res);
				lion_verify_result_free(&res);
				pfree(cur);
				return;
			}

			/*
			 * A suspicion (above), which the walk below settles.  It visits
			 * the pages of the tree again, so this walk's marks go.
			 */
			lion_verify_track_end(vs, false);
			lion_verify_result_free(&res);
			break;
		}

		/* A writer got in; forget what this walk marked, and walk again. */
		lion_verify_track_end(vs, false);
		lion_verify_result_free(&res);
		vs->nposretries++;
	}

	vs->nposwalks++;
	vs->nposholds++;
	(void) lion_verify_refind(vs, cur, &seen, &leaf);

	/*
	 * Test hook: the leaf is held and the exact walk is about to begin.
	 * test/isolation/verify_positions.spec attaches 'notice' here to show
	 * that a tree a writer kept changing got this walk.
	 */
	LION_INJECTION_POINT("lion-verify-postree-held");

	lion_verify_result_init(vs, &res);
	if (!lion_verify_postree_walk(vs, &tree, true, &res) ||
		!lion_verify_pos_covers(vs, cur, &tree, true, false, &cv))
		elog(ERROR, "an exact walk of a position tree was not finished");
	lion_verify_warn_flagged(vs, &res);
	(void) lion_verify_postree_totals(vs, seen.blk, seen.off, cur, &res, &cv,
									  true, true);
	lion_verify_result_free(&res);
	UnlockReleaseBuffer(leaf);
	pfree(cur);
}

/*
 * lion_verify_postree_walks() in the memory context lion_verify_set() uses,
 * emptied after every tree: the levels of a walk, and of every walk thrown
 * away, are garbage the moment it is done.
 */
static void
lion_verify_postree(LionVerifyState *vs, BlockNumber eblk, OffsetNumber eoff,
					XLogRecPtr elsn, const LionEntryTuple *entry)
{
	MemoryContext oldcxt = MemoryContextSwitchTo(vs->setcxt);

	lion_verify_postree_walks(vs, eblk, eoff, elsn, entry);
	MemoryContextSwitchTo(oldcxt);
	MemoryContextReset(vs->setcxt);
}

/*
 * Check one entry tuple.
 *
 * The walk has put every item of the page through lion_verify_dir_item()
 * already, but this does not lean on it for the order of its own reads: the
 * size is checked before any header field is read, and the key's extent and
 * length before the key is hashed.
 */
void
lion_verify_entry(LionVerifyState *vs, BlockNumber blk,
				 OffsetNumber off, ItemId iid, Page page)
{
	LionEntryTuple *entry = (LionEntryTuple *) PageGetItem(page, iid);
	Size		itemsz = ItemIdGetLength(iid);
	uint16		kind;
	LionState  *state;
	uint64	   *poscodes = NULL;	/* an INLINE entry's positions, by row */
	uint32		nposcodes = 0;

	if (itemsz < LION_ENTRY_SHORT_HDRSZ)
		lion_corrupt("lion index \"%s\": entry %u on block %u is only %zu bytes",
					RelationGetRelationName(vs->index), off, blk, itemsz);

	if (itemsz < LionEntryPayloadOffset(entry))
		lion_corrupt("lion index \"%s\": entry %u on block %u is %zu bytes, too small for its %u byte key",
					RelationGetRelationName(vs->index), off, blk, itemsz,
					entry->keylen);

	kind = entry->flags & (LION_ENTRY_INLINE | LION_ENTRY_CHAIN);

	/*
	 * The KEY COLUMN (DESIGN.md §24).  Everything below - the key length, the
	 * hash, whether an EMPTY entry may exist at all - is decided by that
	 * column's opclass, and the columns' entry runs must come out in attno
	 * order, which the directory comparator enforces item by item and this
	 * checks again across pages.
	 */
	if (entry->attno < 1 || entry->attno > vs->ix->ncolumns)
		lion_corrupt("lion index \"%s\": entry %u on block %u belongs to key column %u, but the index has %d",
					RelationGetRelationName(vs->index), off, blk, entry->attno,
					vs->ix->ncolumns);
	if (entry->attno < vs->lastattno)
		lion_corrupt("lion index \"%s\": entry %u on block %u belongs to key column %u, below the column %u of the entry before it",
					RelationGetRelationName(vs->index), off, blk, entry->attno,
					vs->lastattno);
	vs->lastattno = entry->attno;
	state = lion_column(vs->ix, (AttrNumber) entry->attno);

	if (kind == LION_ENTRY_CHAIN && LionEntryChainHeader(entry)->unused != 0)
		lion_corrupt("lion index \"%s\": entry %u on block %u has a non-zero reserved header field",
					RelationGetRelationName(vs->index), off, blk);

	if (kind != LION_ENTRY_INLINE && kind != LION_ENTRY_CHAIN)
		lion_corrupt("lion index \"%s\": entry %u on block %u has flags 0x%04X, expected exactly one of INLINE and CHAIN",
					RelationGetRelationName(vs->index), off, blk, entry->flags);

	if ((entry->flags & ~(uint16) (LION_ENTRY_INLINE | LION_ENTRY_CHAIN |
								   LION_ENTRY_RESERVED |
								   LION_ENTRY_DERIVED |
								   LION_ENTRY_POSITIONS |
								   LION_ENTRY_ALIGNKEY)) != 0)
		lion_corrupt("lion index \"%s\": entry %u on block %u has unknown flag bits in 0x%04X",
					RelationGetRelationName(vs->index), off, blk, entry->flags);

	/*
	 * Where the key is (lion_entryfmt.h): double-aligned exactly when the key
	 * column's type needs it.  A reserved entry has no key, and no flag.
	 */
	if ((entry->flags & LION_ENTRY_ALIGNKEY) !=
		((LionEntryIsReserved(entry) || LionEntryIsSlice(entry)) ? 0 :
		 lion_key_layout_flags(state)))
		lion_corrupt("lion index \"%s\": entry %u on block %u has flags 0x%04X, which place its key where key column %u does not keep it",
					RelationGetRelationName(vs->index), off, blk, entry->flags,
					entry->attno);

	/*
	 * A SUMMARY entry (DESIGN.md §32): only in a column the meta page says has
	 * summaries, never a reserved one, the open bucket's flag only with the
	 * summary flag, and one open bucket per column.  What it holds is checked
	 * by lion_verify_summaries() once the structure is known to be sound.
	 */
	if (LionEntryIsSummary(entry))
	{
		if (!state->summarized)
			lion_corrupt("lion index \"%s\": entry %u on block %u is a summary of key column %u, which has none",
						RelationGetRelationName(vs->index), off, blk,
						entry->attno);
		if (LionEntryIsReserved(entry) ||
			(entry->flags & LION_ENTRY_SUMMARY) == 0)
			lion_corrupt("lion index \"%s\": entry %u on block %u has flags 0x%04X, which no summary has",
						RelationGetRelationName(vs->index), off, blk,
						entry->flags);
		if (LionEntryIsSumLast(entry) &&
			++vs->nlastsummaries[entry->attno - 1] > 1)
			lion_corrupt("lion index \"%s\": entry %u on block %u is a second open summary for key column %u",
						RelationGetRelationName(vs->index), off, blk,
						entry->attno);
	}

	if (pg_popcount32((uint32) (entry->flags & LION_ENTRY_RESERVED)) > 1)
		lion_corrupt("lion index \"%s\": entry %u on block %u has flags 0x%04X, which name two reserved entries",
					RelationGetRelationName(vs->index), off, blk,
					entry->flags);

	if (LionEntryIsReserved(entry))
	{
		/*
		 * A reserved entry (DESIGN.md §14 and §17): no key bytes, and one of
		 * each per key column at most - a second one would split its rows
		 * between two entries that no reader looks for twice.
		 */
		int			rkind = lion_reserved_kind(entry->flags & LION_ENTRY_RESERVED);
		const char *what = lion_reserved_name(rkind);
		int64	   *seen = vs->nreservedentries[rkind - LION_KIND_NULL];

		if (entry->keylen != 0)
			lion_corrupt("lion index \"%s\": %s entry %u on block %u has a key of %u bytes",
						RelationGetRelationName(vs->index), what, off, blk,
						entry->keylen);
		if (++seen[entry->attno - 1] > 1)
			lion_corrupt("lion index \"%s\": entry %u on block %u is a second %s entry for key column %u",
						RelationGetRelationName(vs->index), off, blk, what,
						entry->attno);

		/*
		 * Only a multi-key opclass ever writes an empty or a null-elements
		 * entry, and only one that extracts an array's elements an
		 * empty-dimension or an empty-inner one; finding one anywhere else
		 * means the flag bits have been confused somewhere.
		 */
		if (!LionEntryIsNullKey(entry) && !state->multikey)
			lion_corrupt("lion index \"%s\": entry %u on block %u is %s %s entry, but the operator class of key column %u extracts no keys",
						RelationGetRelationName(vs->index), off, blk,
						(rkind == LION_KIND_EMPTY ||
						 rkind == LION_KIND_EMPTYDIM ||
						 rkind == LION_KIND_EMPTYINNER) ? "an" : "a", what,
						entry->attno);
		if ((rkind == LION_KIND_EMPTYDIM || rkind == LION_KIND_EMPTYINNER) &&
			!lion_extracts_array_elements(state->extractvalue.fn_addr))
			lion_corrupt("lion index \"%s\": entry %u on block %u is an %s entry, but the operator class of key column %u does not extract an array's elements",
						RelationGetRelationName(vs->index), off, blk, what,
						entry->attno);
	}
	else if (LionEntryIsSlice(entry))
	{
		/*
		 * A slice (§51): what lion_verify_dir_item() checked of its key and
		 * column, and a kind of its own - one of the others' flags beside
		 * it would make it two kinds.  What it holds is checked by
		 * lion_verify_slices() once the structure is known to be sound.
		 */
		if ((entry->flags & (LION_ENTRY_RESERVED | LION_ENTRY_SUMKINDS)) != 0)
			lion_corrupt("lion index \"%s\": entry %u on block %u has flags 0x%04X, which no slice has",
						RelationGetRelationName(vs->index), off, blk,
						entry->flags);
	}
	else
		lion_verify_keylen(vs, state, blk, off, entry);

	/*
	 * Stored positions (DESIGN.md §17): every key entry of a column that
	 * stores them has the extension, and no other entry does.
	 */
	if (((entry->flags & LION_ENTRY_POSITIONS) != 0) !=
		(state->positions && !LionEntryIsReserved(entry) &&
		 !LionEntryIsDerived(entry)))
		lion_corrupt("lion index \"%s\": entry %u on block %u %s positions, but key column %u %s",
					RelationGetRelationName(vs->index), off, blk,
					(entry->flags & LION_ENTRY_POSITIONS) != 0 ? "stores" : "does not store",
					entry->attno,
					state->positions ? "stores them for every key" : "stores none");
	if ((entry->flags & LION_ENTRY_POSITIONS) != 0)
	{
		const LionEntryPosExt *x = lion_entry_posext(entry);

		if (kind == LION_ENTRY_CHAIN ?
			(!BlockNumberIsValid(x->pos_root) || x->pos_len != 0) :
			(BlockNumberIsValid(x->pos_root) ||
			 x->pos_len > itemsz - LionEntryPayloadOffset(entry) ||
			 (itemsz - x->pos_len) != MAXALIGN(itemsz - x->pos_len) ||
			 (x->pos_len == 0) != (lion_entry_ntids(entry) == 0)))
			lion_corrupt("lion index \"%s\": %s entry %u on block %u has position root %u and %u bytes of inline positions",
						RelationGetRelationName(vs->index),
						kind == LION_ENTRY_CHAIN ? "chain" : "inline", off, blk,
						x->pos_root, x->pos_len);
		if (kind == LION_ENTRY_INLINE && x->pos_len > 0)
		{
			LionContainer *pc = (LionContainer *) palloc(LION_CONTAINER_MAX_SIZE);
			LionPosMember *m = (LionPosMember *) palloc(sizeof(LionPosMember));
			LionPosIter it;

			(void) lion_entry_inline_poschunk(vs->index, entry, itemsz, blk,
											  off, pc);
			if (pc->cardinality != lion_entry_ntids(entry))
				lion_corrupt("lion index \"%s\": inline entry %u on block %u holds positions for %u rows, but " UINT64_FORMAT " TIDs",
							RelationGetRelationName(vs->index), off, blk,
							(unsigned) pc->cardinality, lion_entry_ntids(entry));

			/*
			 * As many rows as TIDs, and every TID has to be one of them,
			 * which the payload's walk below looks up: positions kept for a
			 * row the set does not hold leave a row it does hold without
			 * any, and a phrase query then misses it.  The chunk has passed
			 * lion_poschunk_check(), so its codes ascend and are at most
			 * LION_POS_MAX_MEMBERS.
			 */
			poscodes = (uint64 *) palloc(sizeof(uint64) * Max(pc->cardinality, 1));
			lion_poschunk_iter_init(&it, pc);
			while (nposcodes < pc->cardinality && lion_poschunk_iter_next(&it, m))
				poscodes[nposcodes++] = m->code;
			pfree(m);
			pfree(pc);
		}
	}

	if (kind == LION_ENTRY_INLINE)
	{
		Size		paylen = LION_ENTRY_PAYLOAD_LEN(entry, itemsz);
		Size		cur = 0;
		Size		csize;
		bool		haveprev = false;
		uint32		prevckey = 0;
		uint64		card = 0;
		uint32		ncontainers = 0;
		LionVerifyCover cv;
		Size		limit;

		if (BlockNumberIsValid(lion_entry_head(entry)) || BlockNumberIsValid(lion_entry_tail(entry)))
			lion_corrupt("lion index \"%s\": inline entry %u on block %u has head %u and tail %u, expected none",
						RelationGetRelationName(vs->index), off, blk,
						lion_entry_head(entry), lion_entry_tail(entry));

		cv.codes = poscodes;
		cv.ncodes = nposcodes;
		cv.missing = 0;
		cv.first_missing = 0;
		while ((csize = lion_inline_fetch(vs->index, blk, off,
										  LionEntryGetPayload(entry), paylen,
										  &cur, vs->cbuf)) > 0)
		{
			lion_verify_container(vs, blk, off, vs->cbuf, csize,
								 &haveprev, &prevckey);
			card += vs->cbuf->cardinality;
			ncontainers++;
			if (poscodes == NULL)
				continue;
			if (vs->cbuf->type == LION_CT_SPARSE)
				lion_sparse_iterate(vs->cbuf, lion_verify_cover_pair, &cv);
			else
			{
				cv.ckey = vs->cbuf->ckey;
				lion_container_iterate(vs->cbuf, lion_verify_cover_lo, &cv);
			}
		}
		if (cv.missing > 0)
		{
			ItemPointerData tid;

			lion_code_to_tid(cv.first_missing, &tid);
			lion_corrupt("lion index \"%s\": " UINT64_FORMAT " TIDs of inline entry %u on block %u have no positions, the first (%u,%u)",
						RelationGetRelationName(vs->index), cv.missing, off,
						blk, ItemPointerGetBlockNumber(&tid),
						ItemPointerGetOffsetNumber(&tid));
		}
		if (poscodes != NULL)
			pfree(poscodes);

		/*
		 * The items and the positions together are what inline_limit bounds
		 * (DESIGN.md §4): ambuild, INSERT and VACUUM all spill a set past it
		 * - or past what a long key leaves of the largest entry,
		 * lion_inline_max() - onto pages, so that a rebuild can write again
		 * whatever they wrote.  The growth slack is bounded on its own,
		 * below.
		 */
		limit = lion_inline_max(Min(LionEntryPayloadOffset(entry),
									(Size) LION_MAX_ENTRY_SIZE - 1),
								(Size) vs->inline_limit);
		if (cur + lion_entry_pos_len(entry, itemsz) > limit)
			lion_corrupt("lion index \"%s\": inline entry %u on block %u holds %zu bytes of items and %zu of positions, past the %zu the meta page's inline_limit of %u leaves it",
						RelationGetRelationName(vs->index), off, blk, cur,
						lion_entry_pos_len(entry, itemsz), limit,
						vs->inline_limit);

		/*
		 * Whatever is left is the payload's growth slack: the zeroed tail an
		 * INSERT leaves so that the next member fits without rewriting the
		 * entry (DESIGN.md §4), or the one VACUUM leaves when it writes a
		 * shrunken payload back into the bytes the entry already had (§18).
		 * Both are bounded by the same constant, so that slack cannot hide a
		 * malformed payload, and every byte of it has to really be zero -
		 * which is also what makes it a terminator.
		 */
		if (paylen - cur > LION_ENTRY_SLACK_BOUND)
			lion_corrupt("lion index \"%s\": entry %u on block %u has %zu bytes of payload slack, at most %d allowed",
						RelationGetRelationName(vs->index), off, blk,
						paylen - cur, LION_ENTRY_SLACK_BOUND);
		{
			const char *pay = LionEntryGetPayload(entry);
			Size		i;

			for (i = cur; i < paylen; i++)
			{
				if (pay[i] != 0)
					lion_corrupt("lion index \"%s\": entry %u on block %u has a non-zero byte at payload offset %zu, past its last item",
								RelationGetRelationName(vs->index), off, blk, i);
			}
		}

		if (card != lion_entry_ntids(entry))
			lion_corrupt("lion index \"%s\": inline entry %u on block %u claims " UINT64_FORMAT " TIDs, but its payload holds " UINT64_FORMAT,
						RelationGetRelationName(vs->index), off, blk,
						lion_entry_ntids(entry), card);

		if (ncontainers != lion_entry_ncontainers(entry))
			lion_corrupt("lion index \"%s\": inline entry %u on block %u claims %u containers, but its payload holds %u",
						RelationGetRelationName(vs->index), off, blk,
						lion_entry_ncontainers(entry), ncontainers);
	}
	else
	{
		if (itemsz != LionEntryPayloadOffset(entry))
			lion_corrupt("lion index \"%s\": chain entry %u on block %u is %zu bytes, expected %zu",
						RelationGetRelationName(vs->index), off, blk, itemsz,
						LionEntryPayloadOffset(entry));

		if (!BlockNumberIsValid(lion_entry_head(entry)) || !BlockNumberIsValid(lion_entry_tail(entry)))
			lion_corrupt("lion index \"%s\": chain entry %u on block %u has head %u and tail %u",
						RelationGetRelationName(vs->index), off, blk,
						lion_entry_head(entry), lion_entry_tail(entry));

		lion_verify_set(vs, blk, off, PageGetLSN(page), entry);
		if ((entry->flags & LION_ENTRY_POSITIONS) != 0)
			lion_verify_postree(vs, blk, off, PageGetLSN(page), entry);
	}
}

/*
 * lion_index_verify(regclass, heapallindexed bool)
 *
 * The locking and the change of identity are amcheck's
 * (amcheck_lock_relation_and_check()), for amcheck's reasons.
 *
 * THE TABLE IS LOCKED BEFORE THE INDEX, which is the order every command that
 * takes both follows (DROP INDEX, REINDEX, the executor).  Locking the index
 * first deadlocks with a transaction that has locked the table and goes on to
 * drop the index: it waits for our index lock while we wait for its table
 * lock.  The index's table is therefore looked up before either is locked,
 * and the lookup is repeated once both are held, because a concurrent DROP
 * INDEX and CREATE INDEX could have given the Oid to another index meanwhile.
 * A relation that is not a lion index is told by its catalog row before
 * anything is locked, and opening it as an index under AccessShareLock alone
 * is what reports it: it takes neither lock, nor the owner's identity below.
 *
 * THE TABLE OWNER'S CODE RUNS AS THE TABLE OWNER.  With heapallindexed, the
 * heap scan evaluates the index's expressions and its predicate, and those
 * are functions the owner chose - and can replace after CREATE INDEX with
 * anything at all, IMMUTABLE label included.  Run as the caller, which is
 * normally a superuser checking somebody else's table, they would do whatever
 * the owner wrote with the superuser's rights (the class of CVE-2022-1552,
 * which amcheck and REINDEX fixed).  So everything from opening the index on
 * runs as the table owner, inside a SECURITY_RESTRICTED_OPERATION, with the
 * GUC changes those functions make confined to a nest level that is rolled
 * back when the check is over, and - on 17 and later, where the server's own
 * maintenance commands do the same - with search_path restricted to
 * pg_catalog and pg_temp.  On an ERROR the (sub)transaction abort restores
 * the identity and the settings; on the normal path this function does.
 *
 * The one decision that has to be the CALLER's is whether the error messages
 * may carry key values, so it is taken before the switch (showvalues).
 *
 * BOTH LOCKS ARE SHAREUPDATEEXCLUSIVELOCKS, the lock CREATE INDEX
 * CONCURRENTLY holds while it builds (see the file header): INSERT, UPDATE
 * and DELETE go on, while VACUUM - the one thing that removes anything from
 * the index - ANALYZE, DDL and a second verify() wait for the check, which
 * sees only the changes an INSERT makes and settles each of them (DESIGN.md
 * §7).  They are released when this function returns rather than at commit,
 * as amcheck does - nothing here sends an invalidation that could make that
 * unsafe.  During recovery only AccessShareLock is possible (and replay takes
 * no relation locks to be kept out by); DESIGN.md §7 says what that means.
 */
Datum
lion_index_verify(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	bool		heapallindexed = PG_GETARG_BOOL(1);
	bool		inrecovery = RecoveryInProgress();
	LOCKMODE	lockmode = inrecovery ? AccessShareLock : ShareUpdateExclusiveLock;
	LionVerifyState vs;
	Oid			heapoid;
	Oid			save_userid;
	int			save_sec_context;
	int			save_nestlevel;

	memset(&vs, 0, sizeof(vs));
	vs.showvalues = superuser();
	vs.concurrent = !inrecovery;

	/* before its table is locked: a partitioned index has one too */
	lion_refuse_partitioned_index(relid);

	/*
	 * Whether it is a lion index at all is asked of its catalog row before
	 * anything is locked or the identity changes: the table of a btree index
	 * would otherwise be locked against VACUUM, DDL and every other check of
	 * it - and wait behind them - and its owner become the user, all to say
	 * "not a lion index".  The row is read with no lock, which is why
	 * lion_open_index() asks again once both locks are held.
	 */
	heapoid = lion_is_lion_index(relid) ? IndexGetRelation(relid, true) :
		InvalidOid;
	if (!OidIsValid(heapoid))
	{
		/*
		 * Opening it as a lion index raises the error that says what it is.
		 * With AccessShareLock, because this is only to say that, and it
		 * should not wait behind a VACUUM of the table to say it.
		 */
		index_close(lion_open_index(relid, AccessShareLock), AccessShareLock);
		elog(ERROR, "could not find the table of index %u", relid);
	}
	vs.heap = table_open(heapoid, lockmode);

	GetUserIdAndSecContext(&save_userid, &save_sec_context);
	SetUserIdAndSecContext(vs.heap->rd_rel->relowner,
						   save_sec_context | SECURITY_RESTRICTED_OPERATION);
	save_nestlevel = NewGUCNestLevel();
	RestrictSearchPath();

	vs.index = lion_open_index(relid, lockmode);
	if (IndexGetRelation(relid, false) != heapoid)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_TABLE),
				 errmsg("could not open the table of index \"%s\"",
						RelationGetRelationName(vs.index))));

	vs.ix = lion_get_index_state(vs.index);
	vs.nblocks = vs.startblocks = RelationGetNumberOfBlocks(vs.index);
	vs.cbuf = (LionContainer *) palloc(LION_CONTAINER_MAX_SIZE);
	vs.refs = (uint8 *) palloc0(sizeof(uint8) * Max(vs.nblocks, 1));
	vs.setcxt = AllocSetContextCreate(CurrentMemoryContext,
									  "lion index verify posting set",
									  ALLOCSET_DEFAULT_SIZES);

	/*
	 * What the walks of the trees keep (LionVerifySeq): an equal share of
	 * work_mem for each sequence that can be alive at once, and a temporary
	 * file past it - in temp_tablespaces, looked up now, before any page is
	 * read, since the lookup reads catalogs.
	 */
	vs.seqacct.cap = Max((Size) work_mem * 1024 / LION_VERIFY_SEQS,
						 sizeof(LionVerifyPage));
	PrepareTempTablespaces();
	lion_verify_seq_init(&vs.setvisits, &vs.seqacct, sizeof(BlockNumber));
	lion_verify_seq_init(&vs.flags, &vs.seqacct, sizeof(LionVerifyFlag));

	/*
	 * On a standby, the replay position before anything of the index is
	 * read: a page whose LSN is past it is one replay has changed since the
	 * check began, which the parts of the check that read the index once
	 * - the directory's levels, the reachability pass - tell from damage by
	 * that (lion_verify_past_start()).
	 */
	vs.lsnstart = inrecovery ? GetXLogReplayRecPtr(NULL) : InvalidXLogRecPtr;

	lion_verify_meta(&vs);

	/*
	 * Test hook: the block count and the root are taken, nothing has been
	 * walked.  test/isolation/verify_concurrent.spec parks here and has
	 * writers split the directory's root underneath, and VACUUM wait.
	 */
	LION_INJECTION_POINT("lion-verify-meta-read");

	lion_verify_directory(&vs);
	lion_verify_reachable(&vs);
	lion_verify_recheck(&vs);
	lion_verify_flags_recheck(&vs);

	/*
	 * The structure is sound: now what the summaries hold (DESIGN.md §32),
	 * which reads the index the way the counts do.
	 */
	lion_verify_summaries(&vs);

	/* ... and what the bit slices hold (DESIGN.md §51) */
	lion_verify_slices(&vs);

	/*
	 * What writers running beside the check cost it (DESIGN.md §7), in the
	 * spirit of ambulkdelete's DEBUG1 breakdown.
	 */
	elog(DEBUG1, "lion index \"%s\": %u blocks at the start and %u at the end; %d candidates settled after %d waits; %lld left links and %lld root flags settled on the spot; %lld posting-set walks, %lld thrown away, %lld with the leaf held, %lld counting passes, %lld kept uncompared; %lld position-tree walks, %lld thrown away, %lld with the leaf held, %lld covers passes, %lld kept uncompared; %lld flagged pages looked at again",
		 RelationGetRelationName(vs.index), vs.startblocks, vs.nblocks,
		 vs.ncands, vs.nwaits, (long long) vs.nleftlinks,
		 (long long) vs.nrootsplits, (long long) vs.nsetwalks,
		 (long long) vs.nsetretries, (long long) vs.nsetholds,
		 (long long) vs.nsetcounts, (long long) vs.nsetuncompared,
		 (long long) vs.nposwalks, (long long) vs.nposretries,
		 (long long) vs.nposholds, (long long) vs.nposcovers,
		 (long long) vs.nposuncompared, (long long) vs.nflags);

	/*
	 * ... and what the walks of the trees kept (LionVerifySeq): never more
	 * memory than work_mem, whatever the size of a tree, and the rest in
	 * temporary files.
	 */
	elog(DEBUG1, "lion index \"%s\": the walks of its trees kept at most %zu bytes in memory and wrote %lld records to temporary files",
		 RelationGetRelationName(vs.index), vs.seqacct.peak,
		 (long long) vs.seqacct.spilled);

	if (heapallindexed)
	{
		/*
		 * Test hook: the structure is checked, the heap scan has not begun,
		 * and nothing is held.  test/isolation/verify_nopos.spec parks here
		 * and damages a position tree in the relation file behind the check,
		 * so that the scan meets a row its key holds without positions: the
		 * structural check reports such a row itself, and nothing a writer
		 * does can leave one between the two (DESIGN.md §17, W1 and V1).
		 */
		LION_INJECTION_POINT("lion-verify-heap-scan");
		lion_verify_heapallindexed(&vs);
	}

	MemoryContextDelete(vs.setcxt);
	lion_verify_seq_free(&vs.setvisits);
	lion_verify_seq_free(&vs.flags);
	pfree(vs.refs);
	pfree(vs.cbuf);

	/* Undo whatever settings the owner's functions changed, and ... */
	AtEOXact_GUC(false, save_nestlevel);
	/* ... be the caller again. */
	SetUserIdAndSecContext(save_userid, save_sec_context);

	index_close(vs.index, lockmode);
	table_close(vs.heap, lockmode);

	PG_RETURN_VOID();
}
