/*-------------------------------------------------------------------------
 *
 * lion_postree.c
 *		Position trees: where a key's position chunks (lion_positions.h) live
 *		once they outgrow its entry, and how members are put and found.
 *
 * A position tree is a posting tree (DESIGN.md §22) whose pages carry
 * LION_PAGE_POSITIONS and the tree's own root as their owner stamp, and whose
 * leaf items are position chunks.  Descent, root push-down, splits and their
 * repair are the posting tree's own code (lion_posting.c, lion_posting_put.c),
 * reached through a LionTreeRef that has no entry.  What is particular to
 * positions is here: where a member goes, and where a reader looks for one.
 *
 * ROUTING.  The keys of a position tree - chunk headers, separators, a page's
 * min and max - are heap BLOCKS, not container keys (lion_positions.h).  A
 * chunk can hold the members of several blocks and the members of one block
 * can fill several chunks, so the separators - each the header block of the
 * first chunk a split put on a page - repeat.  Descending "to the last
 * separator at or below X" would then skip every leaf but the last of a run
 * of equal separators.  Both readers and writers of block X therefore descend
 * to the last separator strictly BELOW X (lion_posting_search_before()) and
 * walk right from there.  That works because of three invariants:
 *
 *	(P1) the members of the tree ascend strictly, chunk after chunk, leaf
 *		 after leaf;
 *	(P2) a chunk's header block is at most the block of its first member, and
 *		 its members' blocks are at most the header of the chunk after it -
 *		 so a member of block X is in the last chunk whose header is below X,
 *		 or in a chunk whose header is X, and nowhere else;
 *	(P3) the first chunk of every leaf but the leftmost has the leaf's
 *		 separator as its header - so the last chunk whose header is below X
 *		 is on the leaf the descent lands on, and never to its left.
 *
 * lion_postree_put() keeps them: a member goes into the last of those
 * candidate chunks whose first member is at or below it, or into the first
 * candidate when none is; a header only ever drops, and only on the tree's
 * very first chunk (lion_poschunk_insert()); a chunk that overflows splits
 * in two, the left half keeping its header.  A page split keeps (P3) because
 * the separator it posts is the header of the first chunk it moved.  Whoever
 * removes members (VACUUM) keeps (P3) by leaving a leaf's first chunk in
 * place, empty if need be, for as long as the leaf lives.
 *
 * LOCKING is the posting tree's: writers of one key serialise on the key's
 * directory leaf, so no split is in flight while a writer walks; readers
 * take one page at a time, SHARE, and a split only ever moves chunks right.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/relation.h"
#include "fmgr.h"
#include "miscadmin.h"
#include "storage/bufmgr.h"
#include "storage/bufpage.h"
#include "utils/builtins.h"
#include "utils/rel.h"

#include "lion.h"
#include "lion_positions.h"
#include "lion_funcs.h"

/*
 * Make an empty position tree for the key whose entry hashes to hash: one
 * leaf, which is its root.  The root comes from extending the relation,
 * never from the free space map, so that its block is an identity, as a
 * posting set's root is (lion_entry_spill()).
 */
BlockNumber
lion_postree_create(Relation index, Relation heaprel, uint32 hash)
{
	Buffer		buf = lion_alloc_page(index, heaprel, false);
	BlockNumber root = BufferGetBlockNumber(buf);
	LionWalState *xstate;
	Page		page;

	xstate = lion_wal_begin(index);
	page = lion_wal_init_buffer(xstate, buf,
								LION_PAGE_CONTAINER | LION_PAGE_POSITIONS);
	lion_page_set_owner(page, hash, root);
	lion_wal_log_special(xstate, page);
	lion_wal_finish(xstate, LION_XLOG_PAGE_INIT);
	UnlockReleaseBuffer(buf);

	return root;
}

/* A leaf of the tree at root, or an ERROR: a writer's caller holds the key. */
static void
lion_postree_check_leaf(Relation index, Buffer buf, BlockNumber root)
{
	Page		page = BufferGetPage(buf);

	if (!lion_page_owns_positions(page, root) || !LionPageIsPostingLeaf(page))
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg("lion index \"%s\": page %u is not a leaf of the position tree at %u",
						RelationGetRelationName(index),
						BufferGetBlockNumber(buf), root)));
}

/* The code of a chunk's first member; false for an empty chunk. */
static bool
lion_postree_first_code(const LionContainer *c, uint64 *code)
{
	LionPosIter it;
	LionPosMember *m = palloc(sizeof(LionPosMember));
	bool		found;

	lion_poschunk_iter_init(&it, c);
	found = lion_poschunk_iter_next(&it, m);
	if (found)
		*code = m->code;
	pfree(m);
	return found;
}

/*
 * Put m into the position tree at root, replacing the member of the same
 * code if there is one (which is what the return value says).  The caller
 * holds the key's directory leaf EXCLUSIVE, as for any write to the key.
 */
bool
lion_postree_put(Relation index, Relation heaprel, uint32 hash,
				 BlockNumber root, const LionPosMember *m)
{
	uint32		block = lion_pos_block(m->code);
	LionTreeRef tree;
	Buffer		buf;
	Buffer		tbuf = InvalidBuffer;
	BlockNumber tblk = InvalidBlockNumber;
	OffsetNumber toff = InvalidOffsetNumber;
	bool		insert_new = false;
	LionContainer *work;
	bool		replaced = false;

	tree.hash = hash;
	tree.root = root;
	tree.kind = LION_PAGE_POSITIONS;
	tree.entrybuf = InvalidBuffer;
	tree.entryoff = InvalidOffsetNumber;
	tree.entry = NULL;

	buf = lion_posting_search_before(index, heaprel, hash, root, block,
									 BUFFER_LOCK_EXCLUSIVE, true);
	if (!BufferIsValid(buf))
		elog(ERROR, "lion index \"%s\": the position tree at %u is gone",
			 RelationGetRelationName(index), root);

	/*
	 * Find the target: the candidates are the last chunk whose header is
	 * below block (on this leaf, by P3) and then the chunks whose header is
	 * block, which may run on over the leaves to the right.  The walk keeps
	 * the leaf the target is on and lets go of every other one; nobody else
	 * writes to the tree meanwhile.
	 */
	for (;;)
	{
		Page		page;
		BlockNumber blk = BufferGetBlockNumber(buf);
		OffsetNumber maxoff;
		OffsetNumber off;
		bool		done = false;
		BlockNumber next;

		lion_postree_check_leaf(index, buf, root);
		page = BufferGetPage(buf);
		maxoff = PageGetMaxOffsetNumber(page);

		for (off = FirstOffsetNumber; off <= maxoff; off++)
		{
			LionContainer *c = lion_page_poschunk_fetch(index, page, blk, off);
			uint64		first;
			bool		nonempty;

			if (c->ckey > block)
			{
				/* the tree's very first chunk, when nothing is at or below block */
				if (!BlockNumberIsValid(tblk))
				{
					tblk = blk;
					toff = off;
				}
				done = true;
				break;
			}
			if (c->ckey < block)
			{
				tblk = blk;
				toff = off;
				continue;
			}
			nonempty = lion_postree_first_code(c, &first);
			if (!BlockNumberIsValid(tblk) || (nonempty && first <= m->code))
			{
				tblk = blk;
				toff = off;
			}
			else if (nonempty)
			{
				done = true;	/* this and every later candidate are above m */
				break;
			}
		}

		next = LionPageGetOpaque(page)->rightlink;
		if (LionPageIsRightmost(page))
			done = true;

		/* keep this leaf if the target is on it, and only then */
		if (tblk == blk)
		{
			if (BufferIsValid(tbuf))
				UnlockReleaseBuffer(tbuf);
			tbuf = buf;
		}
		else
			UnlockReleaseBuffer(buf);

		if (done)
			break;
		if (!BlockNumberIsValid(next) || next == root)
			ereport(ERROR,
					(errcode(ERRCODE_INDEX_CORRUPTED),
					 errmsg("lion index \"%s\": leaf %u of the position tree at %u links to block %u",
							RelationGetRelationName(index), blk, root, next)));
		buf = ReadBuffer(index, next);
		LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
	}

	if (BufferIsValid(tbuf) && BufferGetBlockNumber(tbuf) != tblk)
	{
		UnlockReleaseBuffer(tbuf);
		tbuf = InvalidBuffer;
	}

	if (!BlockNumberIsValid(tblk))
	{
		/*
		 * No chunk at all: the tree is empty, and the descent ended on its
		 * one leaf.  (An emptied leaf of a bigger tree is always followed by
		 * one with the same separator, which is where the descent ends.)
		 */
		buf = lion_posting_search_before(index, heaprel, hash, root, block,
										 BUFFER_LOCK_EXCLUSIVE, true);
		lion_postree_check_leaf(index, buf, root);
		if (PageGetMaxOffsetNumber(BufferGetPage(buf)) != InvalidOffsetNumber)
			elog(ERROR, "lion index \"%s\": position tree at %u has a chunk the walk did not see",
				 RelationGetRelationName(index), root);
		tbuf = buf;
		toff = FirstOffsetNumber;
		insert_new = true;
	}
	else if (!BufferIsValid(tbuf))
	{
		tbuf = ReadBuffer(index, tblk);
		LockBuffer(tbuf, BUFFER_LOCK_EXCLUSIVE);
		lion_postree_check_leaf(index, tbuf, root);
	}

	work = (LionContainer *) palloc0(LION_CONTAINER_MAX_SIZE);
	if (insert_new)
	{
		lion_poschunk_init(work, block);
		if (!lion_poschunk_append(work, LION_CONTAINER_MAX_SIZE, m))
			elog(ERROR, "lion index \"%s\": a position member does not fit an empty chunk",
				 RelationGetRelationName(index));
		lion_tree_put_items_locked(index, heaprel, tbuf, &tree, toff, false,
								   &work, 1, true);
	}
	else
	{
		LionContainer *c = lion_page_poschunk_fetch(index, BufferGetPage(tbuf),
													tblk, toff);

		memcpy(work, c, lion_poschunk_size(c));
		if (lion_poschunk_insert(work, LION_CONTAINER_MAX_SIZE, m, &replaced))
			lion_tree_put_items_locked(index, heaprel, tbuf, &tree, toff, true,
									   &work, 1, true);
		else
		{
			/* The chunk is full: split it, and m goes into its half. */
			LionContainer *halves[2];
			uint64		rfirst;

			halves[0] = (LionContainer *) palloc0(LION_CONTAINER_MAX_SIZE);
			halves[1] = (LionContainer *) palloc0(LION_CONTAINER_MAX_SIZE);
			if (!lion_poschunk_split(c, halves[0], halves[1]) ||
				!lion_postree_first_code(halves[1], &rfirst))
				ereport(ERROR,
						(errcode(ERRCODE_INDEX_CORRUPTED),
						 errmsg("lion index \"%s\": position chunk %u on page %u is full but cannot be split",
								RelationGetRelationName(index), toff, tblk)));
			if (!lion_poschunk_insert(halves[m->code < rfirst ? 0 : 1],
									  LION_CONTAINER_MAX_SIZE, m, &replaced))
				elog(ERROR, "lion index \"%s\": a position member does not fit half a chunk",
					 RelationGetRelationName(index));
			lion_tree_put_items_locked(index, heaprel, tbuf, &tree, toff, true,
									   halves, 2, true);
			pfree(halves[0]);
			pfree(halves[1]);
		}
	}
	pfree(work);
	UnlockReleaseBuffer(tbuf);

	return replaced;
}

/*
 * Find the member of the given code in the position tree at root, copying it
 * into *m.  A reader: SHARE locks, one page at a time.  False when there is
 * none, or when the tree is gone (the key was dropped meanwhile).
 */
bool
lion_postree_fetch(Relation index, uint32 hash, BlockNumber root,
				   uint64 code, LionPosMember *m)
{
	uint32		block = lion_pos_block(code);
	Buffer		buf;

	buf = lion_posting_search_before(index, NULL, hash, root, block,
									 BUFFER_LOCK_SHARE, false);
	while (BufferIsValid(buf))
	{
		Page		page = BufferGetPage(buf);
		BlockNumber blk = BufferGetBlockNumber(buf);
		OffsetNumber maxoff;
		OffsetNumber off;
		BlockNumber next;

		if (!lion_page_owns_positions(page, root) ||
			!LionPageIsPostingLeaf(page))
			break;
		maxoff = PageGetMaxOffsetNumber(page);
		for (off = FirstOffsetNumber; off <= maxoff; off++)
		{
			LionContainer *c = lion_page_poschunk_fetch(index, page, blk, off);

			if (c->ckey > block)
			{
				UnlockReleaseBuffer(buf);
				return false;
			}

			/*
			 * By (P2) a chunk whose successor's header is still below block
			 * holds nothing of block; only the last such chunk can.
			 */
			if (c->ckey < block && off < maxoff &&
				lion_page_poschunk_fetch(index, page, blk,
										 OffsetNumberNext(off))->ckey < block)
				continue;
			if (lion_poschunk_find(c, code, m))
			{
				UnlockReleaseBuffer(buf);
				return true;
			}
		}
		if (LionPageIsRightmost(page))
			break;
		next = LionPageGetOpaque(page)->rightlink;
		UnlockReleaseBuffer(buf);
		CHECK_FOR_INTERRUPTS();
		buf = ReadBuffer(index, next);
		LockBuffer(buf, BUFFER_LOCK_SHARE);
	}
	if (BufferIsValid(buf))
		UnlockReleaseBuffer(buf);
	return false;
}

/* ---------------------------------------------------------------------
 * lion_debug_postree_stress(): a test of the above against a reference
 * --------------------------------------------------------------------- */

/*
 * Not part of the extension's SQL: test/sql/postree.sql creates it from the
 * library for the length of the test.  It builds a position tree of its own
 * in the given (lion) index - pages nothing references, which the index
 * keeps until it is rebuilt or dropped - puts nops members into it, hotpct
 * percent of them into four hot heap blocks (block 0 among them) to make
 * runs of one block that fill several leaves, the rest scattered, appended or
 * replacing (maxnpos bounds the positions of most members) - and checks the
 * tree against an in-memory reference halfway through and at the end: a walk of every leaf, (P1) and (P2), every
 * member's positions, and a lookup of every member and of codes that are
 * not there.  Superuser only.
 */
PG_FUNCTION_INFO_V1(lion_debug_postree_stress);

typedef struct PosStressOp
{
	uint64		code;
	uint32		version;		/* the op's index: the member is a function of both */
} PosStressOp;

static uint64
pos_stress_mix(uint64 x)
{
	x += UINT64CONST(0x9E3779B97F4A7C15);
	x = (x ^ (x >> 30)) * UINT64CONST(0xBF58476D1CE4E5B9);
	x = (x ^ (x >> 27)) * UINT64CONST(0x94D049BB133111EB);
	return x ^ (x >> 31);
}

/* The member a (code, version) stands for: positions ascending, < 16384. */
static void
pos_stress_member(LionPosMember *m, uint64 code, uint32 version,
				  uint32 maxnpos, uint64 seed)
{
	uint64		s = pos_stress_mix(code ^ ((uint64) version << 40) ^ seed);
	uint32		shape = (uint32) (s % 100);
	uint32		npos;
	int			prev = -1;
	uint32		i;

	if (shape < 2)
		npos = 0;				/* a stripped tsvector's member */
	else if (shape < 4)
		npos = LION_POS_MAX_NPOS;
	else
		npos = 1 + (uint32) ((s >> 8) % maxnpos);

	m->code = code;
	m->npos = (uint16) npos;
	for (i = 0; i < npos; i++)
	{
		int			room = (LION_POS_LIMIT - 1 - (int) (npos - 1 - i)) - prev;
		int			step;

		s = pos_stress_mix(s);
		step = 1 + (int) (s % (uint64) Min(room, 60));
		prev += step;
		m->pos[i] = LION_POS_MAKE(prev, (s >> 32) & 3);
	}
}

static int
pos_stress_cmp(const void *a, const void *b)
{
	const PosStressOp *x = a;
	const PosStressOp *y = b;

	if (x->code != y->code)
		return x->code < y->code ? -1 : 1;
	return x->version < y->version ? -1 : (x->version > y->version ? 1 : 0);
}

static bool
pos_member_eq(const LionPosMember *a, const LionPosMember *b)
{
	return a->code == b->code && a->npos == b->npos &&
		memcmp(a->pos, b->pos, sizeof(uint16) * a->npos) == 0;
}

/* Check the tree at root against ops[0 .. nops), returning its leaf count. */
static int64
pos_stress_check(Relation index, uint32 hash, BlockNumber root,
				 const PosStressOp *ops, int nops, uint32 maxnpos, uint64 seed,
				 int64 *nmembers)
{
	PosStressOp *ref = palloc(sizeof(PosStressOp) * Max(nops, 1));
	int			nref = 0;
	int			i;
	int			at = 0;
	int64		leaves = 0;
	bool		any = false;
	uint64		lastcode = 0;
	bool		havechunk = false;
	uint32		lastheader = 0;
	uint32		lastblock = 0;	/* the block of the previous chunk's last member */
	LionPosMember *got = palloc(sizeof(LionPosMember));
	LionPosMember *want = palloc(sizeof(LionPosMember));
	Buffer		buf;

	/* the reference: the last version of every code */
	memcpy(ref, ops, sizeof(PosStressOp) * nops);
	qsort(ref, nops, sizeof(PosStressOp), pos_stress_cmp);
	for (i = 0; i < nops; i++)
	{
		if (nref > 0 && ref[nref - 1].code == ref[i].code)
			ref[nref - 1] = ref[i];
		else
			ref[nref++] = ref[i];
	}

	/* the walk: every leaf from the leftmost, where a descent for 0 ends */
	buf = lion_posting_search_before(index, NULL, hash, root, 0,
									 BUFFER_LOCK_SHARE, false);
	while (BufferIsValid(buf))
	{
		Page		page = BufferGetPage(buf);
		BlockNumber blk = BufferGetBlockNumber(buf);
		OffsetNumber maxoff = PageGetMaxOffsetNumber(page);
		OffsetNumber off;
		BlockNumber next;

		lion_postree_check_leaf(index, buf, root);
		leaves++;
		for (off = FirstOffsetNumber; off <= maxoff; off++)
		{
			LionContainer *c = lion_page_poschunk_fetch(index, page, blk, off);
			char		err[256];
			LionPosIter it;
			bool		first = true;

			if (!lion_poschunk_check(c, ItemIdGetLength(PageGetItemId(page, off)),
									 err, sizeof(err)))
				elog(ERROR, "postree stress: chunk %u on page %u: %s",
					 off, blk, err);
			if (havechunk && c->ckey < lastheader)
				elog(ERROR, "postree stress: chunk %u on page %u has header %u below the previous one's %u",
					 off, blk, c->ckey, lastheader);
			if (havechunk && any && lastblock > c->ckey)
				elog(ERROR, "postree stress: (P2) the chunk before chunk %u on page %u ends at block %u, past its header %u",
					 off, blk, lastblock, c->ckey);
			havechunk = true;
			lastheader = c->ckey;

			lion_poschunk_iter_init(&it, c);
			while (lion_poschunk_iter_next(&it, got))
			{
				if (first && lion_pos_block(got->code) < c->ckey)
					elog(ERROR, "postree stress: chunk %u on page %u starts below its header",
						 off, blk);
				first = false;
				if (any && got->code <= lastcode)
					elog(ERROR, "postree stress: (P1) code " UINT64_FORMAT " on page %u follows " UINT64_FORMAT,
						 got->code, blk, lastcode);
				if (at >= nref || ref[at].code != got->code)
					elog(ERROR, "postree stress: the tree has code " UINT64_FORMAT " where the reference has " UINT64_FORMAT,
						 got->code, at < nref ? ref[at].code : 0);
				pos_stress_member(want, ref[at].code, ref[at].version, maxnpos,
								  seed);
				if (!pos_member_eq(got, want))
					elog(ERROR, "postree stress: the positions of code " UINT64_FORMAT " differ",
						 got->code);
				at++;
				any = true;
				lastcode = got->code;
				lastblock = lion_pos_block(got->code);
			}
		}
		next = LionPageGetOpaque(page)->rightlink;
		if (LionPageIsRightmost(page))
		{
			UnlockReleaseBuffer(buf);
			break;
		}
		UnlockReleaseBuffer(buf);
		buf = ReadBuffer(index, next);
		LockBuffer(buf, BUFFER_LOCK_SHARE);
	}
	if (at != nref)
		elog(ERROR, "postree stress: the walk found %d members of %d", at, nref);

	/* every member by lookup, and the codes just around it */
	for (i = 0; i < nref; i++)
	{
		if (!lion_postree_fetch(index, hash, root, ref[i].code, got))
			elog(ERROR, "postree stress: lookup of code " UINT64_FORMAT " found nothing",
				 ref[i].code);
		pos_stress_member(want, ref[i].code, ref[i].version, maxnpos, seed);
		if (!pos_member_eq(got, want))
			elog(ERROR, "postree stress: lookup of code " UINT64_FORMAT " found other positions",
				 ref[i].code);
		if ((i + 1 >= nref || ref[i + 1].code != ref[i].code + 1) &&
			lion_postree_fetch(index, hash, root, ref[i].code + 1, got))
			elog(ERROR, "postree stress: lookup of absent code " UINT64_FORMAT " found one",
				 ref[i].code + 1);
		if (ref[i].code > 0 && (i == 0 || ref[i - 1].code != ref[i].code - 1) &&
			lion_postree_fetch(index, hash, root, ref[i].code - 1, got))
			elog(ERROR, "postree stress: lookup of absent code " UINT64_FORMAT " found one",
				 ref[i].code - 1);
		CHECK_FOR_INTERRUPTS();
	}

	*nmembers = nref;
	pfree(ref);
	pfree(got);
	pfree(want);
	return leaves;
}

Datum
lion_debug_postree_stress(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	int32		nops = PG_GETARG_INT32(1);
	int32		maxnpos = PG_GETARG_INT32(2);
	int64		seed = PG_GETARG_INT64(3);
	int32		hotpct = PG_GETARG_INT32(4);
	Relation	index;
	uint32		hash;
	BlockNumber root;
	PosStressOp *ops;
	LionPosMember *m;
	uint64		maxcode = 0;
	uint64		s = (uint64) seed;
	int64		leaves = 0;
	int64		nmembers = 0;
	int64		replaced = 0;
	uint16		height;
	Buffer		rbuf;
	int			i;
	static const uint32 hot[] = {0, 1, 7, 100};

	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("lion_debug_postree_stress() is for superusers")));
	if (nops < 0 || maxnpos < 1 || maxnpos > LION_POS_MAX_NPOS ||
		hotpct < 0 || hotpct > 70)
		elog(ERROR, "postree stress: bad arguments");

	index = lion_open_index(relid, RowExclusiveLock);
	hash = (uint32) pos_stress_mix(s);
	root = lion_postree_create(index, NULL, hash);
	ops = palloc(sizeof(PosStressOp) * Max(nops, 1));
	m = palloc(sizeof(LionPosMember));

	for (i = 0; i < nops; i++)
	{
		uint32		r;
		uint64		code;

		s = pos_stress_mix(s);
		r = (uint32) (s % 100);
		if (r < (uint32) hotpct)
			code = lion_pos_block_base(hot[(s >> 8) % lengthof(hot)]) +
				(s >> 16) % (LION_MAX_OFFSET + 1);
		else if (r < 70)
			code = lion_make_code((uint32) ((s >> 8) % 5000),
								  (uint16) ((s >> 24) % LION_CONTAINER_RANGE));
		else if (r < 90 || i == 0)
			code = maxcode + 1 + (s >> 8) % 5;
		else
			code = ops[(s >> 8) % i].code;	/* a replacement */

		ops[i].code = code;
		ops[i].version = (uint32) i;
		maxcode = Max(maxcode, code);
		pos_stress_member(m, code, (uint32) i, (uint32) maxnpos, (uint64) seed);
		if (lion_postree_put(index, NULL, hash, root, m))
			replaced++;

		if (i + 1 == nops / 2)
			(void) pos_stress_check(index, hash, root, ops, i + 1,
									(uint32) maxnpos, (uint64) seed, &nmembers);
		CHECK_FOR_INTERRUPTS();
	}

	leaves = pos_stress_check(index, hash, root, ops, nops, (uint32) maxnpos,
							  (uint64) seed, &nmembers);
	if (nmembers + replaced != nops)
		elog(ERROR, "postree stress: %d puts, " INT64_FORMAT " members and " INT64_FORMAT " replacements",
			 nops, nmembers, replaced);

	rbuf = ReadBuffer(index, root);
	LockBuffer(rbuf, BUFFER_LOCK_SHARE);
	height = LionPageGetOpaque(BufferGetPage(rbuf))->level + 1;
	UnlockReleaseBuffer(rbuf);

	index_close(index, RowExclusiveLock);

	PG_RETURN_TEXT_P(cstring_to_text(psprintf("members " INT64_FORMAT ", replaced " INT64_FORMAT ", leaves " INT64_FORMAT ", height %u",
											  nmembers, replaced, leaves,
											  (unsigned) height)));
}
