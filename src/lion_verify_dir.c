/*-------------------------------------------------------------------------
 *
 * lion_verify_dir.c
 *		lion_index_verify(): the directory (DESIGN.md §21).
 *
 * Part of the SQL-callable helpers of the lion index; lion_funcs.h
 * describes them and declares what their files share.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "lion_funcs.h"

/* ---------------------------------------------------------------------
 * The directory (DESIGN.md §21)
 *
 * The tree is checked level by level from the leaves up.  Each level is
 * walked along its right links, which proves the sibling links and the key
 * order within and across its pages; the level above is then checked against
 * what the walk of the level below collected, which proves that its downlinks
 * name exactly those pages, in that order, with separators that bound them.
 * Every page is passed through lion_verify_visit(), so a page reachable twice
 * - or not at all - is reported by that and by lion_verify_reachable().
 * --------------------------------------------------------------------- */

typedef struct LionVerifyLevel
{
	int			npages;
	int			maxpages;
	BlockNumber *blocks;
	LionEntryTuple **firstkey;	/* first data item of each page, or NULL */
	LionEntryTuple **highkey;	/* its high key, or NULL when rightmost */
	bool	   *incomplete;		/* flagged LION_PAGE_INCOMPLETE_SPLIT */
} LionVerifyLevel;

static void
lion_verify_level_add(LionVerifyLevel *lvl, BlockNumber blk,
					 LionEntryTuple *firstkey, LionEntryTuple *highkey,
					 bool incomplete)
{
	if (lvl->npages >= lvl->maxpages)
	{
		bool		first = (lvl->maxpages == 0);

		lvl->maxpages = first ? 64 : lvl->maxpages * 2;
		if (first)
		{
			lvl->blocks = (BlockNumber *) palloc(sizeof(BlockNumber) * lvl->maxpages);
			lvl->firstkey = (LionEntryTuple **) palloc(sizeof(LionEntryTuple *) * lvl->maxpages);
			lvl->highkey = (LionEntryTuple **) palloc(sizeof(LionEntryTuple *) * lvl->maxpages);
			lvl->incomplete = (bool *) palloc(sizeof(bool) * lvl->maxpages);
		}
		else
		{
			lvl->blocks = (BlockNumber *) repalloc(lvl->blocks,
												   sizeof(BlockNumber) * lvl->maxpages);
			lvl->firstkey = (LionEntryTuple **) repalloc(lvl->firstkey,
														 sizeof(LionEntryTuple *) * lvl->maxpages);
			lvl->highkey = (LionEntryTuple **) repalloc(lvl->highkey,
														sizeof(LionEntryTuple *) * lvl->maxpages);
			lvl->incomplete = (bool *) repalloc(lvl->incomplete,
												sizeof(bool) * lvl->maxpages);
		}
	}
	lvl->blocks[lvl->npages] = blk;
	lvl->firstkey[lvl->npages] = firstkey;
	lvl->highkey[lvl->npages] = highkey;
	lvl->incomplete[lvl->npages] = incomplete;
	lvl->npages++;
}

LionEntryTuple *
lion_verify_copy_item(Page page, OffsetNumber off)
{
	Size		sz = ItemIdGetLength(PageGetItemId(page, off));
	LionEntryTuple *c = (LionEntryTuple *) palloc(sz);

	memcpy(c, PageGetItem(page, PageGetItemId(page, off)), sz);
	return c;
}

/*
 * One equality class, one entry (DESIGN.md §21).  The entries whose prefixes
 * tie - (column, kind, proc 4, hash) - are the candidates for being the same
 * key, and they are contiguous in the leaf order, across page boundaries too,
 * so the check keeps the current prefix run and compares each new entry with
 * every member of it under the opclass equality.  Byte-identical twins are
 * caught by the ordering checks already; this also catches two spellings of
 * one class (the citext shape), which sort apart inside their run.  Runs are
 * a single entry unless the hash collides, so this is linear in practice.
 */
typedef struct LionVerifyRun
{
	int			n;
	int			max;
	LionEntryTuple **items;		/* copies */
	BlockNumber *blocks;
} LionVerifyRun;

static void
lion_verify_run_add(LionVerifyState *vs, LionVerifyRun *run,
					LionEntryTuple *item, BlockNumber blk, OffsetNumber off)
{
	int			i;

	if (run->n > 0)
	{
		LionSearchKey sk;

		lion_search_key_exact(vs->ix, &sk, run->items[0]);
		if (lion_cmp_prefix(item, &sk) != 0)
		{
			for (i = 0; i < run->n; i++)
				pfree(run->items[i]);
			run->n = 0;
		}
	}

	for (i = 0; i < run->n; i++)
	{
		LionEntryTuple *other = run->items[i];
		bool		same;

		if (lion_entry_kind(item) != LION_KIND_VALUE)
			same = true;		/* a second reserved entry of its kind */
		else
		{
			LionState  *col = lion_column(vs->ix, (AttrNumber) item->attno);

			same = lion_keys_equal(col,
								   lion_entry_key(col, other),
								   lion_entry_key(col, item));
		}
		if (same)
			lion_corrupt("lion index \"%s\": entry %u on block %u is a second entry for the key of an entry on block %u",
						RelationGetRelationName(vs->index), off, blk,
						run->blocks[i]);
	}

	if (run->n >= run->max)
	{
		run->max = (run->max == 0) ? 8 : run->max * 2;
		if (run->items == NULL)
		{
			run->items = (LionEntryTuple **) palloc(sizeof(LionEntryTuple *) * run->max);
			run->blocks = (BlockNumber *) palloc(sizeof(BlockNumber) * run->max);
		}
		else
		{
			run->items = (LionEntryTuple **) repalloc(run->items,
													  sizeof(LionEntryTuple *) * run->max);
			run->blocks = (BlockNumber *) repalloc(run->blocks,
												   sizeof(BlockNumber) * run->max);
		}
	}
	{
		Size		sz = MAXALIGN(LION_ENTRY_HDRSZ + item->keylen);
		LionEntryTuple *c = (LionEntryTuple *) palloc(sz);

		memcpy(c, item, LION_ENTRY_HDRSZ + item->keylen);
		run->items[run->n] = c;
		run->blocks[run->n] = blk;
		run->n++;
	}
}

/*
 * A directory page's left link is not the page the walk came from.
 *
 * On a sound index that is a split of the page the walk came from, made after
 * the walk read it: the split links its new page between the two in the same
 * record that changes this page's left link (DESIGN.md §21), and a walk that
 * follows the right link it read never visits the new page.  So the level is
 * walked again from `prev` to blk with each page held until its right sibling
 * is locked - left to right, the order every directory writer locks one level
 * in, so it cannot deadlock - and with both of two neighbours locked, the
 * right one's left link naming the left one is exact.  The pages found in
 * between are not visited here: nothing reached them, and the reachability
 * pass settles them as it settles every page a split made behind the walk.
 */
static void
lion_verify_leftlink(LionVerifyState *vs, BlockNumber blk, uint16 level,
					 uint16 kind, BlockNumber prev, BlockNumber leftlink)
{
	Buffer		buf;
	Page		page;
	BlockNumber cur = prev;
	BlockNumber steps = 0;

	if (!vs->concurrent || !BlockNumberIsValid(prev))
		lion_corrupt("lion index \"%s\": directory page %u has left link %u, expected %u",
					RelationGetRelationName(vs->index), blk, leftlink, prev);

	vs->nleftlinks++;
	page = lion_verify_read_page(vs, cur, kind, &buf);
	for (;;)
	{
		BlockNumber next = LionPageGetOpaque(page)->rightlink;
		Buffer		nbuf;
		Page		npage;

		if (!BlockNumberIsValid(next) || ++steps > vs->nblocks)
			lion_corrupt("lion index \"%s\": directory page %u has left link %u, expected %u, and the right links from block %u do not lead to it",
						RelationGetRelationName(vs->index), blk, leftlink, prev,
						prev);

		npage = lion_verify_read_page(vs, next, kind, &nbuf);
		if (LionPageGetOpaque(npage)->level != level)
			lion_corrupt("lion index \"%s\": directory page %u is at level %u, expected %u",
						RelationGetRelationName(vs->index), next,
						LionPageGetOpaque(npage)->level, level);
		if (LionPageGetOpaque(npage)->leftlink != cur)
			lion_corrupt("lion index \"%s\": directory page %u has left link %u, expected %u",
						RelationGetRelationName(vs->index), next,
						LionPageGetOpaque(npage)->leftlink, cur);
		UnlockReleaseBuffer(buf);
		buf = nbuf;
		page = npage;
		cur = next;
		if (cur == blk)
			break;
		CHECK_FOR_INTERRUPTS();
	}
	UnlockReleaseBuffer(buf);
}

/*
 * A page at the height the meta page gave has no root flag.
 *
 * On a sound index that is a root split made since the meta page was read:
 * it takes the flag off the old root, which stays at its block and level as
 * the left half, and names a new root one level up in the meta page, in one
 * record (DESIGN.md §21).  So a meta page that now says the directory is
 * taller settles it at once.  The new root and the old root's new sibling are
 * pages the walk may not reach; the reachability pass settles them.
 */
static void
lion_verify_root_flag(LionVerifyState *vs, BlockNumber blk, uint16 level,
					  bool isroot)
{
	if (isroot == (level == vs->height))
		return;

	if (!isroot && vs->concurrent)
	{
		Buffer		buf;
		Page		page;

		/* once the meta page has said so, it says so for every such page */
		if (!vs->rootsplit)
		{
			page = lion_verify_read_page(vs, LION_METAPAGE_BLKNO, LION_PAGE_META,
										 &buf);
			vs->rootsplit = LionPageGetMeta(page)->height > vs->height;
			UnlockReleaseBuffer(buf);
			if (vs->rootsplit)
				vs->nrootsplits++;
		}
		if (vs->rootsplit)
			return;
	}

	lion_corrupt("lion index \"%s\": directory page %u at level %u %s the root flag",
				RelationGetRelationName(vs->index), blk, level,
				isroot ? "should not have" : "should have");
}

/*
 * Read blk under a SHARE lock, check its header and kind, and copy it into
 * dest, for a caller that goes on to check the copy with nothing locked.
 */
static void
lion_verify_copy_page(LionVerifyState *vs, BlockNumber blk, uint16 kind,
					  Page dest)
{
	Buffer		buf;
	Page		page;

	page = lion_verify_read_page(vs, blk, kind, &buf);
	memcpy(dest, page, BLCKSZ);
	UnlockReleaseBuffer(buf);
}

/*
 * Walk one level of the directory along its right links.  For an internal
 * level the downlinks and their separators are collected into *children.
 *
 * Each page is COPIED under its SHARE lock and checked from the copy, so no
 * lock is held while the opclass's functions order and hash the keys, nor -
 * on a leaf - while the posting sets of its CHAIN entries are walked
 * (lion_verify_set()).  A page is consistent in itself at every moment a
 * reader can lock it, whatever writers do, so every check of one page is
 * exact.  So are the checks across two neighbours that read the left one's
 * high key and the right one's first key: a page's lower bound is the high
 * key its left neighbour had when the walk read it, a split of the left page
 * after that only lowers the left page's own high key, and a split never
 * leaves a page without its first item.  What a concurrent split can change
 * is the right page's left link (lion_verify_leftlink()) and which page is
 * the root (lion_verify_root_flag()).
 */
static void
lion_verify_walk_level(LionVerifyState *vs, BlockNumber first, uint16 level,
					  bool isleaf, LionVerifyLevel *out,
					  LionVerifyLevel *children)
{
	BlockNumber blk = first;
	BlockNumber prev = InvalidBlockNumber;
	LionEntryTuple *prevhigh = NULL;
	LionVerifyRun run;
	uint16		kind = isleaf ? LION_PAGE_BUCKET : LION_PAGE_DIR;
	Page		page = (Page) palloc(BLCKSZ);

	memset(&run, 0, sizeof(run));

	while (BlockNumberIsValid(blk))
	{
		LionPageOpaque opaque;
		OffsetNumber maxoff;
		OffsetNumber off;
		OffsetNumber firstdata;
		LionEntryTuple *prevkey = NULL;
		LionEntryTuple *firstkey = NULL;
		LionEntryTuple *highkey = NULL;

		lion_verify_visit(vs, blk, "the directory");
		lion_verify_copy_page(vs, blk, kind, page);
		opaque = LionPageGetOpaque(page);

		/*
		 * Test hook: this page is copied and its right link read, and nothing
		 * is held.  test/isolation/verify_concurrent.spec parks on the first
		 * leaf and splits it, so that the next page's left link names the
		 * split's new page instead of this one.
		 */
		LION_INJECTION_POINT("lion-verify-dir-page-read");

		if (opaque->level != level)
			lion_corrupt("lion index \"%s\": directory page %u is at level %u, expected %u",
						RelationGetRelationName(vs->index), blk, opaque->level,
						level);
		if (opaque->leftlink != prev)
			lion_verify_leftlink(vs, blk, level, kind, prev, opaque->leftlink);
		lion_verify_root_flag(vs, blk, level, LionPageIsRoot(page));

		/*
		 * Exact: a split holds the page it flags EXCLUSIVE until it clears the
		 * flag (DESIGN.md §21), so a flag a SHARE lock lets the walk see is one
		 * a crash or an error left behind.
		 */
		if (LionPageIncompleteSplit(page))
			ereport(WARNING,
					(errmsg("lion index \"%s\": directory page %u has an unfinished split",
							RelationGetRelationName(vs->index), blk),
					 errdetail("Its right sibling has no downlink in the parent yet."),
					 errhint("The next INSERT that descends to this page repairs it.")));

		maxoff = PageGetMaxOffsetNumber(page);
		firstdata = lion_page_first_data(page);

		/* Nothing below reads an item this has not vouched for. */
		for (off = FirstOffsetNumber; off <= maxoff; off++)
			(void) lion_verify_dir_item(vs, blk, page, off);

		/*
		 * ... and no two of them overlap, which every writer of the page
		 * requires before it rewrites one (lion_page_check_items()).
		 */
		lion_page_check_items(vs->index, page, blk);

		if (!LionPageIsRightmost(page))
		{
			if (maxoff < FirstOffsetNumber)
				lion_corrupt("lion index \"%s\": directory page %u is not rightmost but has no high key",
							RelationGetRelationName(vs->index), blk);
			if (!ItemIdIsUsed(PageGetItemId(page, FirstOffsetNumber)) ||
				!LionEntryIsHighKey(lion_page_entry(page, FirstOffsetNumber)))
				lion_corrupt("lion index \"%s\": the first item of directory page %u is not a high key",
							RelationGetRelationName(vs->index), blk);
			highkey = lion_verify_copy_item(page, FirstOffsetNumber);
		}
		else if (maxoff >= FirstOffsetNumber &&
				 ItemIdIsUsed(PageGetItemId(page, FirstOffsetNumber)) &&
				 LionEntryIsHighKey(lion_page_entry(page, FirstOffsetNumber)))
			lion_corrupt("lion index \"%s\": rightmost directory page %u carries a high key",
						RelationGetRelationName(vs->index), blk);

		for (off = firstdata; off <= maxoff; off++)
		{
			ItemId		iid = PageGetItemId(page, off);
			LionEntryTuple *item;

			if (!ItemIdIsUsed(iid))
				continue;
			item = (LionEntryTuple *) PageGetItem(page, iid);

			if (LionEntryIsHighKey(item))
				lion_corrupt("lion index \"%s\": item %u of directory page %u is a second high key",
							RelationGetRelationName(vs->index), off, blk);
			if (isleaf == LionEntryIsDownlink(item))
				lion_corrupt("lion index \"%s\": item %u of directory page %u is %sa downlink",
							RelationGetRelationName(vs->index), off, blk,
							isleaf ? "" : "not ");

			/* Strictly increasing within the page (DESIGN.md §21). */
			if (prevkey != NULL &&
				lion_cmp_entries(vs->ix, prevkey, item) >= 0)
				lion_corrupt_reindex("lion index \"%s\": item %u of directory page %u does not sort after the one before it",
									RelationGetRelationName(vs->index), off, blk);

			if (firstkey == NULL)
			{
				firstkey = lion_verify_copy_item(page, off);
				if (prevhigh != NULL &&
					lion_cmp_entries(vs->ix, prevhigh, firstkey) > 0)
					lion_corrupt_reindex("lion index \"%s\": the high key of the page left of %u sorts after its first key",
										RelationGetRelationName(vs->index), blk);
			}
			if (prevkey != NULL)
				pfree(prevkey);
			prevkey = lion_verify_copy_item(page, off);

			if (isleaf)
			{
				lion_verify_entry(vs, blk, off, iid, page);
				lion_verify_run_add(vs, &run, item, blk, off);
			}
			else
			{
				if (children != NULL)
					lion_verify_level_add(children, item->head,
										  lion_verify_copy_item(page, off),
										  NULL, false);
			}

			CHECK_FOR_INTERRUPTS();
		}

		if (highkey != NULL && prevkey != NULL &&
			lion_cmp_entries(vs->ix, prevkey, highkey) >= 0)
			lion_corrupt_reindex("lion index \"%s\": the last key of directory page %u is not below its high key",
								RelationGetRelationName(vs->index), blk);
		if (prevkey != NULL)
			pfree(prevkey);

		lion_verify_level_add(out, blk, firstkey, highkey,
							  LionPageIncompleteSplit(page));

		prev = blk;
		prevhigh = highkey;		/* owned by *out; not freed here */
		blk = opaque->rightlink;

		CHECK_FOR_INTERRUPTS();
	}

	pfree(page);
}

/*
 * Split the downlinks the walk of level `level` collected into the ones that
 * name pages the walk of the level below reached - copied into *seen, in
 * order, for the comparison - and the ones that do not.
 *
 * The level below is walked FIRST, so a split made between the two walks
 * shows up here and nowhere else: its new page went into the level's right
 * links behind the child walk, and its downlink into this level before this
 * walk got there.  (A split made before the child walk reached the place is
 * simply walked: the child walk reads the flagged page only after the split
 * has finished, because the split holds it EXCLUSIVE until then.)  Such a
 * downlink is a candidate - the page has to be in the level below, between
 * the reached page whose downlink precedes it and the one whose downlink
 * follows it - and lion_verify_recheck_downlink() walks there once the
 * writers in flight are done.
 *
 * Three kinds of downlink to an unreached page are corruption whatever
 * writers do, and are reported at once: the level's FIRST downlink, which
 * names the level's leftmost page, and no split ever moves that; one to a
 * page something else reached - a posting page, or a directory page of
 * another level - since no page is freed or changes level while the check
 * runs; and a second downlink to the same page.
 */
static void
lion_verify_filter_downlinks(LionVerifyState *vs, uint32 level,
							 const LionVerifyLevel *below,
							 const LionVerifyLevel *children,
							 LionVerifyLevel *seen)
{
	BlockNumber *sorted = lion_verify_sorted_blocks(below->blocks,
													below->npages);
	BlockNumber lastseen = InvalidBlockNumber;
	int			firstcand = vs->ncands;
	int			pending = vs->ncands;
	int			j;

	for (j = 0; j < children->npages; j++)
	{
		BlockNumber b = children->blocks[j];
		LionVerifyCand cand;

		if (lion_verify_has_block(sorted, below->npages, b))
		{
			lion_verify_level_add(seen, b, children->firstkey[j], NULL, false);
			/* the candidates since the last reached page lie before this one */
			for (; pending < vs->ncands; pending++)
				vs->cands[pending].right = b;
			lastseen = b;
			continue;
		}

		if (j == 0)
			lion_corrupt("lion index \"%s\": downlink %d of level %u points at block %u, but the next page of level %u is block %u",
						RelationGetRelationName(vs->index), j, level, b,
						level - 1, below->blocks[0]);
		if (b < vs->nblocks && vs->refs[b] != 0)
			lion_corrupt("lion index \"%s\": downlink %d of level %u points at block %u, which is not a page of level %u",
						RelationGetRelationName(vs->index), j, level, b,
						level - 1);
		if (!vs->concurrent)
			lion_corrupt("lion index \"%s\": downlink %d of level %u points at block %u, which the walk of level %u did not reach",
						RelationGetRelationName(vs->index), j, level, b,
						level - 1);

		memset(&cand, 0, sizeof(cand));
		cand.kind = LION_VCAND_DOWNLINK;
		cand.blk = b;
		cand.level = (uint16) (level - 1);
		cand.left = lastseen;
		cand.right = InvalidBlockNumber;
		cand.downlink = j;
		lion_verify_add_cand(vs, &cand);
	}

	/* two downlinks to one unreached page */
	if (vs->ncands - firstcand > 1)
	{
		int			n = vs->ncands - firstcand;
		BlockNumber *dl = (BlockNumber *) palloc(sizeof(BlockNumber) * n);
		int			k;

		for (k = 0; k < n; k++)
			dl[k] = vs->cands[firstcand + k].blk;
		qsort(dl, n, sizeof(BlockNumber), lion_verify_blkcmp);
		for (k = 1; k < n; k++)
		{
			if (dl[k] == dl[k - 1])
				lion_corrupt("lion index \"%s\": block %u is referenced more than once (reached again as a downlink of level %u)",
							RelationGetRelationName(vs->index), dl[k], level);
		}
		pfree(dl);
	}

	pfree(sorted);
}

/*
 * Check the whole directory: every level, and every level against the one
 * below it.  The levels are walked from the leaves up, which is what lets a
 * split made between two walks show up as nothing worse than a downlink to a
 * page the lower walk did not reach (lion_verify_filter_downlinks()).
 */
void
lion_verify_directory(LionVerifyState *vs)
{
	BlockNumber *leftmost;
	LionVerifyLevel *lvl;
	uint32		h = vs->height;
	uint32		i;
	int			j;

	/* The leftmost page of every level, from the root down. */
	leftmost = (BlockNumber *) palloc(sizeof(BlockNumber) * (h + 1));
	{
		BlockNumber blk = vs->root;

		for (i = 0; i <= h; i++)
		{
			Buffer		buf;
			Page		page;
			uint16		level = (uint16) (h - i);

			page = lion_verify_read_page(vs, blk,
										 level == 0 ? LION_PAGE_BUCKET :
										 LION_PAGE_DIR, &buf);
			if (LionPageGetOpaque(page)->level != level)
				lion_corrupt("lion index \"%s\": the leftmost page %u is at level %u, expected %u",
							RelationGetRelationName(vs->index), blk,
							LionPageGetOpaque(page)->level, level);
			leftmost[level] = blk;
			if (level > 0)
			{
				LionEntryTuple *down = NULL;

				if (PageGetMaxOffsetNumber(page) >= lion_page_first_data(page))
					down = lion_verify_dir_item(vs, blk, page,
												lion_page_first_data(page));
				if (down == NULL)
					lion_corrupt("lion index \"%s\": internal page %u has no downlink",
								RelationGetRelationName(vs->index), blk);
				if (!LionEntryIsMinusInf(down))
					lion_corrupt("lion index \"%s\": the leftmost downlink of page %u is not minus infinity",
								RelationGetRelationName(vs->index), blk);
				/* checked against the index's length when it is read */
				blk = down->head;
			}
			UnlockReleaseBuffer(buf);
		}
	}

	lvl = (LionVerifyLevel *) palloc0(sizeof(LionVerifyLevel) * (h + 1));

	for (i = 0; i <= h; i++)
	{
		LionVerifyLevel walked;
		LionVerifyLevel children;

		memset(&walked, 0, sizeof(walked));
		memset(&children, 0, sizeof(children));
		lion_verify_walk_level(vs, leftmost[i], (uint16) i, i == 0, &lvl[i],
							   i == 0 ? NULL : &walked);

		if (i > 0)
		{
			const LionVerifyLevel *below = &lvl[i - 1];
			int			p;

			lion_verify_filter_downlinks(vs, i, below, &walked, &children);

			/*
			 * Match the downlinks with the pages of the level below, each in
			 * the order its own walk found them; j is the next downlink to
			 * match.  A page may lack a downlink when its left neighbour is
			 * flagged LION_PAGE_INCOMPLETE_SPLIT: the right half of a split
			 * whose downlink record a crash or an error cut off, which the
			 * next writer that descends to the left page finishes (DESIGN.md
			 * §21) and which the walk has already warned about.  The flag
			 * with the downlink already in place is normal too - the flag is
			 * cleared by a record of its own - and needs nothing here.  The
			 * rules are the posting tree's, in lion_verify_chain().
			 */
			j = 0;
			for (p = 0; p < below->npages; p++)
			{
				if (j < children.npages && children.blocks[j] == below->blocks[p])
				{
					LionEntryTuple *sep = children.firstkey[j];

					if (j == 0)
					{
						if (!LionEntryIsMinusInf(sep))
							lion_corrupt("lion index \"%s\": the first downlink of level %u is not minus infinity",
										RelationGetRelationName(vs->index), i);
					}
					else if (LionEntryIsMinusInf(sep))
						lion_corrupt("lion index \"%s\": downlink %d of level %u is minus infinity",
									RelationGetRelationName(vs->index), j, i);
					else if (below->firstkey[p] != NULL &&
							 lion_cmp_entries(vs->ix, sep,
											  below->firstkey[p]) > 0)
						lion_corrupt("lion index \"%s\": the separator of block %u sorts after its own first key",
									RelationGetRelationName(vs->index),
									below->blocks[p]);
					j++;
				}
				else if (p > 0 && below->incomplete[p - 1])
				{
					/* the right half of an unfinished split: no downlink yet */
				}
				else if (j < children.npages)
					lion_corrupt("lion index \"%s\": downlink %d of level %u points at block %u, but the next page of level %u is block %u",
								RelationGetRelationName(vs->index), j, i,
								children.blocks[j], i - 1, below->blocks[p]);
				else
					lion_corrupt("lion index \"%s\": level %u has %d downlinks but level %u has %d pages",
								RelationGetRelationName(vs->index), i,
								children.npages, i - 1, below->npages);

				/*
				 * A child's high key was the next separator when the split
				 * made them; later splits of the child only lower it.
				 */
				if (j < children.npages &&
					below->highkey[p] != NULL &&
					lion_cmp_entries(vs->ix, below->highkey[p],
									 children.firstkey[j]) > 0)
					lion_corrupt_reindex("lion index \"%s\": the high key of block %u sorts after the separator of its right sibling",
										RelationGetRelationName(vs->index),
										below->blocks[p]);
			}

			if (j < children.npages)
				lion_corrupt("lion index \"%s\": level %u has %d downlinks but level %u has %d pages",
							RelationGetRelationName(vs->index), i,
							children.npages, i - 1, below->npages);
		}

		/*
		 * Test hook: level i is walked (and compared with level i - 1), the
		 * levels above it are not, and nothing is held.
		 * test/isolation/verify_concurrent.spec parks here after the leaves
		 * and has INSERTs split them, spill an entry and reuse a free page.
		 */
		LION_INJECTION_POINT("lion-verify-dir-level-walked");
	}

	pfree(leftmost);
}

/*
 * Check the meta page.
 */
void
lion_verify_meta(LionVerifyState *vs)
{
	Buffer		buf;
	Page		page;
	LionMetaPageData *meta;

	lion_verify_visit(vs, LION_METAPAGE_BLKNO, "the meta page");
	page = lion_verify_read_page(vs, LION_METAPAGE_BLKNO, LION_PAGE_META, &buf);
	meta = LionPageGetMeta(page);

	if (meta->magic != LION_MAGIC ||
		(meta->version != LION_VERSION &&
		 meta->version != LION_VERSION_SUMMARIES))
		lion_corrupt("lion index \"%s\": meta page has magic %08X version %u, expected %08X version %u or %u",
					RelationGetRelationName(vs->index), meta->magic,
					meta->version, LION_MAGIC, LION_VERSION,
					LION_VERSION_SUMMARIES);

	/* DESIGN.md §32: version 7 is version 6 with summaries, and only that. */
	if ((meta->version == LION_VERSION) != (meta->summary_cols == 0))
		lion_corrupt("lion index \"%s\": meta page version %u names summarized key columns %08X",
					RelationGetRelationName(vs->index), meta->version,
					meta->summary_cols);
	if (meta->summary_cols != 0 &&
		(meta->summary_tids < LION_MIN_SUMMARY_TIDS ||
		 meta->summary_tids > LION_MAX_SUMMARY_TIDS))
		lion_corrupt("lion index \"%s\": meta page has summary buckets of %u TIDs, expected %d .. %d",
					RelationGetRelationName(vs->index), meta->summary_tids,
					LION_MIN_SUMMARY_TIDS, LION_MAX_SUMMARY_TIDS);
	if ((meta->summary_cols >> vs->ix->ncolumns) != 0 &&
		vs->ix->ncolumns < 32)
		lion_corrupt("lion index \"%s\": meta page names summarized key columns %08X of %d",
					RelationGetRelationName(vs->index), meta->summary_cols,
					vs->ix->ncolumns);

	if (meta->offset_bits != LION_OFFSET_BITS ||
		meta->container_bits != LION_CONTAINER_BITS)
		lion_corrupt("lion index \"%s\": meta page has offset_bits %u and container_bits %u, expected %d and %d",
					RelationGetRelationName(vs->index), meta->offset_bits,
					meta->container_bits, LION_OFFSET_BITS, LION_CONTAINER_BITS);

	if (meta->inline_limit < LION_MIN_INLINE_LIMIT ||
		meta->inline_limit > LION_MAX_INLINE_LIMIT)
		lion_corrupt("lion index \"%s\": meta page has inline_limit %u, expected %d .. %d",
					RelationGetRelationName(vs->index), meta->inline_limit,
					LION_MIN_INLINE_LIMIT, LION_MAX_INLINE_LIMIT);

	/*
	 * A root split since the block count was taken can name a root past it
	 * (lion_verify_block_exists() reads the count again), and makes the
	 * directory taller, which the bound below reads the count again for.
	 */
	if (!lion_verify_block_exists(vs, meta->root))
		lion_corrupt("lion index \"%s\": meta page names root block %u, but the index has %u blocks",
					RelationGetRelationName(vs->index), meta->root, vs->nblocks);

	/*
	 * A directory of height h has at least h + 1 pages besides this one, and
	 * the walk sizes its arrays by the height: bound it before it is used.
	 */
	if ((uint64) meta->height + 2 > (uint64) vs->nblocks)
		lion_verify_refresh_nblocks(vs);
	if ((uint64) meta->height + 2 > (uint64) vs->nblocks)
		lion_corrupt("lion index \"%s\": meta page has directory height %u, but the index has only %u blocks",
					RelationGetRelationName(vs->index), meta->height,
					vs->nblocks);

	if (LionPageGetOpaque(page)->rightlink != InvalidBlockNumber)
		lion_corrupt("lion index \"%s\": the meta page has a right link to block %u",
					RelationGetRelationName(vs->index),
					LionPageGetOpaque(page)->rightlink);

	vs->root = meta->root;
	vs->height = meta->height;

	UnlockReleaseBuffer(buf);
}


/*
 * Classify blk, which no walk reached.
 *
 * Some unreferenced blocks are tolerated (with a warning), because a crash or
 * an error can leave them behind and none of them makes the index wrong: a
 * block that was never initialised (the relation was extended and the
 * transaction did not get as far as its WAL record), an empty container page
 * (a crash between the two steps of a whole-set free), a full leaf whose root
 * was never written (a multi-leaf spill that did not reach its last record) -
 * the kinds the leak sweep of the next VACUUM frees - and an INTERNAL posting
 * page, which a posting set freed leaves-first leaves behind with its
 * downlinks still on it (DESIGN.md §22).
 *
 * None of that is changed by writers running beside the check: a page a
 * writer is still initialising is one it holds EXCLUSIVE from the moment it
 * takes it - out of the free space map, or from ExtendBufferedRel(), which
 * locks the new block before anyone can read it - to the record that
 * initialises and links it, so the SHARE lock below waits for that record,
 * and a page seen all-zero or DELETED under it is one no record came for.  A
 * new page is never empty, and every page a split or a push-down makes is
 * stamped with a root that is live.  Only the internal and the live kinds can
 * be pages a writer made behind the walk, and those are the candidates.
 */
LionVerifyUnref
lion_verify_classify(LionVerifyState *vs, BlockNumber blk)
{
	Buffer		buf;
	Page		page;
	BlockNumber ohead;
	uint32		ohash;

	buf = ReadBuffer(vs->index, blk);
	LockBuffer(buf, BUFFER_LOCK_SHARE);
	page = BufferGetPage(buf);

	if (PageIsNew(page))
	{
		UnlockReleaseBuffer(buf);
		return LION_UNREF_LEAK;
	}
	if (PageGetSpecialSize(page) != LION_SPECIAL_SIZE ||
		LionPageGetOpaque(page)->page_id != LION_PAGE_ID)
	{
		UnlockReleaseBuffer(buf);
		return LION_UNREF_FOREIGN;
	}

	/*
	 * A DELETED page is the normal state of a freed one (DESIGN.md §18): it
	 * is unreferenced on purpose, it is in the free space map, and the next
	 * allocation whose safexid test it passes takes it.  Nothing to report.
	 */
	if (LionPageIsDeleted(page))
	{
		UnlockReleaseBuffer(buf);
		return LION_UNREF_FREE;
	}

	if (!LionPageIsContainer(page))
	{
		UnlockReleaseBuffer(buf);
		return LION_UNREF_LIVE;
	}
	if (PageGetMaxOffsetNumber(page) == 0)
	{
		UnlockReleaseBuffer(buf);
		return LION_UNREF_LEAK;
	}
	if (LionPageIsPostingInternal(page))
	{
		UnlockReleaseBuffer(buf);
		return LION_UNREF_INTERNAL;
	}

	/*
	 * A FULL leaf is leaked when its root is not a live root of its key: a
	 * multi-leaf spill that an ERROR or a crash stopped before its last record
	 * writes the leaves and never the root (lion_entry_spill(), DESIGN.md
	 * §18).  The root is looked at with nothing held.  A leaf that is its own
	 * root is a one-page set, which a spill writes in the same record as its
	 * entry.
	 */
	ohead = LionPageGetOpaque(page)->owner_head;
	ohash = LionPageGetOpaque(page)->owner_hash;
	UnlockReleaseBuffer(buf);

	if (ohead != blk && !lion_posting_root_live(vs->index, ohash, ohead, true))
		return LION_UNREF_LEAK;
	return LION_UNREF_LIVE;
}

void
lion_verify_warn_leak(LionVerifyState *vs, BlockNumber blk)
{
	ereport(WARNING,
			(errmsg("lion index \"%s\": block %u is unused and unreachable",
					RelationGetRelationName(vs->index), blk),
			 errdetail("An interrupted page allocation leaks blocks; the next VACUUM turns them into free pages.")));
}


/*
 * Every block has to belong to the meta page, the directory or exactly one
 * key's posting set (lion_verify_classify() says which unreferenced ones are
 * tolerated).
 *
 * The pass covers the blocks the index had when the check began: a block the
 * relation grew by since then holds nothing that was in the index when it
 * began, and a link into one that the walk followed has been checked.  A
 * live page no walk reached may be a page a writer made - a split's new
 * sibling, a push-down's child, a spilled set's root, from the end of the
 * relation or out of the free space map - after the walk had passed the place
 * it went.  On a primary that is a candidate for lion_verify_recheck(); on a
 * standby it is reported, as it always was.
 */
void
lion_verify_reachable(LionVerifyState *vs)
{
	BlockNumber blk;

	for (blk = 0; blk < vs->startblocks; blk++)
	{
		LionVerifyCand cand;

		if (vs->refs[blk] != 0)
			continue;

		switch (lion_verify_classify(vs, blk))
		{
			case LION_UNREF_FREE:
				continue;
			case LION_UNREF_LEAK:
				lion_verify_warn_leak(vs, blk);
				continue;
			case LION_UNREF_FOREIGN:
				lion_verify_unreachable(vs, blk);
				break;
			case LION_UNREF_INTERNAL:
				if (!vs->concurrent)
				{
					lion_verify_warn_leak(vs, blk);
					continue;
				}
				break;
			case LION_UNREF_LIVE:
				if (!vs->concurrent)
					lion_verify_unreachable(vs, blk);
				break;
		}

		memset(&cand, 0, sizeof(cand));
		cand.kind = LION_VCAND_UNREACHED;
		cand.blk = blk;
		cand.left = cand.right = InvalidBlockNumber;
		lion_verify_add_cand(vs, &cand);

		CHECK_FOR_INTERRUPTS();
	}
}
