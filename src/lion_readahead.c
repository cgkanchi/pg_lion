/*-------------------------------------------------------------------------
 *
 * lion_readahead.c
 *		Reading a relation's blocks in an order its reader predicts: a read
 *		stream over the blocks it pushes (17 and later), asks of the kernel
 *		ahead of its reads, or plain reads (lion_readahead.h; DESIGN.md §44.4,
 *		"Read streams").
 *
 * A LionReadahead of LION_READAHEAD_STREAM binds a LionRing
 * (lion_readahead_ring.c) to core's read stream: the stream's callback takes
 * the ring's next block, and the ring pulls the stream's buffers, restarts
 * it and reads apart through the ops below.  Every pin it holds - the
 * stream's, and the ring's held buffers - is the current resource owner's
 * like any other, so an error releases them with the transaction or the
 * portal it ends; a reader that goes on ends or resets it
 * (lion_readahead_end(), lion_readahead_reset()) before it stops, as an
 * index scan does at endscan and rescan.  The callback only takes a block from memory, or has the
 * reader's refill push more from its own queues, so it never blocks, and
 * from 18 the stream may batch its I/O (READ_STREAM_USE_BATCHING).
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "common/hashfn.h"
#include "executor/instrument.h"
#include "miscadmin.h"
#include "storage/bufmgr.h"
#if PG_VERSION_NUM >= 170000
#include "storage/read_stream.h"
#endif
#include "utils/rel.h"

#include "lion_compat.h"
#include "lion_readahead.h"

struct LionReadahead
{
	Relation	rel;
	int			mode;			/* LION_READAHEAD_* as it reads, which may be
								 * less than asked (lion_readahead_begin()) */
	LionAsk    *ask;			/* LION_READAHEAD_FADVISE: the relation's
								 * asks */
	bool		ownask;			/* ... made for this one alone */
	LionReadaheadRefill refill;
	void	   *refill_arg;
#if PG_VERSION_NUM >= 170000
	ReadStream *stream;			/* LION_READAHEAD_STREAM */
#endif
	LionRing	ring;
	void	   *ringmem;
	int64		nread;			/* blocks its reads brought in */
	LionReadaheadCounts taken;	/* the ring's counts as last taken */
};

/* ----------------------------------------------------------------
 * Asks of the kernel (LION_READAHEAD_FADVISE; DESIGN.md §44.4, "Prefetch")
 * ----------------------------------------------------------------
 */

/* Forget what a relation's asks found: the next asks are made. */
void
lion_ask_reset(LionAsk *a)
{
	memset(a->seen, 0xFF, sizeof(a->seen));
	a->warm = 0;
	a->dormant = 0;
}

/*
 * Ask the kernel for a block a read will want soon: PrefetchBuffer(), which
 * pins nothing and is posix_fadvise() under it, read by an I/O worker or
 * io_uring alike from 18 on.  While the relation's asks are dormant
 * (LionAsk) the ask is let pass, and with once a block in its table of
 * blocks asked for lately is skipped.  The caller keeps blk below the
 * relation's size as it read it: past the last segment md.c would fail
 * rather than ignore it.
 */
void
lion_ask(LionAsk *a, Relation rel, BlockNumber blk, bool once)
{
	PrefetchBufferResult r;

	if (a->dormant > 0)
	{
		a->dormant--;
		return;
	}
	if (once)
	{
		uint32		slot = murmurhash32(blk) & (LION_ASK_SLOTS - 1);

		if (a->seen[slot] == blk)
			return;
		a->seen[slot] = blk;
	}
	r = PrefetchBuffer(rel, MAIN_FORKNUM, blk);
	a->nasked++;
	if (r.initiated_io)
	{
		a->nmissing++;
		a->warm = 0;
	}
	else if (++a->warm >= LION_ASK_WARM)
	{
		a->warm = 0;
		a->dormant = LION_ASK_DORMANT;
	}
}

/* ----------------------------------------------------------------
 * The ring's side of core's read stream (LION_READAHEAD_STREAM)
 * ----------------------------------------------------------------
 */

/* The blocks this backend has read in so far, for the counts. */
static inline int64
lion_readahead_blks_read(void)
{
	return pgBufferUsage.shared_blks_read + pgBufferUsage.local_blks_read;
}

#if PG_VERSION_NUM >= 170000

/*
 * From 18 the stream may stage its reads and submit them together: its
 * callback, lion_ring_give() and the reader's refill, takes blocks from
 * memory and neither waits nor reads.
 */
#if PG_VERSION_NUM >= 180000
#define LION_READAHEAD_FLAGS	(READ_STREAM_DEFAULT | READ_STREAM_USE_BATCHING)
#else
#define LION_READAHEAD_FLAGS	READ_STREAM_DEFAULT
#endif

static BlockNumber
lion_readahead_cb(ReadStream *stream, void *callback_private_data,
				  void *per_buffer_data)
{
	LionReadahead *s = (LionReadahead *) callback_private_data;

	return lion_ring_give(&s->ring);
}

static LionRingBuf
lion_readahead_op_next(void *arg)
{
	LionReadahead *s = (LionReadahead *) arg;

	return (LionRingBuf) read_stream_next_buffer(s->stream, NULL);
}

static void
lion_readahead_op_restart(void *arg)
{
	LionReadahead *s = (LionReadahead *) arg;

	read_stream_reset(s->stream);
}

static LionRingBuf
lion_readahead_op_direct(void *arg, BlockNumber blk)
{
	LionReadahead *s = (LionReadahead *) arg;

	return (LionRingBuf) ReadBuffer(s->rel, blk);
}

static void
lion_readahead_op_release(void *arg, LionRingBuf buf)
{
	ReleaseBuffer((Buffer) buf);
}

static BlockNumber
lion_readahead_op_block(void *arg, LionRingBuf buf)
{
	return BufferGetBlockNumber((Buffer) buf);
}

static bool
lion_readahead_op_refill(void *arg)
{
	LionReadahead *s = (LionReadahead *) arg;

	return s->refill != NULL && s->refill(s->refill_arg);
}

static void
lion_readahead_op_fail(void *arg, const char *what)
{
	LionReadahead *s = (LionReadahead *) arg;

	elog(ERROR, "lion read stream of \"%s\": %s",
		 RelationGetRelationName(s->rel), what);
}

static const LionRingOps lion_readahead_ops = {
	.next = lion_readahead_op_next,
	.restart = lion_readahead_op_restart,
	.direct = lion_readahead_op_direct,
	.release = lion_readahead_op_release,
	.block = lion_readahead_op_block,
	.refill = lion_readahead_op_refill,
	.fail = lion_readahead_op_fail
};
#endif							/* PG_VERSION_NUM >= 170000 */

/*
 * The whole ring checked, in assert-enabled builds, after everything that
 * changes it: a few microseconds a read at the largest ring.
 */
static inline void
lion_readahead_check(const LionReadahead *s)
{
#ifdef USE_ASSERT_CHECKING
	if (s->mode == LION_READAHEAD_STREAM)
	{
		const char *bad = lion_ring_check(&s->ring);

		if (bad != NULL)
			elog(ERROR, "lion read stream of \"%s\" is inconsistent: %s",
				 RelationGetRelationName(s->rel), bad);
	}
#endif
}

/* ----------------------------------------------------------------
 * The interface
 * ----------------------------------------------------------------
 */

/*
 * A readahead of rel's main fork that reads as mode says, in the current
 * memory context.  LION_READAHEAD_STREAM holds at most pins pins - those of
 * the blocks its read stream reads ahead and those it holds for their reads
 * - and as many blocks pending; on 16, which has no read streams, or with
 * fewer than LION_READAHEAD_MIN_PINS, it asks the kernel instead.  ask is
 * the relation's asks of the kernel (LION_READAHEAD_FADVISE), which several
 * readaheads may share; NULL gives this one its own.  refill, if given, may
 * push more blocks when the read stream wants one and none is pending
 * (LionRingOps).
 */
LionReadahead *
lion_readahead_begin(Relation rel, int mode, int pins, LionAsk *ask,
					 LionReadaheadRefill refill, void *refill_arg)
{
	LionReadahead *s = palloc0(sizeof(LionReadahead));

	Assert(mode == LION_READAHEAD_OFF || mode == LION_READAHEAD_FADVISE ||
		   mode == LION_READAHEAD_STREAM);
	s->rel = rel;
	s->refill = refill;
	s->refill_arg = refill_arg;
#if PG_VERSION_NUM < 170000
	if (mode == LION_READAHEAD_STREAM)
		mode = LION_READAHEAD_FADVISE;
#endif
	if (mode == LION_READAHEAD_STREAM && pins < LION_READAHEAD_MIN_PINS)
		mode = LION_READAHEAD_FADVISE;
	if (mode == LION_READAHEAD_FADVISE && ask == NULL)
	{
		ask = palloc(sizeof(LionAsk));
		lion_ask_reset(ask);
		ask->nasked = ask->nmissing = 0;
		s->ownask = true;
	}
	s->ask = ask;
	s->mode = mode;
#if PG_VERSION_NUM >= 170000
	if (mode == LION_READAHEAD_STREAM)
	{
		s->ringmem = palloc(lion_ring_memsize(pins, pins));
		lion_ring_init(&s->ring, pins, pins, s->ringmem,
					   &lion_readahead_ops, s);
		s->stream = read_stream_begin_relation(LION_READAHEAD_FLAGS, NULL, rel,
											   MAIN_FORKNUM, lion_readahead_cb,
											   s, 0);
	}
#endif
	return s;
}

/* How it reads: the mode asked of lion_readahead_begin(), or less. */
int
lion_readahead_mode(const LionReadahead *s)
{
	return s->mode;
}

/*
 * The reader expects to read blk: a stream pushes it, urgent if the reader
 * is in the middle of a run of which this is the next, predicted if not
 * (lion_ring_push()); LION_READAHEAD_FADVISE asks the kernel for it, once
 * skipping a block asked for lately (lion_ask()).  The caller keeps blk
 * below the relation's size as it last counted it.  True if it was pushed
 * or asked for.
 */
bool
lion_readahead_ask(LionReadahead *s, BlockNumber blk, bool urgent, bool once)
{
	bool		taken = false;

	Assert(BlockNumberIsValid(blk));
	switch (s->mode)
	{
		case LION_READAHEAD_FADVISE:
			lion_ask(s->ask, s->rel, blk, once);
			taken = true;
			break;
		case LION_READAHEAD_STREAM:
			taken = lion_ring_push(&s->ring, blk, urgent);
			lion_readahead_check(s);
			break;
	}
	return taken;
}

/* Read blk, pinned and not locked, as ReadBuffer() would. */
Buffer
lion_readahead_read(LionReadahead *s, BlockNumber blk)
{
	int64		before;
	Buffer		buf;

	if (s->mode != LION_READAHEAD_STREAM)
		return ReadBuffer(s->rel, blk);
	before = lion_readahead_blks_read();
	buf = (Buffer) lion_ring_read(&s->ring, blk);
	s->nread += lion_readahead_blks_read() - before;
	lion_readahead_check(s);
	Assert(BufferIsValid(buf) && BufferGetBlockNumber(buf) == blk);
	return buf;
}

/* Whether a read of blk would come from the stream. */
bool
lion_readahead_tracks(const LionReadahead *s, BlockNumber blk)
{
	return s->mode == LION_READAHEAD_STREAM && lion_ring_tracks(&s->ring, blk);
}

/* Blocks pushed or given and not read: how far ahead the stream reaches. */
int
lion_readahead_depth(const LionReadahead *s)
{
	return s->mode == LION_READAHEAD_STREAM ? lion_ring_ahead(&s->ring) : 0;
}

/* What it has done since the counts were last taken, into c (added). */
void
lion_readahead_counts(LionReadahead *s, LionReadaheadCounts *c)
{
	LionRing   *r = &s->ring;

	if (s->mode != LION_READAHEAD_STREAM)
		return;
	c->nstreamed += (int64) r->nstreamed - s->taken.nstreamed;
	c->napart += (int64) r->napart - s->taken.napart;
	c->nwasted += (int64) r->nwasted - s->taken.nwasted;
	c->nrestarts += (int64) r->nrestarts - s->taken.nrestarts;
	c->nread += s->nread;
	s->taken.nstreamed = (int64) r->nstreamed;
	s->taken.napart = (int64) r->napart;
	s->taken.nwasted = (int64) r->nwasted;
	s->taken.nrestarts = (int64) r->nrestarts;
	s->nread = 0;
}

/*
 * Forget every block pushed and release every pin held: the stream holds
 * nothing, and is ready for other blocks.
 */
void
lion_readahead_reset(LionReadahead *s)
{
	if (s->mode != LION_READAHEAD_STREAM)
		return;
	lion_ring_clear(&s->ring);
	lion_readahead_check(s);
}

/* Reset, and free. */
void
lion_readahead_end(LionReadahead *s)
{
	lion_readahead_reset(s);
#if PG_VERSION_NUM >= 170000
	if (s->stream != NULL)
		read_stream_end(s->stream);
#endif
	if (s->ringmem != NULL)
		pfree(s->ringmem);
	if (s->ownask)
		pfree(s->ask);
	pfree(s);
}
