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

#include "lion_funcs.h"

PG_FUNCTION_INFO_V1(lion_index_verify);

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

	if (vs->settrack)
	{
		if (vs->nsetvisits >= vs->maxsetvisits)
		{
			/* repalloc() keeps the array in the context it was made in */
			vs->maxsetvisits *= 2;
			vs->setvisits = (BlockNumber *) repalloc(vs->setvisits,
													 sizeof(BlockNumber) * vs->maxsetvisits);
		}
		vs->setvisits[vs->nsetvisits++] = blk;
	}
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
	vs->nsetvisits = 0;
}

static void
lion_verify_track_end(LionVerifyState *vs, bool keep)
{
	int			i;

	if (!keep)
	{
		for (i = 0; i < vs->nsetvisits; i++)
		{
			Assert(vs->refs[vs->setvisits[i]] > 0);
			vs->refs[vs->setvisits[i]]--;
		}
	}
	vs->settrack = false;
	vs->nsetvisits = 0;
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
Page
lion_verify_read_page(LionVerifyState *vs, BlockNumber blk, uint16 kind,
					 Buffer *bufp)
{
	Buffer		buf;
	Page		page;
	PageHeader	phdr;
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

	if (PageIsNew(page))
		lion_corrupt("lion index \"%s\": block %u has never been initialised",
					RelationGetRelationName(vs->index), blk);

	/*
	 * The header bounds everything else on the page: the line pointer array
	 * ends at pd_lower, the items live in [pd_upper, pd_special).  A page read
	 * from disk had this checked by PageIsVerified(); one that was damaged in
	 * shared buffers did not.
	 */
	phdr = (PageHeader) page;
	if (phdr->pd_lower < SizeOfPageHeaderData ||
		phdr->pd_lower > phdr->pd_upper ||
		phdr->pd_upper > phdr->pd_special ||
		phdr->pd_special > BLCKSZ)
		lion_corrupt("lion index \"%s\": block %u has a page header with lower %u, upper %u and special %u",
					RelationGetRelationName(vs->index), blk,
					phdr->pd_lower, phdr->pd_upper, phdr->pd_special);

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

	flags = opaque->flags & LION_PAGE_KINDS;
	if (flags != LION_PAGE_META && flags != LION_PAGE_BUCKET &&
		flags != LION_PAGE_CONTAINER && flags != LION_PAGE_DIR)
		lion_corrupt("lion index \"%s\": block %u has flags 0x%04X, expected exactly one page kind",
					RelationGetRelationName(vs->index), blk, opaque->flags);

	/* Only a container page may ever carry the DELETED bit (DESIGN.md §18). */
	if ((opaque->flags & LION_PAGE_DELETED) != 0 && flags != LION_PAGE_CONTAINER)
		lion_corrupt("lion index \"%s\": block %u is marked deleted but is a %s page",
					RelationGetRelationName(vs->index), blk,
					flags == LION_PAGE_META ? "meta" :
					flags == LION_PAGE_DIR ? "directory" : "leaf");

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

	if (state->typbyval)
	{
		if (keylen != sizeof(Datum))
			lion_corrupt("lion index \"%s\": entry %u on block %u has key length %zu, expected %zu for a by-value type",
						RelationGetRelationName(vs->index), off, blk, keylen,
						sizeof(Datum));
	}
	else if (state->typlen > 0)
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
		 * is no longer accepted either, because no reader does.
		 */
		if (keylen < VARHDRSZ || !VARATT_IS_4B_U(key) ||
			VARSIZE(key) != keylen)
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
	if (itemsz < LION_ENTRY_HDRSZ)
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

	if (kind == LION_KIND_NULL || kind == LION_KIND_EMPTY)
	{
		/* the NULL and EMPTY entries, or pivots made from them (§14, §17) */
		if (item->keylen != 0)
			lion_corrupt("lion index \"%s\": %s item %u on block %u has a key of %u bytes",
						RelationGetRelationName(vs->index),
						kind == LION_KIND_NULL ? "null" : "empty", off, blk,
						item->keylen);
		return item;
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
	const uint32 *ckeys;
	uint32		n = c->cardinality;
	uint32		run = 1;
	uint32		i;

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

	ckeys = LION_SPARSE_CKEYS_CONST(c);
	for (i = 1; i <= n; i++)
	{
		if (i < n && ckeys[i] == ckeys[i - 1])
		{
			run++;
			continue;
		}
		if (run >= LION_SPARSE_THRESHOLD)
			lion_corrupt("lion index \"%s\": sparse segment %u on block %u holds %u members of container key %u, which needs a container of its own",
						RelationGetRelationName(vs->index), off, blk, run,
						ckeys[i - 1]);
		run = 1;
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
	 * DESIGN.md §38: a NARROW only in an index of version 8.  A build writes
	 * one only into such an index, VACUUM likewise, and inserts only widen
	 * one that is there; and a build of pg_lion before §38 reads version 6
	 * and 7 indexes, which have to stay what it can read.  Its width is
	 * lion_container_check()'s.  The version is the one the meta page says
	 * now, as lion_verify_meta() read it.
	 */
	if (c->type == LION_CT_NARROW && vs->version < LION_VERSION_NARROW)
		lion_corrupt("lion index \"%s\": container %u on block %u is a NARROW, which an index of version %u cannot hold",
					RelationGetRelationName(vs->index), off, blk,
					vs->version);

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
 * One level of a posting tree, as the walk of that level found it
 * (DESIGN.md §22).
 */
typedef struct LionVerifyPLevel
{
	int			npages;
	int			maxpages;
	BlockNumber *blocks;
	uint32	   *firstkey;		/* first separator, or first container key */
	bool	   *hasfirst;		/* false for an empty leaf */
	uint32	   *lastkey;		/* last container key of a leaf */
	uint32	   *highkey;		/* internal pages only */
	bool	   *hashigh;
	bool	   *incomplete;	/* flagged LION_PAGE_INCOMPLETE_SPLIT */
} LionVerifyPLevel;

static void
lion_verify_plevel_add(LionVerifyPLevel *lvl, BlockNumber blk, uint32 firstkey,
					   bool hasfirst, uint32 lastkey, uint32 highkey,
					   bool hashigh, bool incomplete)
{
	if (lvl->npages >= lvl->maxpages)
	{
		bool		first = (lvl->maxpages == 0);

		lvl->maxpages = first ? 64 : lvl->maxpages * 2;
		if (first)
		{
			lvl->blocks = (BlockNumber *) palloc(sizeof(BlockNumber) * lvl->maxpages);
			lvl->firstkey = (uint32 *) palloc(sizeof(uint32) * lvl->maxpages);
			lvl->hasfirst = (bool *) palloc(sizeof(bool) * lvl->maxpages);
			lvl->lastkey = (uint32 *) palloc(sizeof(uint32) * lvl->maxpages);
			lvl->highkey = (uint32 *) palloc(sizeof(uint32) * lvl->maxpages);
			lvl->hashigh = (bool *) palloc(sizeof(bool) * lvl->maxpages);
			lvl->incomplete = (bool *) palloc(sizeof(bool) * lvl->maxpages);
		}
		else
		{
			lvl->blocks = (BlockNumber *) repalloc(lvl->blocks, sizeof(BlockNumber) * lvl->maxpages);
			lvl->firstkey = (uint32 *) repalloc(lvl->firstkey, sizeof(uint32) * lvl->maxpages);
			lvl->hasfirst = (bool *) repalloc(lvl->hasfirst, sizeof(bool) * lvl->maxpages);
			lvl->lastkey = (uint32 *) repalloc(lvl->lastkey, sizeof(uint32) * lvl->maxpages);
			lvl->highkey = (uint32 *) repalloc(lvl->highkey, sizeof(uint32) * lvl->maxpages);
			lvl->hashigh = (bool *) repalloc(lvl->hashigh, sizeof(bool) * lvl->maxpages);
			lvl->incomplete = (bool *) repalloc(lvl->incomplete, sizeof(bool) * lvl->maxpages);
		}
	}
	lvl->blocks[lvl->npages] = blk;
	lvl->firstkey[lvl->npages] = firstkey;
	lvl->hasfirst[lvl->npages] = hasfirst;
	lvl->lastkey[lvl->npages] = lastkey;
	lvl->highkey[lvl->npages] = highkey;
	lvl->hashigh[lvl->npages] = hashigh;
	lvl->incomplete[lvl->npages] = incomplete;
	lvl->npages++;
}

/* The unfinished-split WARNING, which a set walk may defer (see below). */
static void
lion_verify_warn_posting_incomplete(LionVerifyState *vs, BlockNumber blk)
{
	ereport(WARNING,
			(errmsg("lion index \"%s\": posting page %u has an unfinished split",
					RelationGetRelationName(vs->index), blk),
			 errdetail("Its right sibling has no downlink in the parent yet."),
			 errhint("The next INSERT into that key repairs it.")));
}

/*
 * The page kind and owner checks every page of a posting set goes through.
 *
 * With `exact` false the walk is one lion_verify_set() may throw away and
 * repeat, because writers of the key can be changing the set under it; then
 * one check here is not an error but a reason to walk again - the ROOT at
 * another level than the descent found it at, which is what a push-down of
 * the root does (DESIGN.md §22: the root keeps its block and becomes the
 * level above) - and NULL comes back with the buffer released.  Every other
 * check here is exact whatever writers do: VACUUM, the only thing that frees
 * a page, is locked out, so a page reached through a link of the set belongs
 * to the set for as long as the check runs, and no page but the root ever
 * changes level.
 */
static Page
lion_verify_posting_page(LionVerifyState *vs, BlockNumber blk,
						 BlockNumber eblk, OffsetNumber eoff,
						 const LionEntryTuple *entry, uint16 level,
						 bool exact, LionVerifySetResult *res,
						 Buffer *bufp)
{
	Page		page;
	LionPageOpaque opaque;

	lion_verify_visit(vs, blk, "a posting tree");
	page = lion_verify_read_page(vs, blk, LION_PAGE_CONTAINER, bufp);
	opaque = LionPageGetOpaque(page);

	/*
	 * DESIGN.md §18.  A live entry must not reach a freed page at all, and
	 * every page of a posting set has to name that set: readers rely on both
	 * to tell a set they still hold a link to from one whose pages have been
	 * handed to somebody else.
	 */
	if (LionPageIsDeleted(page))
		lion_corrupt("lion index \"%s\": block %u is reachable from chain entry %u on block %u but is marked deleted",
					RelationGetRelationName(vs->index), blk, eoff, eblk);

	if (opaque->owner_head != entry->head ||
		opaque->owner_hash != entry->hash)
		lion_corrupt("lion index \"%s\": block %u of chain entry %u on block %u is owned by hash %u at head %u, expected hash %u at head %u",
					RelationGetRelationName(vs->index), blk, eoff, eblk,
					opaque->owner_hash, opaque->owner_head,
					entry->hash, entry->head);

	if (opaque->level != level)
	{
		if (!exact && blk == entry->head)
		{
			UnlockReleaseBuffer(*bufp);
			*bufp = InvalidBuffer;
			return NULL;
		}
		lion_corrupt("lion index \"%s\": posting page %u of chain entry %u on block %u is at level %u, expected %u",
					RelationGetRelationName(vs->index), blk, eoff, eblk,
					opaque->level, level);
	}

	/*
	 * A flag seen under a SHARE lock is a split that was abandoned, never one
	 * in progress: a writer holds the flagged page EXCLUSIVE from the record
	 * that sets the flag to the one that clears it (DESIGN.md §22), so this
	 * is exact on a primary.  A walk that may be repeated warns only once it
	 * is kept, which lion_verify_chain_totals() does.
	 */
	if (LionPageIncompleteSplit(page))
	{
		if (exact)
			lion_verify_warn_posting_incomplete(vs, blk);
		else
		{
			if (res->nincomplete >= res->maxincomplete)
			{
				res->maxincomplete = Max(res->maxincomplete * 2, 4);
				res->incomplete = (res->incomplete == NULL) ?
					(BlockNumber *) palloc(sizeof(BlockNumber) * res->maxincomplete) :
					(BlockNumber *) repalloc(res->incomplete,
											 sizeof(BlockNumber) * res->maxincomplete);
			}
			res->incomplete[res->nincomplete++] = blk;
		}
	}

	return page;
}

/*
 * Walk the LEAF chain of a posting set, left to right: the items of each page
 * and the ascending ckey order within and across pages.  Returns false when
 * the walk has to be repeated (see lion_verify_posting_page()).
 *
 * The order across pages is exact even with the key's writers running: a
 * split moves items only onto a page it links immediately right of the page
 * they came from, so everything the walk read on one page is below everything
 * on the page it read that page's right link from.  So is the set of items
 * the walk sees: an item that moves right in a split the walk has not reached
 * yet is met on its new page, one that moves after the walk read its page was
 * seen there.  What is NOT exact while writers run is how many items there
 * are and which leaf is the last, so those are only collected here and
 * compared by lion_verify_chain_totals() - except in an exact walk, which
 * checks the tail at once, as it always did.
 */
static bool
lion_verify_posting_leaves(LionVerifyState *vs, BlockNumber eblk,
						   OffsetNumber eoff, const LionEntryTuple *entry,
						   BlockNumber first, LionVerifyPLevel *out,
						   bool exact, LionVerifySetResult *res)
{
	BlockNumber blk = first;
	BlockNumber last = InvalidBlockNumber;
	bool		haveprev = false;
	uint32		prevckey = 0;

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

		page = lion_verify_posting_page(vs, blk, eblk, eoff, entry, 0, exact,
										res, &buf);
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

		lion_verify_plevel_add(out, blk, minckey,
							   firstused != InvalidOffsetNumber, maxckey, 0,
							   false, LionPageIncompleteSplit(page));

		last = blk;
		blk = opaque->rightlink;
		UnlockReleaseBuffer(buf);

		CHECK_FOR_INTERRUPTS();
	}

	res->last = last;
	if (exact && last != entry->tail)
		lion_corrupt("lion index \"%s\": chain entry %u on block %u ends at block %u, but its tail is block %u",
					RelationGetRelationName(vs->index), eoff, eblk, last,
					entry->tail);
	return true;
}

/*
 * Walk one INTERNAL level of a posting set, collecting its downlinks.
 * Returns false when the walk has to be repeated (lion_verify_posting_page()).
 */
static bool
lion_verify_posting_level(LionVerifyState *vs, BlockNumber eblk,
						  OffsetNumber eoff, const LionEntryTuple *entry,
						  BlockNumber first, uint16 level,
						  LionVerifyPLevel *out, LionVerifyPLevel *children,
						  bool exact, LionVerifySetResult *res)
{
	BlockNumber blk = first;

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

		page = lion_verify_posting_page(vs, blk, eblk, eoff, entry, level,
										exact, res, &buf);
		if (page == NULL)
			return false;
		maxoff = PageGetMaxOffsetNumber(page);
		firstdata = lion_posting_first_data(page);

		/* Every item is one whole pivot before any of them is read. */
		for (off = FirstOffsetNumber; off <= maxoff; off++)
		{
			ItemId		iid = lion_verify_itemid(vs, blk, page, off);

			if (!ItemIdIsUsed(iid) ||
				ItemIdGetLength(iid) != LION_POSTING_PIVOT_SIZE)
				lion_corrupt("lion index \"%s\": item %u of internal posting page %u is %zu bytes, expected %zu",
							RelationGetRelationName(vs->index), off, blk,
							(Size) ItemIdGetLength(iid),
							LION_POSTING_PIVOT_SIZE);
		}
		lion_page_check_items(vs->index, page, blk);

		if (!LionPageIsRightmost(page))
		{
			if (maxoff < FirstOffsetNumber)
				lion_corrupt("lion index \"%s\": internal posting page %u is not rightmost but has no high key",
							RelationGetRelationName(vs->index), blk);
			if (BlockNumberIsValid(lion_posting_pivot(page, FirstOffsetNumber)->child))
				lion_corrupt("lion index \"%s\": the first item of internal posting page %u is a downlink, not a high key",
							RelationGetRelationName(vs->index), blk);
			highkey = lion_posting_pivot(page, FirstOffsetNumber)->ckey;
			hashigh = true;
		}
		else if (maxoff >= FirstOffsetNumber &&
				 !BlockNumberIsValid(lion_posting_pivot(page, FirstOffsetNumber)->child))
			lion_corrupt("lion index \"%s\": rightmost internal posting page %u carries a high key",
						RelationGetRelationName(vs->index), blk);

		if (firstdata > maxoff)
			lion_corrupt("lion index \"%s\": internal posting page %u has no downlink",
						RelationGetRelationName(vs->index), blk);

		for (off = firstdata; off <= maxoff; off++)
		{
			LionPostingPivot *piv = lion_posting_pivot(page, off);

			if (!BlockNumberIsValid(piv->child))
				lion_corrupt("lion index \"%s\": item %u of internal posting page %u is a second high key",
							RelationGetRelationName(vs->index), off, blk);

			/*
			 * Separators are non-decreasing rather than strictly increasing: a
			 * split whose two halves are both empty by the time its repair runs
			 * gives the right one the left one's separator, which is a range of
			 * zero keys and routes everything to the right page (DESIGN.md §22).
			 */
			if (hasfirst && piv->ckey < prevkey)
				lion_corrupt("lion index \"%s\": downlink %u of internal posting page %u has separator %u, below the one before it (%u)",
							RelationGetRelationName(vs->index), off, blk,
							piv->ckey, prevkey);
			if (hashigh && piv->ckey >= highkey)
				lion_corrupt("lion index \"%s\": downlink %u of internal posting page %u has separator %u, not below its high key %u",
							RelationGetRelationName(vs->index), off, blk,
							piv->ckey, highkey);

			if (!hasfirst)
			{
				firstkey = piv->ckey;
				hasfirst = true;
			}
			prevkey = piv->ckey;

			if (children != NULL)
				lion_verify_plevel_add(children, piv->child, piv->ckey, true,
									   piv->ckey, 0, false, false);

			CHECK_FOR_INTERRUPTS();
		}

		lion_verify_plevel_add(out, blk, firstkey, hasfirst, prevkey, highkey,
							   hashigh, LionPageIncompleteSplit(page));

		blk = LionPageGetOpaque(page)->rightlink;
		UnlockReleaseBuffer(buf);

		CHECK_FOR_INTERRUPTS();
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
 */
static bool
lion_verify_chain(LionVerifyState *vs, BlockNumber eblk, OffsetNumber eoff,
				  const LionEntryTuple *entry, bool exact,
				  LionVerifySetResult *res)
{
	BlockNumber leftmost[LION_POSTING_MAX_HEIGHT + 1];
	LionVerifyPLevel *lvl;
	uint32		height = 0;
	uint32		i;
	int			j;

	if (!BlockNumberIsValid(entry->head) || !BlockNumberIsValid(entry->tail))
		lion_corrupt("lion index \"%s\": chain entry %u on block %u has head %u and tail %u",
					RelationGetRelationName(vs->index), eoff, eblk,
					entry->head, entry->tail);

	/*
	 * The leftmost page of every level, from the root down.  The pages are
	 * read, not visited: the level walks below visit them.  An ERROR raised
	 * with a buffer locked releases it on the way out, as everywhere else in
	 * this file.
	 */
	{
		BlockNumber blk = entry->head;
		int			steps = 0;
		uint16		expect = 0;

		for (i = 0; i <= LION_POSTING_MAX_HEIGHT; i++)
			leftmost[i] = InvalidBlockNumber;

		for (;;)
		{
			Buffer		buf;
			Page		page;
			uint16		level;
			OffsetNumber firstdata;
			ItemId		iid;

			if (steps++ > LION_POSTING_MAX_HEIGHT)
				lion_corrupt("lion index \"%s\": the posting tree of chain entry %u on block %u is deeper than %d levels",
							RelationGetRelationName(vs->index), eoff, eblk,
							LION_POSTING_MAX_HEIGHT);

			/* the block's range, the header and the page kind */
			page = lion_verify_read_page(vs, blk, LION_PAGE_CONTAINER, &buf);

			level = LionPageGetOpaque(page)->level;
			if (level > LION_POSTING_MAX_HEIGHT)
				lion_corrupt("lion index \"%s\": posting page %u claims level %u",
							RelationGetRelationName(vs->index), blk, level);
			if (blk == entry->head)
				height = level;
			else if (level != expect)
				lion_corrupt("lion index \"%s\": posting page %u is at level %u, but its parent is at level %u",
							RelationGetRelationName(vs->index), blk, level,
							expect + 1);
			if (!LionPageIsRightmost(page) && blk == entry->head)
				lion_corrupt("lion index \"%s\": the root %u of chain entry %u on block %u has a right sibling",
							RelationGetRelationName(vs->index), blk, eoff, eblk);
			leftmost[level] = blk;
			if (level == 0)
			{
				UnlockReleaseBuffer(buf);
				break;
			}

			firstdata = lion_posting_first_data(page);
			if (firstdata > PageGetMaxOffsetNumber(page))
				lion_corrupt("lion index \"%s\": internal posting page %u has no downlink",
							RelationGetRelationName(vs->index), blk);
			iid = lion_verify_itemid(vs, blk, page, firstdata);
			if (!ItemIdIsUsed(iid) ||
				ItemIdGetLength(iid) != LION_POSTING_PIVOT_SIZE)
				lion_corrupt("lion index \"%s\": item %u of internal posting page %u is %zu bytes, expected %zu",
							RelationGetRelationName(vs->index), firstdata, blk,
							(Size) ItemIdGetLength(iid),
							LION_POSTING_PIVOT_SIZE);

			/* checked against the index's length when it is read */
			blk = lion_posting_pivot(page, firstdata)->child;
			expect = level - 1;
			UnlockReleaseBuffer(buf);
		}
	}

	res->height = height;
	lvl = (LionVerifyPLevel *) palloc0(sizeof(LionVerifyPLevel) * (height + 1));

	if (!lion_verify_posting_leaves(vs, eblk, eoff, entry, leftmost[0], &lvl[0],
									exact, res))
		return false;

	/*
	 * Test hook: the leaves are walked, the levels above are not, and nothing
	 * is held.  test/isolation/verify_concurrent.spec parks here and has an
	 * INSERT split a leaf of this set or push its root down.  Never in an
	 * exact walk, which may hold a directory leaf that a writer would then
	 * wait for without isolationtester seeing it wait.
	 */
	if (!exact)
		LION_INJECTION_POINT("lion-verify-set-leaves-walked");

	for (i = 1; i <= height; i++)
	{
		LionVerifyPLevel children;
		const LionVerifyPLevel *below = &lvl[i - 1];
		int			p;

		memset(&children, 0, sizeof(children));
		if (!lion_verify_posting_level(vs, eblk, eoff, entry, leftmost[i],
									   (uint16) i, &lvl[i], &children, exact,
									   res))
			return false;

		/*
		 * A downlink to a page the walk of the level below did not see is a
		 * split made after that walk passed: walk the set again (see the
		 * header).  In an exact walk nothing can have split, and the
		 * comparison below reports such a downlink as the mismatch it is.
		 */
		if (!exact)
		{
			BlockNumber *seen = lion_verify_sorted_blocks(below->blocks,
														  below->npages);

			for (j = 0; j < children.npages; j++)
			{
				if (!lion_verify_has_block(seen, below->npages,
										   children.blocks[j]))
				{
					pfree(seen);
					return false;
				}
			}
			pfree(seen);
		}

		/*
		 * Match the downlinks, in the order the walk of this level found
		 * them, with the pages of the level below, in the order ITS walk
		 * found them.  j is the next downlink to match.
		 *
		 * One page may lack a downlink without being damage: the right
		 * sibling a split made, while the split is unfinished.  The split
		 * writes the sibling in one record and its downlink in the next, and
		 * leaves the LEFT page flagged LION_PAGE_INCOMPLETE_SPLIT in between;
		 * a crash - or an error - there leaves it so until the next writer
		 * that descends to the left page finishes it (DESIGN.md §22).  The
		 * sibling is then reachable through its left neighbour's right link
		 * and from nothing above, and the walk has already warned about the
		 * flag.  A three-way split flags both of its first two pages, so each
		 * missing downlink is covered by the page to its own left.  Such a
		 * page lies in its left neighbour's key range until the split
		 * finishes, and is bounded above by the next downlink's separator
		 * like any other page.
		 */
		j = 0;
		for (p = 0; p < below->npages; p++)
		{
			if (j < children.npages && children.blocks[j] == below->blocks[p])
			{
				uint32		sep = children.firstkey[j];

				if (j == 0 && sep != 0)
					lion_corrupt("lion index \"%s\": the first downlink of posting level %u has separator %u, expected minus infinity",
								RelationGetRelationName(vs->index), i, sep);

				/* the separator is at or below its child's own first key */
				if (below->hasfirst[p] && sep > below->firstkey[p])
					lion_corrupt("lion index \"%s\": the separator %u of posting block %u sorts after its own first container key %u",
								RelationGetRelationName(vs->index), sep,
								below->blocks[p], below->firstkey[p]);
				j++;
			}
			else if (p > 0 && below->incomplete[p - 1])
			{
				/* the right half of an unfinished split: no downlink yet */
			}
			else if (j < children.npages)
				lion_corrupt("lion index \"%s\": downlink %d of posting level %u points at block %u, but the next page of level %u is block %u",
							RelationGetRelationName(vs->index), j, i,
							children.blocks[j], i - 1, below->blocks[p]);
			else
				lion_corrupt("lion index \"%s\": posting level %u of chain entry %u on block %u has %d downlinks but level %u has %d pages",
							RelationGetRelationName(vs->index), i, eoff, eblk,
							children.npages, i - 1, below->npages);

			/* ... and the page's own upper bound is below the next one */
			if (j < children.npages)
			{
				uint32		next = children.firstkey[j];

				if (i == 1)
				{
					if (below->hasfirst[p] && below->lastkey[p] >= next)
						lion_corrupt("lion index \"%s\": leaf %u holds container key %u, at or above the separator %u of its right sibling",
									RelationGetRelationName(vs->index),
									below->blocks[p], below->lastkey[p], next);
				}
				else if (below->hashigh[p] && below->highkey[p] > next)
					lion_corrupt("lion index \"%s\": the high key %u of posting block %u sorts after the separator %u of its right sibling",
								RelationGetRelationName(vs->index),
								below->highkey[p], below->blocks[p], next);
			}
		}

		if (j < children.npages)
			lion_corrupt("lion index \"%s\": posting level %u of chain entry %u on block %u has %d downlinks but level %u has %d pages",
						RelationGetRelationName(vs->index), i, eoff, eblk,
						children.npages, i - 1, below->npages);
	}

	return true;
}

/*
 * What a walk of a posting set is compared with its entry by: the last leaf
 * against `tail`, the TIDs and items against `ntids` and `ncontainers`.  Only
 * for a walk that is kept: an exact one, or one lion_verify_set() found the
 * entry unchanged around - or, with compare false, the one a standby keeps of
 * a set that replay would not leave alone, which is compared with nothing.
 * Also where a kept walk's unfinished-split warnings are given, so that a walk
 * that is thrown away and repeated gives them once.
 */
static void
lion_verify_chain_totals(LionVerifyState *vs, BlockNumber eblk,
						 OffsetNumber eoff, const LionEntryTuple *entry,
						 const LionVerifySetResult *res, bool compare)
{
	int			i;

	for (i = 0; i < res->nincomplete; i++)
		lion_verify_warn_posting_incomplete(vs, res->incomplete[i]);

	if (res->height > vs->max_posting_height)
		vs->max_posting_height = res->height;

	if (!compare)
		return;

	if (res->last != entry->tail)
		lion_corrupt("lion index \"%s\": chain entry %u on block %u ends at block %u, but its tail is block %u",
					RelationGetRelationName(vs->index), eoff, eblk, res->last,
					entry->tail);

	if (res->card != entry->ntids)
		lion_corrupt("lion index \"%s\": chain entry %u on block %u claims " UINT64_FORMAT " TIDs, but its containers hold " UINT64_FORMAT,
					RelationGetRelationName(vs->index), eoff, eblk,
					entry->ntids, res->card);

	if (res->ncontainers != entry->ncontainers)
		lion_corrupt("lion index \"%s\": chain entry %u on block %u claims %u containers, but its posting tree holds %u",
					RelationGetRelationName(vs->index), eoff, eblk,
					entry->ncontainers, res->ncontainers);
}

/* Does the entry header read now say what the one read before said? */
static bool
lion_verify_same_entry(const LionEntryTuple *a, const LionEntryTuple *b)
{
	return a->flags == b->flags && a->head == b->head && a->tail == b->tail &&
		a->ntids == b->ntids && a->ncontainers == b->ncontainers;
}

/*
 * Find the entry `cur` names again, by its exact stored key, and copy its
 * header - flags, head, tail, counters - over cur's; the key, the hash and the
 * column cannot change.  *blkp is the leaf it was last seen on and the search
 * starts there and goes right: an entry only ever leaves its leaf in a split,
 * for the page the split links immediately to the right (DESIGN.md §21), and
 * nothing deletes an entry while VACUUM is locked out.  So the leaf whose high
 * key is above the key is where the entry is, and its absence there is
 * corruption.  With keep the leaf comes back locked SHARE (the last resort of
 * lion_verify_set()), else it is released.
 */
static Buffer
lion_verify_refind(LionVerifyState *vs, LionEntryTuple *cur, BlockNumber *blkp,
				   OffsetNumber *offp, bool keep)
{
	LionSearchKey sk;
	BlockNumber blk = *blkp;
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
							RelationGetRelationName(vs->index), *blkp);
			blk = next;
			CHECK_FOR_INTERRUPTS();
			continue;
		}

		off = lion_dir_binsrch(page, &sk);
		iid = (off <= maxoff) ? PageGetItemId(page, off) : NULL;
		item = (iid != NULL && ItemIdIsUsed(iid)) ?
			(LionEntryTuple *) PageGetItem(page, iid) : NULL;
		if (item == NULL || lion_cmp_entry(item, &sk) != 0)
			lion_corrupt("lion index \"%s\": entry %u on block %u is not on block %u, where its key sorts",
						RelationGetRelationName(vs->index), *offp, *blkp, blk);
		if ((item->flags & LION_ENTRY_CHAIN) != 0 &&
			ItemIdGetLength(iid) != LionEntryPayloadOffset(item))
			lion_corrupt("lion index \"%s\": chain entry %u on block %u is %zu bytes, expected %zu",
						RelationGetRelationName(vs->index), off, blk,
						(Size) ItemIdGetLength(iid),
						LionEntryPayloadOffset(item));

		cur->flags = item->flags;
		cur->head = item->head;
		cur->tail = item->tail;
		cur->ntids = item->ntids;
		cur->ncontainers = item->ncontainers;

		*blkp = blk;
		*offp = off;
		if (keep)
			return buf;
		UnlockReleaseBuffer(buf);
		return InvalidBuffer;
	}
}

/*
 * Walk and check the posting set of the CHAIN entry `entry`, which the walk
 * of the directory found at (eblk, eoff) - on its copy of the leaf, with the
 * leaf itself no longer locked (DESIGN.md §7).
 *
 * The counters of a set that writers are adding to cannot be compared with
 * its containers by a walk that holds nothing, and holding the entry's leaf
 * for every walk would keep the writers of every key on that leaf waiting
 * for it.  So the set is walked with nothing held, and the entry read again
 * afterwards, and the walk is kept when the entry reads the same both times.
 * That is exact, for two reasons.  Every writer of a key holds the directory
 * leaf of its entry EXCLUSIVE from its first record to its last (§5, §21),
 * and the entry was read under a SHARE lock both times, so a writer that
 * changed the set in between did all of it in between.  And every change an
 * INSERT makes to a set moves the entry: an item added, grown or split off
 * adds a TID to `ntids` in the record that places it, a push-down of the root
 * moves `tail`; the one kind of writer that adds no TID - one that finishes an
 * abandoned split on its way down and then finds its TID already there, or
 * fails - only adds a downlink and clears a flag, which the walk accepts
 * either way, and whatever internal split that makes shows up as a downlink
 * the walk of the level below never saw, which throws the walk away too.
 * VACUUM, the one writer that changes containers without adding TIDs, is
 * locked out.
 *
 * A set that keeps changing - a hot key - is walked at most
 * LION_VERIFY_SET_ATTEMPTS times that way, and then once more with the leaf
 * held SHARE throughout.  That is the lock order every writer uses (directory
 * page before posting page, §5), and it keeps waiting only the writers of the
 * keys on that one leaf, for one walk of one set.
 *
 * On a standby the set is walked the same way, and NEVER with its leaf held
 * (DESIGN.md §25).  Replay takes the blocks of a record in the order the
 * writer registered them and holds every one to the end of the record, and
 * every record that changes a set registers the set's pages before the
 * entry's leaf - so a walk holding the leaf waits for a posting page the
 * startup process holds while the startup process waits for the leaf, on two
 * buffer content locks, which no deadlock detector watches and no cancel
 * reaches: replay stopped for good, and so did this backend (2026-09-29
 * review; test/recovery/run.sh phase 4).  The entry reading the same both
 * times settles a walk there too, for replay's own reason: every record that
 * changes a set's items, its counters, its last leaf or the level of a leaf
 * root carries the entry, and the records that do not - a downlink, a flag
 * cleared, an internal page split, an internal root pushed down - change
 * nothing the entry counts and are what the walk accepts from a writer
 * between two levels, or throws away.  A set that replay keeps changing has
 * no last resort there, so after LION_VERIFY_SET_ATTEMPTS walks the next one
 * that gets to the end of the set is kept - every page it read is checked -
 * and its totals are not compared with the entry, which a WARNING says.
 * Only a root pushed down or an upper level split under the walk stops one
 * short of the end, and a set that does that to LION_VERIFY_STANDBY_WALKS
 * walks is an ERROR asking for replay to be paused.
 */
static void
lion_verify_set_walks(LionVerifyState *vs, BlockNumber eblk, OffsetNumber eoff,
					  const LionEntryTuple *entry)
{
	Size		sz = LionEntryPayloadOffset(entry);
	LionEntryTuple *cur = (LionEntryTuple *) palloc(sz);
	BlockNumber blk = eblk;
	OffsetNumber off = eoff;
	LionVerifySetResult res;
	Buffer		leaf;
	int			attempt;

	memcpy(cur, entry, sz);

	for (attempt = 0;; attempt++)
	{
		LionEntryTuple before = *cur;
		bool		ok;

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

		memset(&res, 0, sizeof(res));
		lion_verify_track_begin(vs);
		vs->nsetwalks++;
		ok = lion_verify_chain(vs, blk, off, cur, false, &res);
		(void) lion_verify_refind(vs, cur, &blk, &off, false);

		if (ok && lion_verify_same_entry(&before, cur))
		{
			lion_verify_track_end(vs, true);
			lion_verify_chain_totals(vs, blk, off, cur, &res, true);
			pfree(cur);
			return;
		}

		/* A standby's last resort: the walk, without the totals (above). */
		if (!vs->concurrent && ok && attempt + 1 >= LION_VERIFY_SET_ATTEMPTS &&
			(cur->flags & LION_ENTRY_CHAIN) != 0)
		{
			lion_verify_track_end(vs, true);
			lion_verify_chain_totals(vs, blk, off, cur, &res, false);
			vs->nsetuncompared++;
			ereport(WARNING,
					(errmsg("lion index \"%s\": the totals of chain entry %u on block %u were not compared with its posting set",
							RelationGetRelationName(vs->index), off, blk),
					 errdetail("Recovery changed the set during each of the %d walks made of it; every page of the last one was checked.",
							   attempt + 1),
					 errhint("Run the check with replay paused (pg_wal_replay_pause(), then pg_wal_replay_resume()) to compare them.")));
			pfree(cur);
			return;
		}

		/* A writer got in; forget what this walk marked, and walk again. */
		lion_verify_track_end(vs, false);
		vs->nsetretries++;
		if ((cur->flags & LION_ENTRY_CHAIN) == 0)
			break;				/* reported below */
		if (attempt + 1 >= (vs->concurrent ? LION_VERIFY_SET_ATTEMPTS :
							LION_VERIFY_STANDBY_WALKS))
			break;
	}

	if (!vs->concurrent)
	{
		if ((cur->flags & LION_ENTRY_CHAIN) == 0)
			lion_corrupt("lion index \"%s\": chain entry %u on block %u is an inline entry now",
						RelationGetRelationName(vs->index), off, blk);
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("lion index \"%s\": could not walk the posting set of chain entry %u on block %u to its end",
						RelationGetRelationName(vs->index), off, blk),
				 errdetail("Recovery changed the upper levels of the set during each of the %d walks made of it.",
						   attempt + 1),
				 errhint("Run the check with replay paused (pg_wal_replay_pause(), then pg_wal_replay_resume()).")));
	}

	vs->nsetwalks++;
	vs->nsetholds++;
	leaf = lion_verify_refind(vs, cur, &blk, &off, true);

	/*
	 * Test hook: the leaf is held and the exact walk is about to begin.
	 * test/isolation/verify_concurrent.spec attaches 'notice' here to show
	 * that a set a writer kept changing got this walk.
	 */
	LION_INJECTION_POINT("lion-verify-set-held");

	if ((cur->flags & LION_ENTRY_CHAIN) == 0)
		lion_corrupt("lion index \"%s\": chain entry %u on block %u is an inline entry now",
					RelationGetRelationName(vs->index), off, blk);
	memset(&res, 0, sizeof(res));
	(void) lion_verify_chain(vs, blk, off, cur, true, &res);
	lion_verify_chain_totals(vs, blk, off, cur, &res, true);
	UnlockReleaseBuffer(leaf);
	pfree(cur);
}

/*
 * lion_verify_set_walks() in a memory context of its own, emptied after every
 * set: the level arrays of a walk - and of every walk thrown away - are
 * garbage the moment the set is settled, and an index has as many sets as it
 * has keys with more than an entry's worth of rows.
 */
static void
lion_verify_set(LionVerifyState *vs, BlockNumber eblk, OffsetNumber eoff,
				const LionEntryTuple *entry)
{
	MemoryContext oldcxt = MemoryContextSwitchTo(vs->setcxt);

	lion_verify_set_walks(vs, eblk, eoff, entry);
	MemoryContextSwitchTo(oldcxt);
	MemoryContextReset(vs->setcxt);
}

/*
 * The position tree of a chain entry that stores positions (DESIGN.md §17):
 * every page visited once, a position page of this tree at the level its
 * link promises; every chunk well formed, the members ascending across the
 * whole tree and each chunk's reaching no further than the next one's header
 * ((P1) and (P2) of lion_postree.c); and at least as many members as the
 * entry has TIDs, since a row's positions are written before its TID and
 * removed after it.
 *
 * On a primary the entry's leaf is held SHARE for the walk, which keeps the
 * key's writers out (they hold it EXCLUSIVE for the whole insert), so the
 * tree and the count cannot move under it.  On a standby the leaf is never
 * held while a set is walked (lion_verify_set_walks() says why), so there the
 * walk checks the pages and leaves the count alone.
 */
static void
lion_verify_postree(LionVerifyState *vs, BlockNumber eblk, OffsetNumber eoff,
					const LionEntryTuple *entry)
{
	const LionEntryPosExt *x = lion_entry_posext(entry);
	BlockNumber root = x->pos_root;
	Size		sz = LionEntryPayloadOffset(entry);
	LionEntryTuple *cur = (LionEntryTuple *) palloc(sz);
	BlockNumber blk = eblk;
	OffsetNumber off = eoff;
	Buffer		leaf = InvalidBuffer;
	BlockNumber levelstart = root;
	int			level = -1;
	uint64		nmembers = 0;
	bool		any = false;
	uint64		lastcode = 0;
	uint32		lastblock = 0;
	bool		havechunk = false;
	uint32		lastheader = 0;
	LionPosMember *m = (LionPosMember *) palloc(sizeof(LionPosMember));
	char		err[256];

	memcpy(cur, entry, sz);
	if (vs->concurrent)
	{
		leaf = lion_verify_refind(vs, cur, &blk, &off, true);
		if ((cur->flags & LION_ENTRY_CHAIN) == 0)
			lion_corrupt("lion index \"%s\": chain entry %u on block %u is an inline entry now",
						RelationGetRelationName(vs->index), off, blk);
	}

	while (BlockNumberIsValid(levelstart))
	{
		BlockNumber pblk = levelstart;
		BlockNumber nextlevel = InvalidBlockNumber;

		while (BlockNumberIsValid(pblk))
		{
			Buffer		buf;
			Page		page;
			int			plevel;

			lion_verify_visit(vs, pblk, "a position tree page");
			buf = ReadBuffer(vs->index, pblk);
			LockBuffer(buf, BUFFER_LOCK_SHARE);
			page = BufferGetPage(buf);
			if (!lion_page_owns_positions(page, root))
				lion_corrupt("lion index \"%s\": block %u is not a page of the position tree at %u of entry %u on block %u",
							RelationGetRelationName(vs->index), pblk, root,
							off, blk);
			plevel = LionPageGetOpaque(page)->level;
			if (level < 0)
				level = plevel;
			if (plevel != level || plevel >= LION_POSTING_MAX_HEIGHT)
				lion_corrupt("lion index \"%s\": position page %u is at level %d, expected %d",
							RelationGetRelationName(vs->index), pblk, plevel,
							level);

			if (plevel > 0)
			{
				if (pblk == levelstart)
				{
					OffsetNumber first = lion_posting_first_data(page);

					if (PageGetMaxOffsetNumber(page) < first)
						lion_corrupt("lion index \"%s\": internal position page %u has no downlink",
									RelationGetRelationName(vs->index), pblk);
					nextlevel = lion_posting_downlink(vs->index, page, pblk,
													  first);
				}
			}
			else
			{
				OffsetNumber maxoff = PageGetMaxOffsetNumber(page);
				OffsetNumber o;

				for (o = FirstOffsetNumber; o <= maxoff; o++)
				{
					LionContainer *c = lion_page_poschunk_fetch(vs->index, page,
																pblk, o);
					LionPosIter it;
					bool		first = true;

					if (!lion_poschunk_check(c, ItemIdGetLength(PageGetItemId(page, o)),
											 err, sizeof(err)))
						lion_corrupt("lion index \"%s\": chunk %u on position page %u: %s",
									RelationGetRelationName(vs->index), o, pblk,
									err);
					if ((havechunk && c->ckey < lastheader) ||
						(any && lastblock > c->ckey))
						lion_corrupt("lion index \"%s\": chunk %u on position page %u, of header block %u, is out of order",
									RelationGetRelationName(vs->index), o, pblk,
									c->ckey);
					havechunk = true;
					lastheader = c->ckey;
					lion_poschunk_iter_init(&it, c);
					while (lion_poschunk_iter_next(&it, m))
					{
						if ((first && lion_pos_block(m->code) < c->ckey) ||
							(any && m->code <= lastcode))
							lion_corrupt("lion index \"%s\": the members of position page %u are out of order",
										RelationGetRelationName(vs->index), pblk);
						first = false;
						any = true;
						lastcode = m->code;
						lastblock = lion_pos_block(m->code);
						nmembers++;
					}
				}
			}

			pblk = LionPageIsRightmost(page) ? InvalidBlockNumber :
				LionPageGetOpaque(page)->rightlink;
			UnlockReleaseBuffer(buf);
			CHECK_FOR_INTERRUPTS();
		}
		if (level == 0)
			break;
		levelstart = nextlevel;
		level--;
	}

	if (BufferIsValid(leaf))
	{
		if (nmembers < cur->ntids)
			lion_corrupt("lion index \"%s\": chain entry %u on block %u has " UINT64_FORMAT " TIDs but positions for " UINT64_FORMAT " rows",
						RelationGetRelationName(vs->index), off, blk,
						cur->ntids, nmembers);
		UnlockReleaseBuffer(leaf);
	}
	pfree(m);
	pfree(cur);
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

	if (itemsz < LION_ENTRY_HDRSZ)
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

	if (entry->unused != 0)
		lion_corrupt("lion index \"%s\": entry %u on block %u has a non-zero reserved header field",
					RelationGetRelationName(vs->index), off, blk);

	if (kind != LION_ENTRY_INLINE && kind != LION_ENTRY_CHAIN)
		lion_corrupt("lion index \"%s\": entry %u on block %u has flags 0x%04X, expected exactly one of INLINE and CHAIN",
					RelationGetRelationName(vs->index), off, blk, entry->flags);

	if ((entry->flags & ~(uint16) (LION_ENTRY_INLINE | LION_ENTRY_CHAIN |
								   LION_ENTRY_RESERVED |
								   LION_ENTRY_SUMKINDS |
								   LION_ENTRY_POSITIONS)) != 0)
		lion_corrupt("lion index \"%s\": entry %u on block %u has unknown flag bits in 0x%04X",
					RelationGetRelationName(vs->index), off, blk, entry->flags);

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

	if ((entry->flags & LION_ENTRY_RESERVED) == LION_ENTRY_RESERVED)
		lion_corrupt("lion index \"%s\": entry %u on block %u is both the null and the empty entry",
					RelationGetRelationName(vs->index), off, blk);

	if (LionEntryIsReserved(entry))
	{
		/*
		 * A reserved entry (DESIGN.md §14 and §17): no key bytes, hash 0,
		 * bucket 0, and one of each per index at most - a second one would
		 * split its rows between two entries that no reader looks for twice.
		 */
		const char *what = LionEntryIsNullKey(entry) ? "null" : "empty";

		if (entry->keylen != 0)
			lion_corrupt("lion index \"%s\": %s entry %u on block %u has a key of %u bytes",
						RelationGetRelationName(vs->index), what, off, blk,
						entry->keylen);
		if (entry->hash != LION_NULLKEY_HASH)
			lion_corrupt("lion index \"%s\": %s entry %u on block %u stores hash %u, expected %d",
						RelationGetRelationName(vs->index), what, off, blk,
						entry->hash, LION_NULLKEY_HASH);
		if (LionEntryIsNullKey(entry) ?
			(++vs->nnullentries[entry->attno - 1] > 1) :
			(++vs->nemptyentries[entry->attno - 1] > 1))
			lion_corrupt("lion index \"%s\": entry %u on block %u is a second %s entry for key column %u",
						RelationGetRelationName(vs->index), off, blk, what,
						entry->attno);

		/*
		 * Only a multi-key opclass ever writes an empty entry; finding one in
		 * a scalar index means the two flag bits have been confused
		 * somewhere.
		 */
		if (LionEntryIsEmptyKey(entry) && !state->multikey)
			lion_corrupt("lion index \"%s\": entry %u on block %u is an empty-key entry, but the operator class of key column %u extracts no keys",
						RelationGetRelationName(vs->index), off, blk,
						entry->attno);
	}
	else
	{
		uint32		hash;

		lion_verify_keylen(vs, state, blk, off, entry);

		/* A summary's hash is a constant, not its key's (DESIGN.md §32). */
		hash = LionEntryIsSummary(entry) ? LION_SUMMARY_HASH :
			lion_hash_key(state,
						  lion_entry_key(state, entry));
		if (hash != entry->hash)
			lion_corrupt("lion index \"%s\": entry %u on block %u stores hash %u, but its key hashes to %u",
						RelationGetRelationName(vs->index), off, blk,
						entry->hash, hash);
	}

	/*
	 * Stored positions (DESIGN.md §17): every key entry of a column that
	 * stores them has the extension, and no other entry does.
	 */
	if (((entry->flags & LION_ENTRY_POSITIONS) != 0) !=
		(state->positions && !LionEntryIsReserved(entry) &&
		 !LionEntryIsSummary(entry)))
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
			 (x->pos_len == 0) != (entry->ntids == 0)))
			lion_corrupt("lion index \"%s\": %s entry %u on block %u has position root %u and %u bytes of inline positions",
						RelationGetRelationName(vs->index),
						kind == LION_ENTRY_CHAIN ? "chain" : "inline", off, blk,
						x->pos_root, x->pos_len);
		if (kind == LION_ENTRY_INLINE && x->pos_len > 0)
		{
			LionContainer *pc = (LionContainer *) palloc(LION_CONTAINER_MAX_SIZE);

			(void) lion_entry_inline_poschunk(vs->index, entry, itemsz, blk,
											  off, pc);
			if (pc->cardinality != entry->ntids)
				lion_corrupt("lion index \"%s\": inline entry %u on block %u holds positions for %u rows, but " UINT64_FORMAT " TIDs",
							RelationGetRelationName(vs->index), off, blk,
							(unsigned) pc->cardinality, entry->ntids);
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

		if (BlockNumberIsValid(entry->head) || BlockNumberIsValid(entry->tail))
			lion_corrupt("lion index \"%s\": inline entry %u on block %u has head %u and tail %u, expected none",
						RelationGetRelationName(vs->index), off, blk,
						entry->head, entry->tail);

		while ((csize = lion_inline_fetch(LionEntryGetPayload(entry), paylen,
										 &cur, vs->cbuf)) > 0)
		{
			lion_verify_container(vs, blk, off, vs->cbuf, csize,
								 &haveprev, &prevckey);
			card += vs->cbuf->cardinality;
			ncontainers++;
		}

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

		if (card != entry->ntids)
			lion_corrupt("lion index \"%s\": inline entry %u on block %u claims " UINT64_FORMAT " TIDs, but its payload holds " UINT64_FORMAT,
						RelationGetRelationName(vs->index), off, blk,
						entry->ntids, card);

		if (ncontainers != entry->ncontainers)
			lion_corrupt("lion index \"%s\": inline entry %u on block %u claims %u containers, but its payload holds %u",
						RelationGetRelationName(vs->index), off, blk,
						entry->ncontainers, ncontainers);
	}
	else
	{
		if (itemsz != LionEntryPayloadOffset(entry))
			lion_corrupt("lion index \"%s\": chain entry %u on block %u is %zu bytes, expected %zu",
						RelationGetRelationName(vs->index), off, blk, itemsz,
						LionEntryPayloadOffset(entry));

		if (!BlockNumberIsValid(entry->head) || !BlockNumberIsValid(entry->tail))
			lion_corrupt("lion index \"%s\": chain entry %u on block %u has head %u and tail %u",
						RelationGetRelationName(vs->index), off, blk,
						entry->head, entry->tail);

		lion_verify_set(vs, blk, off, entry);
		if ((entry->flags & LION_ENTRY_POSITIONS) != 0)
			lion_verify_postree(vs, blk, off, entry);
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
 * A relation that is not an index has no table at all; opening it as one is
 * then what reports that.
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

	heapoid = IndexGetRelation(relid, true);
	if (!OidIsValid(heapoid))
	{
		/*
		 * Not an index: opening it as one raises the error that says so.
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
	vs.maxsetvisits = 64;
	vs.setvisits = (BlockNumber *) palloc(sizeof(BlockNumber) * vs.maxsetvisits);
	vs.setcxt = AllocSetContextCreate(CurrentMemoryContext,
									  "lion index verify posting set",
									  ALLOCSET_DEFAULT_SIZES);

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

	/*
	 * The structure is sound: now what the summaries hold (DESIGN.md §32),
	 * which reads the index the way the counts do.
	 */
	lion_verify_summaries(&vs);

	/*
	 * What writers running beside the check cost it (DESIGN.md §7), in the
	 * spirit of ambulkdelete's DEBUG1 breakdown.
	 */
	elog(DEBUG1, "lion index \"%s\": %u blocks at the start and %u at the end; %d candidates settled after %d waits; %lld left links and %lld root flags settled on the spot; %lld posting-set walks, %lld thrown away, %lld with the leaf held, %lld kept uncompared",
		 RelationGetRelationName(vs.index), vs.startblocks, vs.nblocks,
		 vs.ncands, vs.nwaits, (long long) vs.nleftlinks,
		 (long long) vs.nrootsplits, (long long) vs.nsetwalks,
		 (long long) vs.nsetretries, (long long) vs.nsetholds,
		 (long long) vs.nsetuncompared);

	if (heapallindexed)
		lion_verify_heapallindexed(&vs);

	MemoryContextDelete(vs.setcxt);
	pfree(vs.setvisits);
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
