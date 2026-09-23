/*-------------------------------------------------------------------------
 * lion_wal.c
 *	  The WAL shim and the custom resource manager (DESIGN.md §25).
 *
 * Two halves live here.
 *
 * The SHIM (lion_wal_begin/register_buffer/op/finish) is what every write
 * path in this extension calls.  It writes either a GenericXLog record - the
 * page image is copied, modified and diffed, which is what the extension did
 * before §25 - or one of our own records, which carries the bytes that
 * changed and nothing else.  Which one an index gets is decided once, at
 * CREATE INDEX, and recorded in the meta page's `wal_mode`, so a cluster can
 * hold both kinds and a REINDEX moves an index from one to the other.
 *
 * The RESOURCE MANAGER (lion_redo/desc/identify/mask) replays them.  Two
 * properties of it are load-bearing beyond crash recovery, and DESIGN.md §9
 * and §11 rest on them:
 *
 *	- a record that removes TIDs or deletes items takes a CLEANUP lock on the
 *	  page it removes them from, which is the standby half of §11's rule.  A
 *	  standby reader that holds a pin therefore blocks replay of the removal
 *	  exactly as it blocks VACUUM on the primary, and the count pushdown may
 *	  trust the visibility map in recovery again (§9's last rule, which the
 *	  generic-WAL path still has to obey);
 *	- redo reproduces every page byte for byte, which
 *	  `wal_consistency_checking = 'pg_lion'` proves for the whole regression
 *	  suite.
 *
 * The record catalogue is in lion_wal.h and in DESIGN.md §25.
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/rmgr.h"
#include "access/xlog.h"
#include "access/xloginsert.h"
#include "access/xlogutils.h"
#include "access/itup.h"
#include "access/bufmask.h"
#include "lib/stringinfo.h"
#include "miscadmin.h"
#include "storage/bufmgr.h"
#include "utils/guc.h"
#include "utils/memutils.h"
#include "access/xact.h"

#include "lion.h"
#include "lion_wal.h"

/* The rmid this server registered us under; -1 when we are not registered. */
int			lion_rmgr_id = RM_EXPERIMENTAL_ID;
static bool lion_rmgr_is_registered = false;

/* ---------------------------------------------------------------------
 * The shim
 * --------------------------------------------------------------------- */

typedef struct LionWalBlock
{
	Buffer		buf;
	Page		page;			/* what the caller was handed */
	uint8		regflags;		/* REGBUF_* */
	StringInfoData ops;			/* the operation stream (rmgr mode only) */
	Size		lastop;			/* offset of the last op header in ops */
} LionWalBlock;

struct LionWalState
{
	Relation	index;
	bool		rmgr;			/* writing one of our own records? */
	bool		needwal;		/* RelationNeedsWAL() */
	bool		incrit;			/* did we open the critical section? */
	int			nblocks;
	GenericXLogState *gx;		/* generic mode only */
	xl_lion_header hdr;
	StringInfoData main;		/* record-level payload after the header */
	LionWalBlock blk[LION_WAL_MAX_BLOCKS];

	/* The item image a DELTA is computed against (lion_wal_save_item()). */
	Page		savepage;		/* NULL when nothing is saved */
	OffsetNumber saveoff;
	Size		savelen;
	char	   *savebuf;		/* BLCKSZ bytes, allocated once */
};

/*
 * One record is open at a time - the storage code never nests them - so the
 * state is a singleton with buffers that are allocated once and reset per
 * record.  GenericXLogStart() palloc's 32 KB (four page images) for every
 * record it writes; an rmgr-mode record allocates nothing at all, which is
 * part of what §25 is for.
 */
static LionWalState lion_wal_cur;
static bool lion_wal_open = false;
static bool lion_wal_removal = false;
static MemoryContext lion_wal_cxt = NULL;

/*
 * An ERROR between lion_wal_begin() and lion_wal_finish() unwinds past both,
 * leaving the singleton marked as in use.  In rmgr mode that cannot happen -
 * the record is inside a critical section, so the ERROR is a PANIC - but in
 * generic mode it can (the "could not add an item" checks), and without this
 * the next record in the same backend would refuse to start.  GenericXLog
 * needed nothing of the kind because its state was a palloc that the aborting
 * context freed.
 */
static void
lion_wal_xact_callback(XactEvent event, void *arg)
{
	if (event == XACT_EVENT_ABORT || event == XACT_EVENT_PARALLEL_ABORT)
	{
		lion_wal_open = false;
		lion_wal_removal = false;
	}
}

static void
lion_wal_subxact_callback(SubXactEvent event, SubTransactionId mySubid,
						  SubTransactionId parentSubid, void *arg)
{
	if (event == SUBXACT_EVENT_ABORT_SUB)
	{
		lion_wal_open = false;
		lion_wal_removal = false;
	}
}

/*
 * How large each block's operation stream starts out.  The largest record any
 * call site writes is a directory split, which logs a whole page of items
 * plus two bytes of length for each of them; BLCKSZ + a page's worth of
 * two-byte lengths covers it with room to spare, so the stream never grows
 * inside the critical section.
 */
#define LION_WAL_OPS_INITSZ		(BLCKSZ + BLCKSZ / 4)

static void
lion_wal_setup_buffers(void)
{
	int			i;

	if (lion_wal_cxt != NULL)
		return;

	lion_wal_cxt = AllocSetContextCreate(TopMemoryContext,
										 "pg_lion WAL",
										 ALLOCSET_SMALL_SIZES);

	/*
	 * The operation streams are filled while the record is open, which in
	 * rmgr mode is inside a critical section, so this context has to say that
	 * allocating in one is intended - the same declaration core makes for the
	 * buffers XLogRecordAssemble() fills.  The buffers are sized here so that
	 * they practically never grow: a record describes at most one page's
	 * worth of items per block, plus one eight-byte operation header each.
	 */
	MemoryContextAllowInCriticalSection(lion_wal_cxt, true);

	RegisterXactCallback(lion_wal_xact_callback, NULL);
	RegisterSubXactCallback(lion_wal_subxact_callback, NULL);

	{
		MemoryContext old = MemoryContextSwitchTo(lion_wal_cxt);

		initStringInfo(&lion_wal_cur.main);
		enlargeStringInfo(&lion_wal_cur.main, 256);
		for (i = 0; i < LION_WAL_MAX_BLOCKS; i++)
		{
			initStringInfo(&lion_wal_cur.blk[i].ops);
			enlargeStringInfo(&lion_wal_cur.blk[i].ops, LION_WAL_OPS_INITSZ);
		}
		lion_wal_cur.savebuf = (char *) palloc(BLCKSZ);
		MemoryContextSwitchTo(old);
	}
}

/*
 * The wal_mode of an index.  The meta page is cached in rd_amcache, so this
 * is a pointer chase on every path that has already looked a key up.
 */
int
lion_wal_mode(Relation index)
{
	LionIndexState *ix = lion_get_index_state(index);

	return (ix->meta.wal_mode == LION_WAL_MODE_RMGR) ?
		LION_WAL_MODE_RMGR : LION_WAL_MODE_GENERIC;
}

void
lion_wal_removal_begin(void)
{
	Assert(!lion_wal_removal);
	lion_wal_removal = true;
}

void
lion_wal_removal_end(void)
{
	lion_wal_removal = false;
}

bool
lion_rmgr_registered(void)
{
	return lion_rmgr_is_registered;
}

LionWalState *
lion_wal_begin(Relation index)
{
	LionWalState *state = &lion_wal_cur;
	int			i;

	if (lion_wal_open)
		elog(ERROR, "lion index: a WAL record is already open");

	lion_wal_setup_buffers();

	state->index = index;
	state->nblocks = 0;
	state->gx = NULL;
	state->incrit = false;
	state->needwal = RelationNeedsWAL(index);
	state->hdr.initmask = 0;
	state->hdr.cleanupmask = 0;
	state->hdr.unused = 0;
	state->savepage = NULL;
	state->saveoff = InvalidOffsetNumber;
	state->savelen = 0;
	resetStringInfo(&state->main);
	for (i = 0; i < LION_WAL_MAX_BLOCKS; i++)
	{
		state->blk[i].buf = InvalidBuffer;
		state->blk[i].page = NULL;
		state->blk[i].regflags = 0;
		state->blk[i].lastop = 0;
		resetStringInfo(&state->blk[i].ops);
	}

	state->rmgr = (lion_wal_mode(index) == LION_WAL_MODE_RMGR);

	if (state->rmgr && !lion_rmgr_is_registered)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("lion index \"%s\" was built for the pg_lion WAL resource manager, which this server has not registered",
						RelationGetRelationName(index)),
				 errdetail("The index can be read, but not written, without the resource manager."),
				 errhint("Add \"pg_lion\" to shared_preload_libraries and restart, or REINDEX the index to rebuild it with wal_mode = generic.")));

	lion_wal_open = true;

	if (state->rmgr)
	{
		/*
		 * From here the caller works on the PAGES, so an ERROR before the
		 * record is inserted would leave modified pages in shared buffers
		 * that nothing describes.  Everything fallible has to be behind us
		 * (see the header of lion_wal.h).
		 */
		START_CRIT_SECTION();
		state->incrit = true;
	}
	else
		state->gx = GenericXLogStart(index);

	return state;
}

Page
lion_wal_register_buffer(LionWalState *state, Buffer buf, int flags)
{
	LionWalBlock *b;
	int			i;

	Assert(lion_wal_open && state == &lion_wal_cur);

	/* A buffer registered twice is the same block; hand back its page. */
	for (i = 0; i < state->nblocks; i++)
	{
		if (state->blk[i].buf == buf)
			return state->blk[i].page;
	}

	if (state->nblocks >= LION_WAL_MAX_BLOCKS)
		elog(ERROR, "lion index: too many buffers in one WAL record");

	b = &state->blk[state->nblocks];
	b->buf = buf;

	/*
	 * VACUUM's regrow window: the first block of every record written there
	 * is the container page TIDs are leaving, whatever the operation looks
	 * like (see lion_wal_removal_begin()).
	 */
	if (lion_wal_removal && state->nblocks == 0)
		flags |= LION_WALBUF_CLEANUP;

	if ((flags & LION_WALBUF_INIT) != 0)
		state->hdr.initmask |= (uint8) (1 << state->nblocks);
	if ((flags & LION_WALBUF_CLEANUP) != 0)
		state->hdr.cleanupmask |= (uint8) (1 << state->nblocks);

	if (state->rmgr)
	{
		b->regflags = REGBUF_STANDARD;
		if ((flags & LION_WALBUF_INIT) != 0)
			b->regflags |= REGBUF_WILL_INIT;
		if ((flags & LION_WALBUF_IMAGE) != 0)
			b->regflags |= REGBUF_FORCE_IMAGE;
		b->page = BufferGetPage(buf);
	}
	else
	{
		int			gflags = 0;

		if ((flags & (LION_WALBUF_INIT | LION_WALBUF_IMAGE)) != 0)
			gflags |= GENERIC_XLOG_FULL_IMAGE;
		b->page = GenericXLogRegisterBuffer(state->gx, buf, gflags);
	}

	state->nblocks++;
	return b->page;
}

static LionWalBlock *
lion_wal_find_block(LionWalState *state, Page page)
{
	int			i;

	for (i = 0; i < state->nblocks; i++)
	{
		if (state->blk[i].page == page)
			return &state->blk[i];
	}

	elog(ERROR, "lion index: page is not registered in this WAL record");
	return NULL;				/* keep the compiler quiet */
}

void
lion_wal_op(LionWalState *state, Page page, uint8 op, OffsetNumber off,
			uint16 aux, const void *data, Size len)
{
	LionWalBlock *b;
	xl_lion_op	hdr;

	Assert(lion_wal_open && state == &lion_wal_cur);

	if (!state->rmgr)
		return;					/* the byte-wise diff describes it already */

	if (len > PG_UINT16_MAX)
		elog(ERROR, "lion index: WAL operation payload of %zu bytes is too large",
			 len);

	b = lion_wal_find_block(state, page);

	hdr.op = op;
	hdr.unused = 0;
	hdr.off = (uint16) off;
	hdr.aux = aux;
	hdr.len = (uint16) len;

	b->lastop = (Size) b->ops.len;
	appendBinaryStringInfo(&b->ops, (const char *) &hdr, (int) SizeOfLionOp);
	if (len > 0)
		appendBinaryStringInfo(&b->ops, (const char *) data, (int) len);
}

void
lion_wal_op_append(LionWalState *state, Page page, const void *data, Size len)
{
	LionWalBlock *b;
	xl_lion_op	hdr;

	Assert(lion_wal_open && state == &lion_wal_cur);

	if (!state->rmgr || len == 0)
		return;

	b = lion_wal_find_block(state, page);
	Assert(b->ops.len >= (int) (b->lastop + SizeOfLionOp));

	memcpy(&hdr, b->ops.data + b->lastop, SizeOfLionOp);
	if ((Size) hdr.len + len > PG_UINT16_MAX)
		elog(ERROR, "lion index: WAL operation payload is too large");
	hdr.len = (uint16) (hdr.len + len);
	memcpy(b->ops.data + b->lastop, &hdr, SizeOfLionOp);

	appendBinaryStringInfo(&b->ops, (const char *) data, (int) len);
}

void
lion_wal_op_count(LionWalState *state, Page page, uint16 aux)
{
	LionWalBlock *b;
	xl_lion_op	hdr;

	Assert(lion_wal_open && state == &lion_wal_cur);

	if (!state->rmgr)
		return;

	b = lion_wal_find_block(state, page);
	Assert(b->ops.len >= (int) (b->lastop + SizeOfLionOp));

	memcpy(&hdr, b->ops.data + b->lastop, SizeOfLionOp);
	hdr.aux = aux;
	memcpy(b->ops.data + b->lastop, &hdr, SizeOfLionOp);
}

/*
 * How many equal bytes it takes to close a delta range.
 *
 * A range costs four bytes of header, so a handful of bytes that happen to be
 * equal are cheaper to carry along than to skip: the scan only ends a range
 * once it has seen this many in a row.
 */
#define LION_WAL_DELTA_GAP		5

void
lion_wal_save_item(LionWalState *state, Page page, OffsetNumber off)
{
	ItemId		iid;
	Size		len;

	Assert(lion_wal_open && state == &lion_wal_cur);

	state->savepage = NULL;

	if (!state->rmgr)
		return;					/* the byte-wise diff is the delta already */

	/* It has to be a page of this record; finding its block says so. */
	(void) lion_wal_find_block(state, page);

	if (off < FirstOffsetNumber || off > PageGetMaxOffsetNumber(page))
		return;
	iid = PageGetItemId(page, off);
	if (!ItemIdHasStorage(iid))
		return;
	len = ItemIdGetLength(iid);
	if (len == 0 || len > BLCKSZ)
		return;					/* corrupt: let the full REPLACE carry it */

	memcpy(state->savebuf, PageGetItem(page, iid), len);
	state->savepage = page;
	state->saveoff = off;
	state->savelen = len;
}

void
lion_wal_op_replace(LionWalState *state, Page page, OffsetNumber off,
					const void *item, Size len)
{
	LionWalBlock *b;
	const char *newitem = (const char *) item;
	const char *old;
	Size		oldlen;
	Size		startpos;
	Size		budget;
	Size		i;

	Assert(lion_wal_open && state == &lion_wal_cur);

	if (!state->rmgr)
		return;

	if (state->savepage != page || state->saveoff != off ||
		len == 0 || len > BLCKSZ)
	{
		lion_wal_op(state, page, LION_OP_REPLACE, off, 0, item, len);
		state->savepage = NULL;
		return;
	}

	old = state->savebuf;
	oldlen = state->savelen;
	state->savepage = NULL;		/* one image, one delta */

	/*
	 * Redo's base image is the old item zero-extended to the new length, so
	 * make the saved copy exactly that: the comparison below is then between
	 * two flat buffers and can run a word at a time, which matters because it
	 * runs inside the critical section on every insert.
	 */
	if (len > oldlen)
	{
		memset(state->savebuf + oldlen, 0, len - oldlen);
		oldlen = len;
	}

	b = lion_wal_find_block(state, page);
	startpos = (Size) b->ops.len;

	/*
	 * What the alternative costs: one operation header and every byte of the
	 * item.  A delta that saves less than half of that is not worth having -
	 * it is more bytes of WAL for a replay that does more work - and the item
	 * goes in whole instead.
	 */
	budget = SizeOfLionOp + len / 2;

	lion_wal_op(state, page, LION_OP_DELTA, off, (uint16) len, NULL, 0);

	i = 0;
	while (i < len)
	{
		Size		start;
		Size		end;
		Size		equal;
		xl_lion_range r;

		/* Skip what did not change, eight bytes at a time. */
		while (i + sizeof(uint64) <= len &&
			   memcmp(newitem + i, old + i, sizeof(uint64)) == 0)
			i += sizeof(uint64);
		while (i < len && newitem[i] == old[i])
			i++;
		if (i >= len)
			break;

		/*
		 * A run of changed bytes, swallowing short runs of equal ones: the
		 * base image is the item as the page holds it, zero-extended when the
		 * item grew (which is exactly what redo reconstructs).
		 */
		start = i;
		end = i + 1;
		equal = 0;
		for (i = start + 1; i < len; i++)
		{
			if (newitem[i] != old[i])
			{
				end = i + 1;
				equal = 0;
			}
			else if (++equal >= LION_WAL_DELTA_GAP)
				break;
		}

		r.at = (uint16) start;
		r.len = (uint16) (end - start);
		lion_wal_op_append(state, page, &r, SizeOfLionRange);
		lion_wal_op_append(state, page, newitem + start, end - start);
		i = end;

		if ((Size) b->ops.len - startpos > budget)
		{
			/*
			 * Too much of the item changed.  Nothing but this operation has
			 * been appended since startpos, so dropping it leaves the stream
			 * exactly as it was and the item is logged whole.
			 */
			b->ops.len = (int) startpos;
			b->ops.data[startpos] = '\0';
			b->lastop = startpos;
			lion_wal_op(state, page, LION_OP_REPLACE, off, 0, item, len);
			return;
		}
	}
}

void
lion_wal_register_data(LionWalState *state, const void *ptr, Size len)
{
	Assert(lion_wal_open && state == &lion_wal_cur);

	if (!state->rmgr)
		return;

	appendBinaryStringInfo(&state->main, (const char *) ptr, (int) len);
}

void
lion_wal_finish(LionWalState *state, uint8 info)
{
	int			i;

	Assert(lion_wal_open && state == &lion_wal_cur);

	if (!state->rmgr)
	{
		GenericXLogFinish(state->gx);
		lion_wal_open = false;
		return;
	}

	for (i = 0; i < state->nblocks; i++)
		MarkBufferDirty(state->blk[i].buf);

	if (state->needwal)
	{
		XLogRecPtr	recptr;

		XLogBeginInsert();
		XLogRegisterData(&state->hdr, (uint32) SizeOfLionHeader);
		if (state->main.len > 0)
			XLogRegisterData(state->main.data, (uint32) state->main.len);

		for (i = 0; i < state->nblocks; i++)
			XLogRegisterBuffer((uint8) i, state->blk[i].buf,
							   state->blk[i].regflags);

		for (i = 0; i < state->nblocks; i++)
		{
			if (state->blk[i].ops.len > 0)
				XLogRegisterBufData((uint8) i, state->blk[i].ops.data,
									(uint32) state->blk[i].ops.len);
		}

		recptr = XLogInsert((RmgrId) lion_rmgr_id, info);

		for (i = 0; i < state->nblocks; i++)
			PageSetLSN(state->blk[i].page, recptr);
	}

	END_CRIT_SECTION();
	state->incrit = false;
	lion_wal_open = false;
}

void
lion_wal_abort(LionWalState *state)
{
	Assert(lion_wal_open && state == &lion_wal_cur);

	if (!state->rmgr)
	{
		GenericXLogAbort(state->gx);
		lion_wal_open = false;
		return;
	}

#ifdef USE_ASSERT_CHECKING
	{
		int			i;

		for (i = 0; i < state->nblocks; i++)
			Assert(state->blk[i].ops.len == 0);
	}
#endif

	END_CRIT_SECTION();
	state->incrit = false;
	lion_wal_open = false;
}

/* ---------------------------------------------------------------------
 * Replay
 * --------------------------------------------------------------------- */

/*
 * Apply one block's operation stream to a page.
 *
 * Every operation is a function of the record and of the page as the writer
 * found it, so replaying them produces the writer's page byte for byte - which
 * is what wal_consistency_checking checks and what lets rm_mask() get away
 * with masking nothing but the page header and the free space.
 *
 * Two operations replay the writer's CALL rather than its bytes
 * (LION_OP_CONTAINER_ADD, LION_OP_SPARSE_INS): adding one member to a sorted
 * ARRAY shifts every element above it, so logging the bytes would cost up to
 * 4 KB where the call costs twelve.  They are deterministic for the same
 * reason every other operation is - same input item, same library function.
 */
/*
 * Where LION_OP_DELTA rebuilds an item whose length changed.  One buffer for
 * the whole startup process: redo is single-threaded and the buffer is used
 * and finished with inside one operation.
 */
static PGAlignedBlock lion_redo_scratch;

static void
lion_redo_apply(Page page, char *data, Size len, BlockNumber blkno)
{
	Size		pos = 0;

	while (pos < len)
	{
		xl_lion_op	op;
		char	   *payload;

		if (pos + SizeOfLionOp > len)
			elog(PANIC, "pg_lion: truncated operation stream on block %u", blkno);
		memcpy(&op, data + pos, SizeOfLionOp);
		pos += SizeOfLionOp;
		if (pos + op.len > len)
			elog(PANIC, "pg_lion: operation %u on block %u runs past the record",
				 op.op, blkno);
		payload = data + pos;
		pos += op.len;

		switch (op.op)
		{
			case LION_OP_INIT:

				/*
				 * The page flags travel in aux so that the operation is eight
				 * bytes; everything else in the special area is set by the
				 * SPECIAL operation the writer emits once it is final, which
				 * is after the items have been placed.
				 */
				lion_init_page(page, op.aux);
				if (op.len == sizeof(LionPageOpaqueData))
					memcpy(LionPageGetOpaque(page), payload,
						   sizeof(LionPageOpaqueData));
				else if (op.len != 0)
					elog(PANIC, "pg_lion: bad INIT payload on block %u", blkno);
				break;

			case LION_OP_SPECIAL:
				if (op.len != sizeof(LionPageOpaqueData))
					elog(PANIC, "pg_lion: bad SPECIAL payload on block %u", blkno);
				memcpy(LionPageGetOpaque(page), payload,
					   sizeof(LionPageOpaqueData));
				break;

			case LION_OP_ADD:
				if (PageAddItemExtended(page, payload, op.len, op.off, 0) ==
					InvalidOffsetNumber)
					elog(PANIC, "pg_lion: could not add an item at %u on block %u",
						 op.off, blkno);
				break;

			case LION_OP_ADDMANY:
				{
					Size		p = 0;
					int			k;

					for (k = 0; k < (int) op.aux; k++)
					{
						uint16		isz;

						if (p + sizeof(uint16) > op.len)
							elog(PANIC, "pg_lion: truncated ADDMANY on block %u",
								 blkno);
						memcpy(&isz, payload + p, sizeof(uint16));
						p += sizeof(uint16);
						if (p + isz > op.len)
							elog(PANIC, "pg_lion: truncated ADDMANY item on block %u",
								 blkno);
						if (PageAddItemExtended(page, payload + p, isz,
												op.off + (OffsetNumber) k,
												0) == InvalidOffsetNumber)
							elog(PANIC, "pg_lion: could not add item %d at %u on block %u",
								 k, op.off, blkno);
						p += isz;
					}
					if (p != op.len)
						elog(PANIC, "pg_lion: trailing bytes in ADDMANY on block %u",
							 blkno);
				}
				break;

			case LION_OP_REPLACE:
				if (!PageIndexTupleOverwrite(page, op.off, payload, op.len))
					elog(PANIC, "pg_lion: could not overwrite item %u on block %u",
						 op.off, blkno);
				break;

			case LION_OP_DELTA:
				{
					ItemId		iid;
					Size		oldlen;
					Size		newlen = (Size) op.aux;
					Size		p = 0;
					char	   *target;

					if (op.off < FirstOffsetNumber ||
						op.off > PageGetMaxOffsetNumber(page))
						elog(PANIC, "pg_lion: DELTA for item %u past the end of block %u",
							 op.off, blkno);
					iid = PageGetItemId(page, op.off);
					if (!ItemIdHasStorage(iid))
						elog(PANIC, "pg_lion: DELTA for unused item %u on block %u",
							 op.off, blkno);
					oldlen = ItemIdGetLength(iid);
					if (newlen == 0 || newlen > BLCKSZ || oldlen > BLCKSZ)
						elog(PANIC, "pg_lion: DELTA of %zu bytes for item %u on block %u",
							 newlen, op.off, blkno);

					/*
					 * The base image is the item as this page holds it, cut to
					 * the new length or zero-extended to it - the very bytes
					 * the writer compared against.  A length that does not
					 * change is patched where it lies, which moves nothing;
					 * otherwise the patched image goes back through
					 * PageIndexTupleOverwrite(), exactly as on the primary.
					 */
					if (newlen == oldlen)
						target = (char *) PageGetItem(page, iid);
					else
					{
						target = lion_redo_scratch.data;
						memcpy(target, PageGetItem(page, iid),
							   Min(oldlen, newlen));
						if (newlen > oldlen)
							memset(target + oldlen, 0, newlen - oldlen);
					}

					while (p < op.len)
					{
						xl_lion_range r;

						if (p + SizeOfLionRange > op.len)
							elog(PANIC, "pg_lion: truncated DELTA on block %u",
								 blkno);
						memcpy(&r, payload + p, SizeOfLionRange);
						p += SizeOfLionRange;
						if (p + r.len > op.len ||
							(Size) r.at + r.len > newlen)
							elog(PANIC, "pg_lion: DELTA range %u+%u past item %u on block %u",
								 r.at, r.len, op.off, blkno);
						memcpy(target + r.at, payload + p, r.len);
						p += r.len;
					}

					if (newlen != oldlen &&
						!PageIndexTupleOverwrite(page, op.off, target, newlen))
						elog(PANIC, "pg_lion: could not resize item %u to %zu bytes on block %u",
							 op.off, newlen, blkno);
				}
				break;

			case LION_OP_SETBYTES:
				{
					ItemId		iid = PageGetItemId(page, op.off);

					if (!ItemIdHasStorage(iid) ||
						(Size) op.aux + op.len > ItemIdGetLength(iid))
						elog(PANIC, "pg_lion: SETBYTES past the end of item %u on block %u",
							 op.off, blkno);
					memcpy((char *) PageGetItem(page, iid) + op.aux, payload,
						   op.len);
				}
				break;

			case LION_OP_MULTIDEL:
			case LION_OP_DELETE_NC:
				{
					OffsetNumber offs[MaxIndexTuplesPerPage];
					int			n = (int) op.aux;

					if ((Size) n * sizeof(OffsetNumber) != op.len ||
						n > MaxIndexTuplesPerPage)
						elog(PANIC, "pg_lion: bad delete list on block %u", blkno);

					/* The stream is unaligned; OffsetNumber is not. */
					memcpy(offs, payload, op.len);

					if (op.op == LION_OP_MULTIDEL)
						PageIndexMultiDelete(page, offs, n);
					else
					{
						int			k;

						/*
						 * The offsets are ascending and no line pointer moves,
						 * so they stay valid as they are deleted (DESIGN.md
						 * §18: entry offsets on a directory leaf never move).
						 */
						for (k = 0; k < n; k++)
							PageIndexTupleDeleteNoCompact(page, offs[k]);
					}
				}
				break;

			case LION_OP_DELETE:
				PageIndexTupleDelete(page, op.off);
				break;

			case LION_OP_MINMAX:
				lion_page_update_minmax(page);
				break;

			case LION_OP_FLAGS:
				LionPageGetOpaque(page)->flags = op.aux;
				break;

			case LION_OP_META:
				if (op.len != sizeof(LionMetaPageData))
					elog(PANIC, "pg_lion: bad META payload on block %u", blkno);
				memcpy(LionPageGetMeta(page), payload, sizeof(LionMetaPageData));
				((PageHeader) page)->pd_lower =
					SizeOfPageHeaderData + sizeof(LionMetaPageData);
				break;

			case LION_OP_DELETED:
				{
					FullTransactionId safexid;

					if (op.len != sizeof(FullTransactionId))
						elog(PANIC, "pg_lion: bad DELETED payload on block %u",
							 blkno);
					memcpy(&safexid, payload, sizeof(FullTransactionId));
					lion_page_set_deleted(page, safexid);
				}
				break;

			case LION_OP_CONTAINER_ADD:
				{
					LionContainer *c = (LionContainer *)
						PageGetItem(page, PageGetItemId(page, op.off));

					if (!lion_container_add(c, op.aux))
						elog(PANIC, "pg_lion: could not add member %u to the item at %u on block %u",
							 op.aux, op.off, blkno);
				}
				break;

			case LION_OP_SPARSE_INS:
				{
					LionContainer *s = (LionContainer *)
						PageGetItem(page, PageGetItemId(page, op.off));
					uint32		ckey;
					bool		dup = false;

					if (op.len != sizeof(uint32))
						elog(PANIC, "pg_lion: bad SPARSE_INS payload on block %u",
							 blkno);
					memcpy(&ckey, payload, sizeof(uint32));
					if (!lion_sparse_insert(s, ckey, op.aux, &dup) || dup)
						elog(PANIC, "pg_lion: could not insert (%u,%u) into the segment at %u on block %u",
							 ckey, op.aux, op.off, blkno);
				}
				break;

			default:
				elog(PANIC, "pg_lion: unknown WAL operation %u on block %u",
					 op.op, blkno);
		}
	}
}

/*
 * Replay one record.
 *
 * Blocks are taken in the order the writer registered them, which is the
 * order it locked them (DESIGN.md §5 and §21: directory root to leaf, then
 * container pages left to right, then the new page), so replay cannot
 * deadlock against a standby backend that takes them in the same order.
 *
 * A block whose bit is set in `cleanupmask` is taken with a CLEANUP lock: it
 * is a block this record removes TIDs or items from, and DESIGN.md §11
 * requires that removal to wait for every pin.  Those blocks are always
 * registered FIRST, so the wait happens with no other buffer lock held -
 * §11's waiting rule, which applies to the startup process exactly as it
 * applies to VACUUM.
 */
static void
lion_redo(XLogReaderState *record)
{
	XLogRecPtr	lsn = record->EndRecPtr;
	uint8		info = XLogRecGetInfo(record) & ~XLR_INFO_MASK;
	xl_lion_header hdr;
	Buffer		bufs[XLR_MAX_BLOCK_ID + 1];
	int			maxblk = XLogRecMaxBlockId(record);
	int			i;

	if (info > LION_XLOG_ENTRY || (info & 0x0F) != 0)
		elog(PANIC, "pg_lion: unknown record type %u", info);

	if (XLogRecGetDataLen(record) < SizeOfLionHeader)
		elog(PANIC, "pg_lion: record has no header");
	memcpy(&hdr, XLogRecGetData(record), SizeOfLionHeader);

	for (i = 0; i <= maxblk; i++)
		bufs[i] = InvalidBuffer;

	for (i = 0; i <= maxblk; i++)
	{
		bool		cleanup = (hdr.cleanupmask & (1 << i)) != 0;
		bool		willinit = (hdr.initmask & (1 << i)) != 0;
		ReadBufferMode mode;
		XLogRedoAction action;
		Buffer		buf;

		if (!XLogRecHasBlockRef(record, i))
			continue;

		if (willinit)
			mode = cleanup ? RBM_ZERO_AND_CLEANUP_LOCK : RBM_ZERO_AND_LOCK;
		else
			mode = RBM_NORMAL;

		action = XLogReadBufferForRedoExtended(record, (uint8) i, mode,
											   cleanup, &buf);
		bufs[i] = buf;

		if (action == BLK_NEEDS_REDO)
		{
			Page		page = BufferGetPage(buf);
			Size		len = 0;
			char	   *data = XLogRecGetBlockData(record, (uint8) i, &len);
			BlockNumber blkno = InvalidBlockNumber;

			XLogRecGetBlockTag(record, (uint8) i, NULL, NULL, &blkno);

			if (data == NULL && len != 0)
				elog(PANIC, "pg_lion: block %d has no data", i);
			if (data != NULL)
				lion_redo_apply(page, data, len, blkno);

			PageSetLSN(page, lsn);
			MarkBufferDirty(buf);
		}
	}

	for (i = maxblk; i >= 0; i--)
	{
		if (BufferIsValid(bufs[i]))
			UnlockReleaseBuffer(bufs[i]);
	}
}

/* ---------------------------------------------------------------------
 * pg_waldump / pg_walinspect
 * --------------------------------------------------------------------- */

static const char *
lion_op_name(uint8 op)
{
	switch (op)
	{
		case LION_OP_INIT:
			return "init";
		case LION_OP_SPECIAL:
			return "special";
		case LION_OP_ADD:
			return "add";
		case LION_OP_ADDMANY:
			return "addmany";
		case LION_OP_REPLACE:
			return "replace";
		case LION_OP_SETBYTES:
			return "setbytes";
		case LION_OP_MULTIDEL:
			return "multidel";
		case LION_OP_DELETE_NC:
			return "delete_nc";
		case LION_OP_DELETE:
			return "delete";
		case LION_OP_MINMAX:
			return "minmax";
		case LION_OP_FLAGS:
			return "flags";
		case LION_OP_META:
			return "meta";
		case LION_OP_DELETED:
			return "deleted";
		case LION_OP_CONTAINER_ADD:
			return "container_add";
		case LION_OP_SPARSE_INS:
			return "sparse_ins";
		case LION_OP_DELTA:
			return "delta";
		default:
			return "?";
	}
}

static void
lion_desc(StringInfo buf, XLogReaderState *record)
{
	xl_lion_header hdr;
	int			maxblk = XLogRecMaxBlockId(record);
	int			i;

	if (XLogRecGetDataLen(record) < SizeOfLionHeader)
	{
		appendStringInfoString(buf, "(no header)");
		return;
	}
	memcpy(&hdr, XLogRecGetData(record), SizeOfLionHeader);

	appendStringInfo(buf, "init %u, cleanup %u", hdr.initmask, hdr.cleanupmask);

	for (i = 0; i <= maxblk; i++)
	{
		Size		len = 0;
		char	   *data;
		Size		pos = 0;
		bool		first = true;

		if (!XLogRecHasBlockRef(record, i))
			continue;

		data = XLogRecGetBlockData(record, (uint8) i, &len);
		appendStringInfo(buf, "; blk %d:", i);
		if (data == NULL)
		{
			appendStringInfoString(buf, " fpi");
			continue;
		}

		while (pos + SizeOfLionOp <= len)
		{
			xl_lion_op	op;

			memcpy(&op, data + pos, SizeOfLionOp);
			pos += SizeOfLionOp;
			if (pos + op.len > len)
				break;
			pos += op.len;
			appendStringInfo(buf, "%s %s off %u aux %u len %u",
							 first ? "" : ",", lion_op_name(op.op),
							 op.off, op.aux, op.len);
			first = false;
		}
	}
}

static const char *
lion_identify(uint8 info)
{
	switch (info & ~XLR_INFO_MASK)
	{
		case LION_XLOG_ITEM_SET:
			return "ITEM_SET";
		case LION_XLOG_ITEM_REPLACE:
			return "ITEM_REPLACE";
		case LION_XLOG_ITEM_ADD:
			return "ITEM_ADD";
		case LION_XLOG_ITEM_DELETE:
			return "ITEM_DELETE";
		case LION_XLOG_PAGE_INIT:
			return "PAGE_INIT";
		case LION_XLOG_SPLIT:
			return "SPLIT";
		case LION_XLOG_DOWNLINK:
			return "DOWNLINK";
		case LION_XLOG_SPLIT_CLEAR:
			return "SPLIT_CLEAR";
		case LION_XLOG_META:
			return "META";
		case LION_XLOG_VACUUM_PAGE:
			return "VACUUM_PAGE";
		case LION_XLOG_PAGE_DELETED:
			return "PAGE_DELETED";
		case LION_XLOG_ENTRY:
			return "ENTRY";
		default:
			return NULL;
	}
}

/*
 * The MAXALIGN padding after each item, which nothing logs.
 *
 * PageAddItemExtended() reserves MAXALIGN(size) bytes for an item and copies
 * `size` of them; PageIndexTupleOverwrite() does the same.  The bytes in
 * between keep whatever the page held there before, which is how core's own
 * access methods behave - and core does not care, because a core index tuple
 * is MAXALIGNed by index_form_tuple() and there is no padding.  A lion entry
 * tuple is not: its size is MAXALIGN(header + key) plus the INLINE payload,
 * which is a byte count, and a container item's logical size is derived from
 * its header.  So an item of 54 bytes occupies 56 and two bytes of the page's
 * former contents survive underneath it.
 *
 * On the primary those two bytes are whatever the page held; on a standby the
 * page may have arrived as a full-page image, whose HOLE (pd_lower to
 * pd_upper, which is where those bytes were) is restored as ZEROES.  The two
 * pages then differ in bytes no record ever described and no reader ever
 * reads.  That is exactly what DESIGN.md §25 meant by masking "the slack bytes
 * inside items, which are not logged", and wal_consistency_checking found it
 * the first time it was run: a 54-byte ENTRY record on a directory leaf.
 *
 * The walk is defensive because rm_mask() is handed a page from the WAL and
 * from a buffer, either of which may be a page kind that has no line pointers
 * at all: a meta page and a DELETED page both carry their payload in
 * pd_lower's range, so PageGetMaxOffsetNumber() would read it as a line
 * pointer array.
 */
static void
lion_mask_item_padding(Page page)
{
	PageHeader	phdr = (PageHeader) page;
	LionPageOpaque opaque;
	OffsetNumber maxoff;
	OffsetNumber off;

	if (PageIsNew(page) || PageGetSpecialSize(page) != LION_SPECIAL_SIZE)
		return;
	opaque = LionPageGetOpaque(page);
	if (opaque->page_id != LION_PAGE_ID)
		return;
	if ((opaque->flags & (LION_PAGE_BUCKET | LION_PAGE_DIR |
						  LION_PAGE_CONTAINER)) == 0)
		return;					/* the meta page holds no items */
	if ((opaque->flags & LION_PAGE_DELETED) != 0)
		return;					/* nor does a freed one; pd_lower is its body */

	maxoff = PageGetMaxOffsetNumber(page);
	for (off = FirstOffsetNumber; off <= maxoff; off++)
	{
		ItemId		iid = PageGetItemId(page, off);
		Size		len,
					alen,
					start;

		if (!ItemIdHasStorage(iid))
			continue;
		len = ItemIdGetLength(iid);
		alen = MAXALIGN(len);
		start = ItemIdGetOffset(iid);
		if (alen == len)
			continue;
		if (start < (Size) phdr->pd_upper ||
			start + alen > (Size) phdr->pd_special)
			continue;			/* corrupt: leave it for the comparison to say so */
		memset((char *) page + start + len, MASK_MARKER, alen - len);
	}
}

/*
 * What wal_consistency_checking must not compare.
 *
 * The page header's LSN and checksum, the hint bits, the free space between
 * pd_lower and pd_upper - which holds whatever the last compaction left
 * behind and is never logged - and the alignment padding after each item, for
 * the reason above.
 *
 * The growth SLACK inside an item (DESIGN.md §4) is NOT masked and must not
 * be: an item is always logged at its ALLOCATED length with its slack already
 * zeroed (lion_item_zero_slack()), and the three operations that change an
 * item without rewriting it - CONTAINER_ADD, SPARSE_INS, SETBYTES - leave
 * every byte outside the range they name untouched on the primary and on the
 * standby alike.  Masking it would hide a real divergence.
 */
static void
lion_mask(char *pagedata, BlockNumber blkno)
{
	Page		page = (Page) pagedata;

	mask_page_lsn_and_checksum(page);
	mask_page_hint_bits(page);
	mask_unused_space(page);
	lion_mask_item_padding(page);
}

static const RmgrData lion_rmgr = {
	.rm_name = LION_RMGR_NAME,
	.rm_redo = lion_redo,
	.rm_desc = lion_desc,
	.rm_identify = lion_identify,
	.rm_startup = NULL,
	.rm_cleanup = NULL,
	.rm_mask = lion_mask,
	.rm_decode = NULL
};

/*
 * Register the resource manager, from _PG_init and only while
 * shared_preload_libraries is being processed: RegisterCustomRmgr() refuses
 * any later call, because a record whose manager is not loaded cannot be
 * replayed.
 *
 * The id comes from a GUC because 128 (RM_EXPERIMENTAL_ID) is the id the
 * PostgreSQL project reserves for development, and two extensions that both
 * take it cannot be loaded together.  A release of this extension has to
 * either reserve an id on the wiki page the core documentation names
 * (https://wiki.postgresql.org/wiki/CustomWALResourceManagers) or keep the
 * GUC and document it; the GUC stays either way, because the whole point of
 * a reserved range is that a site can move out of a collision.
 *
 * Once an index in this cluster has been written in rmgr mode, the library
 * must stay in shared_preload_libraries for as long as WAL that mentions it
 * may still be replayed - the same rule the core documentation states for
 * every custom resource manager.
 */
void
lion_wal_init(void)
{
	/*
	 * Everything here is PGC_POSTMASTER, which core refuses to define once
	 * the postmaster is up, so a library that is merely LOADed on demand has
	 * to leave it alone - pg_stat_statements returns from _PG_init() at
	 * exactly this point and for exactly this reason.  Without the preload
	 * there is no resource manager either, and every index in this server is
	 * then written in generic mode.
	 */
	if (!process_shared_preload_libraries_in_progress)
		return;

	DefineCustomIntVariable("pg_lion.rmgr_id",
							"WAL resource manager id pg_lion registers under.",
							"Only read while the library is preloaded. The default, 128, is the id reserved for development; a cluster that loads two extensions wanting it has to move one of them.",
							&lion_rmgr_id,
							RM_EXPERIMENTAL_ID,
							RM_MIN_CUSTOM_ID, RM_MAX_CUSTOM_ID,
							PGC_POSTMASTER,
							0,
							NULL, NULL, NULL);

	RegisterCustomRmgr((RmgrId) lion_rmgr_id, &lion_rmgr);
	lion_rmgr_is_registered = true;
}

/*
 * The wal_mode a new index is built with (DESIGN.md §25).
 *
 * The reloption decides when it names a mode; "auto", which is the default,
 * means the resource manager when this server has it and generic when it has
 * not.  That is what lets one CREATE INDEX script work on a cluster that
 * preloads this library and on one that does not, and it is also why a
 * cluster can hold indexes of both kinds at once: the mode is a property of
 * the INDEX, fixed at build time, and REINDEX is what changes it.
 */
uint32
lion_wal_mode_for_build(Relation index)
{
	LionOptions *opts = (LionOptions *) index->rd_options;
	int			want = opts ? opts->wal_mode : LION_WALOPT_AUTO;

	if (want == LION_WALOPT_RMGR && !lion_rmgr_is_registered)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("lion index \"%s\" asks for wal_mode = rmgr, which this server has not registered",
						RelationGetRelationName(index)),
				 errhint("Add \"pg_lion\" to shared_preload_libraries and restart the server, or leave wal_mode at \"auto\".")));

	if (want == LION_WALOPT_GENERIC)
		return LION_WAL_MODE_GENERIC;
	if (want == LION_WALOPT_RMGR)
		return LION_WAL_MODE_RMGR;

	return lion_rmgr_is_registered ? LION_WAL_MODE_RMGR : LION_WAL_MODE_GENERIC;
}
