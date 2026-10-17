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
 * it and reads apart through the ops below.  One over a list the reader
 * knows whole (lion_readahead_begin_list()) has no ring: its callback is
 * the reader's next block, and its reads take the stream's buffers in
 * order.  Every pin either holds - the stream's, and the ring's held
 * buffers - is the current resource owner's like any other, so an error
 * releases them with the transaction or the portal it ends; a reader that
 * goes on ends or resets it (lion_readahead_end(), lion_readahead_reset())
 * before it stops, as an index scan does at endscan and rescan.  The
 * callback only takes a block from memory, or has the reader's refill or
 * next take one from its own queues or list, so it never blocks, and from
 * 18 the stream may batch its I/O (READ_STREAM_USE_BATCHING).
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
	MemoryContext cxt;			/* where it lives, and its stream */
	Relation	rel;
	int			mode;			/* LION_READAHEAD_* as it reads, which may be
								 * less than asked (lion_readahead_begin()) */
	LionAsk    *ask;			/* LION_READAHEAD_FADVISE: the relation's
								 * asks */
	bool		ownask;			/* ... made for this one alone */
	LionReadaheadRefill refill;
	void	   *refill_arg;
	LionReadaheadNext next;		/* a list's (lion_readahead_begin_list()),
								 * which has no ring */
	void	   *next_arg;
#if PG_VERSION_NUM >= 170000
	ReadStream *stream;			/* LION_READAHEAD_STREAM */
#endif
	LionRing	ring;
	void	   *ringmem;
	int64		nread;			/* blocks its reads brought in */
	int64		nreadall;		/* ... all told, and when its stream last
								 * started */
	int64		nreadstart;
	int			warm;			/* reads in a row that read nothing in */
	int			dormant;		/* reads to make without pushing */
	int64		nlisted;		/* a list's blocks named to the stream ... */
	int64		nreturned;		/* ... and handed to the reader */
	int64		nrested;		/* ... and taken by it apart, resting */
	int64		nwoken;			/* ... and the times its stream started
								 * again after a rest */
	bool		paused;			/* a list's callback named no block for a
								 * rest, which ended its stream ... */
	bool		drained;		/* ... and the stream has handed out every
								 * block named to it before */
	LionReadaheadCounts taken;	/* the counts as last taken */
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

/*
 * A ring's stream, as far ahead at its start as for a whole relation
 * (READ_STREAM_FULL): a full I/O's blocks, io_combine_limit, rather than
 * one, since what the reader pushed it will mostly read.
 */
static ReadStream *
lion_readahead_ring_stream(LionReadahead *s)
{
	return read_stream_begin_relation(LION_READAHEAD_FLAGS | READ_STREAM_FULL,
									  NULL, s->rel, MAIN_FORKNUM,
									  lion_readahead_cb, s, 0);
}

/*
 * The ring's stream, started over (LionRingOps.restart).  Core's reset
 * starts it from one block ahead, and its distance doubles only as the
 * reader waits on its reads: so a stream that read anything in since it
 * last started, which is cold, is begun anew, from a full I/O ahead; one
 * that found every block in shared buffers is reset, which costs less.
 */
static void
lion_readahead_op_restart(void *arg)
{
	LionReadahead *s = (LionReadahead *) arg;

	if (s->nreadall > s->nreadstart)
	{
		ReadStream *old = s->stream;
		MemoryContext oldcxt;

		/* in the readahead's context, not the reader's of the moment */
		s->stream = NULL;
		read_stream_end(old);
		oldcxt = MemoryContextSwitchTo(s->cxt);
		s->stream = lion_readahead_ring_stream(s);
		MemoryContextSwitchTo(oldcxt);
	}
	else
		read_stream_reset(s->stream);
	s->nreadstart = s->nreadall;
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

	return s->refill != NULL && s->dormant == 0 && s->refill(s->refill_arg);
}

static void
lion_readahead_op_fail(void *arg, const char *what)
{
	LionReadahead *s = (LionReadahead *) arg;

	elog(ERROR, "lion read stream of \"%s\": %s",
		 RelationGetRelationName(s->rel), what);
}

/*
 * A list's callback: the reader's next block, counted; none while the list
 * rests (lion_readahead_next()), which ends the stream once it has handed
 * out what it was named, the list's next blocks left to the reader.
 */
static BlockNumber
lion_readahead_list_cb(ReadStream *stream, void *callback_private_data,
					   void *per_buffer_data)
{
	LionReadahead *s = (LionReadahead *) callback_private_data;
	BlockNumber blk;

	if (s->dormant > 0)
	{
		s->paused = true;
		return InvalidBlockNumber;
	}
	blk = s->next(s->next_arg);
	if (BlockNumberIsValid(blk))
		s->nlisted++;
	return blk;
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
	s->cxt = CurrentMemoryContext;
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
		s->stream = lion_readahead_ring_stream(s);
	}
#endif
	return s;
}

/*
 * A plain read stream over a list the reader knows whole and reads in
 * order, every block of it once (DESIGN.md §44.4, "Read streams"): next
 * names each, inside the stream's callback, and lion_readahead_next()
 * returns them in that order, pinned unless the list rests.  Core's stream
 * keeps as many pins as its own limits allow (GetPinLimit()), as it does
 * for a bitmap heap scan, and starts as far ahead as it would for a whole
 * relation (READ_STREAM_FULL), the reader to read every block named.  Only
 * for LION_READAHEAD_STREAM, from 17: otherwise NULL, and the reader asks
 * and reads as the mode says (lion_ask(), ReadBuffer()).
 */
LionReadahead *
lion_readahead_begin_list(Relation rel, int mode, LionReadaheadNext next,
						  void *next_arg)
{
#if PG_VERSION_NUM >= 170000
	LionReadahead *s;

	Assert(next != NULL);
	if (mode != LION_READAHEAD_STREAM)
		return NULL;
	s = palloc0(sizeof(LionReadahead));
	s->cxt = CurrentMemoryContext;
	s->rel = rel;
	s->mode = mode;
	s->next = next;
	s->next_arg = next_arg;
	s->stream = read_stream_begin_relation(LION_READAHEAD_FLAGS |
										   READ_STREAM_FULL, NULL, rel,
										   MAIN_FORKNUM,
										   lion_readahead_list_cb, s, 0);
	return s;
#else
	return NULL;
#endif
}

/*
 * The list's next block, into *blk, and its buffer, pinned and not locked,
 * into *buf; false past the list's end.  A list rests as a ring's stream
 * does (lion_readahead_dormant()): after LION_ASK_WARM blocks in a row that
 * its stream read nothing in for, the next LION_ASK_DORMANT are the
 * reader's to read - *buf InvalidBuffer, once the stream has handed out
 * the blocks it was named before - and the stream then starts again from
 * the list's next block.  Every warm block it hands out is a lookup in
 * shared buffers and a pin that the reader's own read repeats.  A stream
 * far ahead may hold more blocks than a rest lasts: they come out first,
 * in order, through the reads between rests too, and only a stream the
 * rest ended (paused) and that has handed out every block it was named is
 * started again; one that ended at the list's end is not.
 */
bool
lion_readahead_next(LionReadahead *s, BlockNumber *blk, Buffer *buf)
{
#if PG_VERSION_NUM >= 170000
	int64		before;
	Buffer		b = InvalidBuffer;

	if (s->next == NULL || s->stream == NULL)
		elog(ERROR, "lion read stream of \"%s\" has no list",
			 RelationGetRelationName(s->rel));
	*blk = InvalidBlockNumber;
	*buf = InvalidBuffer;
	before = lion_readahead_blks_read();
	if (s->dormant > 0)
	{
		s->dormant--;
		/* what the stream was named before the rest comes first, in order */
		if (!s->drained)
		{
			b = read_stream_next_buffer(s->stream, NULL);
			s->nread += lion_readahead_blks_read() - before;
			if (!BufferIsValid(b))
				s->drained = true;
		}
		if (!BufferIsValid(b))
		{
			Assert(s->nreturned == s->nlisted);
			*blk = s->next(s->next_arg);
			if (!BlockNumberIsValid(*blk))
				return false;
			s->nrested++;
			return true;
		}
	}
	else
	{
		b = read_stream_next_buffer(s->stream, NULL);
		if (!BufferIsValid(b) && s->paused)
		{
			/*
			 * Awake, and the stream, which the rest ended, has handed out
			 * what it was named: started again from the list's next block.
			 */
			Assert(s->nreturned == s->nlisted);
			read_stream_reset(s->stream);
			s->paused = s->drained = false;
			s->nwoken++;
			b = read_stream_next_buffer(s->stream, NULL);
		}
		before = lion_readahead_blks_read() - before;
		s->nread += before;
		if (BufferIsValid(b))
		{
			/* a rest after LION_ASK_WARM blocks in a row read nothing in */
			if (before > 0)
				s->warm = 0;
			else if (++s->warm >= LION_ASK_WARM)
			{
				s->warm = 0;
				s->dormant = LION_ASK_DORMANT;
			}
		}
	}
	if (!BufferIsValid(b))
	{
		/* past the list's end */
		Assert(s->nreturned == s->nlisted);
		return false;
	}
	s->nreturned++;
	Assert(s->nreturned <= s->nlisted);
	*blk = BufferGetBlockNumber(b);
	*buf = b;
	return true;
#else
	elog(ERROR, "lion read stream of \"%s\" has no list",
		 RelationGetRelationName(s->rel));
	return false;
#endif
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
	Assert(s->next == NULL);
	switch (s->mode)
	{
		case LION_READAHEAD_FADVISE:
			lion_ask(s->ask, s->rel, blk, once);
			taken = true;
			break;
		case LION_READAHEAD_STREAM:
			if (s->dormant > 0)
				break;
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

	Assert(s->next == NULL);
	if (s->mode != LION_READAHEAD_STREAM)
		return ReadBuffer(s->rel, blk);
	before = lion_readahead_blks_read();
	buf = (Buffer) lion_ring_read(&s->ring, blk);
	before = lion_readahead_blks_read() - before;
	s->nread += before;
	s->nreadall += before;
	if (before > 0)
		s->warm = s->dormant = 0;
	else if (s->dormant > 0)
		s->dormant--;
	else if (++s->warm >= LION_ASK_WARM)
	{
		s->warm = 0;
		s->dormant = LION_ASK_DORMANT;
	}
	lion_readahead_check(s);
	Assert(BufferIsValid(buf) && BufferGetBlockNumber(buf) == blk);
	return buf;
}

/*
 * Whether a stream is resting (LION_READAHEAD_STREAM over a ring): its
 * last LION_ASK_WARM reads in a row read nothing in, every block they
 * wanted in shared buffers already, so for the next LION_ASK_DORMANT reads,
 * or until one reads a block in, nothing is pushed and nothing refilled,
 * and a read of a block the stream does not hold is ReadBuffer()'s - a warm
 * stream's bookkeeping and the reader's looks into its queues being work
 * for nothing.  The reader may skip its looks meanwhile.  Asks of the
 * kernel have their own rest (lion_ask()), which sees only whether
 * PrefetchBuffer() found the page; a stream sees its reads.
 */
bool
lion_readahead_dormant(const LionReadahead *s)
{
	return s->mode == LION_READAHEAD_STREAM && s->next == NULL &&
		s->dormant > 0;
}

/* Forget how warm its reads were: a reader's pages that are always warm. */
void
lion_readahead_awaken(LionReadahead *s)
{
	s->warm = s->dormant = 0;
}

/* Blocks pushed or given and not read: how far ahead the stream reaches. */
int
lion_readahead_depth(const LionReadahead *s)
{
	if (s->mode != LION_READAHEAD_STREAM)
		return 0;
	if (s->next != NULL)
		return (int) (s->nlisted - s->nreturned);
	return lion_ring_ahead(&s->ring);
}

/*
 * What it has done since the counts were last taken, into c (added).  A
 * list's blocks named to the stream and not read yet count as released
 * unread, which they are once it ends: so the counts taken when the reader
 * is done, as they should be, hold what its end will release.
 */
void
lion_readahead_counts(LionReadahead *s, LionReadaheadCounts *c)
{
	LionRing   *r = &s->ring;

	if (s->mode != LION_READAHEAD_STREAM)
		return;
	if (s->next != NULL)
	{
		int64		ahead = s->nlisted - s->nreturned;

		c->nstreamed += s->nreturned - s->taken.nstreamed;
		c->napart += s->nrested - s->taken.napart;
		c->nwasted += ahead - s->taken.nwasted;
		c->nrestarts += s->nwoken - s->taken.nrestarts;
		c->nread += s->nread;
		s->taken.nstreamed = s->nreturned;
		s->taken.napart = s->nrested;
		s->taken.nwasted = ahead;
		s->taken.nrestarts = s->nwoken;
		s->nread = 0;
		return;
	}
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
	if (s->next != NULL)
	{
		/*
		 * A list's: what the stream read ahead is dropped, counted unread,
		 * and the list goes on past it.
		 */
#if PG_VERSION_NUM >= 170000
		read_stream_reset(s->stream);
#endif
		s->taken.nwasted -= s->nlisted - s->nreturned;
		s->nlisted = s->nreturned;
		s->warm = s->dormant = 0;
		s->paused = s->drained = false;
		return;
	}
	lion_ring_clear(&s->ring);
	lion_readahead_check(s);
}

/* Reset, and free. */
void
lion_readahead_end(LionReadahead *s)
{
	/* the stream reset, not begun anew only to be ended */
	s->nreadstart = s->nreadall;
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
