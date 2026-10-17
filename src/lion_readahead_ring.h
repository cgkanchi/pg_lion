/*-------------------------------------------------------------------------
 * lion_readahead_ring.h
 *	  The bookkeeping of a read stream over blocks a reader predicts
 *	  (DESIGN.md §44.4, "Read streams"): which blocks the reader has said it
 *	  will read, which of them the stream has been given and in what order,
 *	  and which it has handed back, pinned, ahead of their read.
 *
 *	  Core's read stream (PostgreSQL 17 and later) reads the blocks its
 *	  callback names, in that order and only in that order, each pinned when
 *	  the consumer pulls it; it looks ahead only while the consumer pulls,
 *	  and once the callback names no block it is ended until it is reset.
 *	  A reader that knows its whole list - lion_count.c's heap blocks, a
 *	  small filter's members - fits that as it is.  One that predicts - a
 *	  scan reading its queue's next pages, which the queue reorders as it
 *	  goes - reads blocks out of the order it named them, some never, and
 *	  some it never named.  The ring sits between: the reader pushes the
 *	  blocks it expects to read (lion_ring_push()), the stream's callback
 *	  takes them in order (lion_ring_give()), and a read of any block
 *	  (lion_ring_read()) takes it from where it is:
 *
 *		held	pulled from the stream already, by a read of a later block:
 *				the pin is handed over
 *		given	named to the stream, maybe being read: the stream's buffers
 *				are pulled up to it, those before it kept, held, for their
 *				own reads
 *		pending	pushed, not yet given: it goes to the front, and the stream
 *				is pulled until it comes (and started over if it had ended)
 *		neither	read apart (ReadBuffer()), unless the stream is idle (it was
 *				given nothing it has not handed back) and has blocks pending,
 *				or refill pushes some: then through the stream, which this
 *				read sets going on them
 *
 *	  Pending blocks are kept in two queues: urgent, a run the reader is in
 *	  the middle of, given first, and predicted, given after.  Every held or
 *	  given block is a pin, and they are at most the ring's limit: a block
 *	  given when the ring is full pushes out the oldest held one, released
 *	  unread, and when every pin is a given one the callback names no block,
 *	  which ends the stream.  The positions of each part only grow (uint32,
 *	  compared by their differences), so a block's place does not move while
 *	  it is tracked, and a hash finds it by its number.
 *
 *	  Pure: the stream, the reads and the pins are the caller's, through
 *	  LionRingOps, so that test/unit/readahead_test.c can drive it against a
 *	  model of core's read stream.  Frontend-safe (c.h only).
 *-------------------------------------------------------------------------
 */
#ifndef LION_READAHEAD_RING_H
#define LION_READAHEAD_RING_H

#include "storage/block.h"

/* A pinned buffer, as the caller's ops know it (a Buffer: 0 is none). */
typedef int LionRingBuf;

/* What a slot of the given part holds (LionRingSlot.state) */
#define LION_RING_HOLE		0	/* read already, or released */
#define LION_RING_GIVEN		1	/* named to the stream, not pulled */
#define LION_RING_HELD		2	/* pulled, its pin kept for its read */

/* Where a tracked block is (LionRingKey.where) */
#define LION_RING_NOWHERE	0	/* an empty slot of the hash */
#define LION_RING_G			1	/* the given part: given or held */
#define LION_RING_URGENT	2	/* pending, urgent */
#define LION_RING_PREDICTED 3	/* pending, predicted */

typedef struct LionRingSlot
{
	BlockNumber blk;
	LionRingBuf buf;			/* LION_RING_HELD's pin */
	uint8		state;
} LionRingSlot;

typedef struct LionRingKey
{
	BlockNumber blk;
	uint8		where;
	uint32		pos;			/* its position in that part */
} LionRingKey;

/* A queue of pending blocks; InvalidBlockNumber a hole a block left. */
typedef struct LionRingDeque
{
	BlockNumber *b;				/* [mask + 1] */
	uint32		mask;
	uint32		head;			/* the first, maybe a hole */
	uint32		tail;			/* past the last */
} LionRingDeque;

/*
 * The caller's side.  next pulls the stream's next buffer, which must be of
 * the oldest block lion_ring_give() named that was not pulled yet, 0 when
 * the stream has ended; restart releases whatever the stream holds and
 * starts it over, so that it asks lion_ring_give() again; direct reads a
 * block apart; release drops a pin; block names a buffer's block.  refill,
 * if not NULL, may push more predicted blocks when the stream asks for one
 * and none is pending, or a read finds the stream idle and none pending,
 * and says whether it pushed any: it may run inside the stream's callback,
 * so it must not read, wait or fail.  fail reports what
 * the ring cannot go on from - a stream that lost its place - and does not
 * return in the server (an ERROR).
 */
typedef struct LionRingOps
{
	LionRingBuf (*next) (void *arg);
	void		(*restart) (void *arg);
	LionRingBuf (*direct) (void *arg, BlockNumber blk);
	void		(*release) (void *arg, LionRingBuf buf);
	BlockNumber (*block) (void *arg, LionRingBuf buf);
	bool		(*refill) (void *arg);
	void		(*fail) (void *arg, const char *what);
} LionRingOps;

typedef struct LionRing
{
	const LionRingOps *ops;
	void	   *arg;

	/*
	 * The given part, in the order given: [ghead, gpull) held or holes (the
	 * slot at ghead held, unless ghead == gpull), [gpull, gtail) given.
	 */
	LionRingSlot *g;			/* [gmask + 1] */
	uint32		gmask;
	uint32		glimit;			/* the most held and given at once: pins */
	uint32		ghead;
	uint32		gpull;
	uint32		gtail;
	int			nheld;

	LionRingDeque urgent;
	LionRingDeque predicted;
	uint32		plimit;			/* the most each holds, holes included */
	int			npending;		/* their blocks */

	LionRingKey *hash;			/* [hmask + 1], open addressing */
	uint32		hmask;

	bool		ended;			/* the callback named no block: the stream
								 * gives none until it is restarted */

	/* what it has done, for the caller's counts */
	uint64		npushed;		/* blocks pushed, and taken */
	uint64		ngiven;			/* ... named to the stream */
	uint64		nstreamed;		/* reads the stream served */
	uint64		napart;			/* reads of blocks it did not track */
	uint64		nwasted;		/* given blocks released unread */
	uint64		nrestarts;		/* times the stream was started over */
} LionRing;

/* The memory lion_ring_init() takes for a ring of these limits. */
extern size_t lion_ring_memsize(int pins, int pending);
extern void lion_ring_init(LionRing *r, int pins, int pending, void *mem,
						   const LionRingOps *ops, void *arg);
extern bool lion_ring_push(LionRing *r, BlockNumber blk, bool urgent);
extern BlockNumber lion_ring_give(LionRing *r);
extern LionRingBuf lion_ring_read(LionRing *r, BlockNumber blk);
extern void lion_ring_clear(LionRing *r);
extern bool lion_ring_tracks(const LionRing *r, BlockNumber blk);
extern const char *lion_ring_check(const LionRing *r);

/* Blocks given or pending: how far ahead of its reads the ring reaches. */
static inline int
lion_ring_ahead(const LionRing *r)
{
	return (int) (r->gtail - r->gpull) + r->npending;
}

/* Pins it holds: held blocks, and given ones (the stream's). */
static inline int
lion_ring_pins(const LionRing *r)
{
	return r->nheld + (int) (r->gtail - r->gpull);
}

#endif							/* LION_READAHEAD_RING_H */
