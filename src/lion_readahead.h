/*-------------------------------------------------------------------------
 * lion_readahead.h
 *	  Reading a relation's blocks in an order its reader predicts
 *	  (DESIGN.md §44.4, "Read streams"): through a read stream from
 *	  PostgreSQL 17 on, the blocks pushed ahead of their reads
 *	  (lion_readahead_ring.h); by asking the kernel for them ahead
 *	  (PrefetchBuffer()) and reading each when it comes, on 16 or wherever
 *	  a stream is not wanted; or by reading each when it comes and asking
 *	  for nothing.  The reader says which blocks it expects and reads them
 *	  as it would with ReadBuffer(), whichever of the three it is.  A reader
 *	  that knows its whole list and reads it in order - the exact path's
 *	  heap blocks - needs no prediction: from 17 it reads it through a plain
 *	  read stream over the list (lion_readahead_begin_list()), and asks and
 *	  reads as above where it does not stream.
 *-------------------------------------------------------------------------
 */
#ifndef LION_READAHEAD_H
#define LION_READAHEAD_H

#include "storage/bufmgr.h"
#include "utils/rel.h"

#include "lion_readahead_ring.h"

/* How a LionReadahead reads ahead (pg_lion.vector_prefetch's values, §44.8) */
#define LION_READAHEAD_OFF		0	/* not at all: ReadBuffer() as it comes */
#define LION_READAHEAD_FADVISE	1	/* PrefetchBuffer() ahead, then read */
#define LION_READAHEAD_STREAM	2	/* a read stream: 17 and later */

/* ... where the server has read streams, and where not */
#if PG_VERSION_NUM >= 170000
#define LION_READAHEAD_DEFAULT	LION_READAHEAD_STREAM
#else
#define LION_READAHEAD_DEFAULT	LION_READAHEAD_FADVISE
#endif

/*
 * What a reader knows of its asks of the kernel for one relation
 * (LION_READAHEAD_FADVISE; DESIGN.md §44.4, "Prefetch"): the blocks it has
 * asked for lately, the last one of LION_ASK_SLOTS slots by its hash, so
 * that a block several of its reads share is asked for once; and whether
 * asking pays - after LION_ASK_WARM asks in a row that found their page in
 * shared buffers already, each a lookup for nothing, the next
 * LION_ASK_DORMANT asks pass unasked, and then it asks again.  Several
 * readaheads of one relation may share one, as one scan's do.
 */
#define LION_ASK_SLOTS		128		/* a power of two */
#define LION_ASK_WARM		32
#define LION_ASK_DORMANT	256

typedef struct LionAsk
{
	BlockNumber seen[LION_ASK_SLOTS];	/* blocks asked for lately */
	int			warm;			/* asks in a row that found their page */
	int			dormant;		/* asks to let pass */
	int64		nasked;			/* asks made (PrefetchBuffer() calls) ... */
	int64		nmissing;		/* ... that did not find their page */
} LionAsk;

extern void lion_ask_reset(LionAsk *a);
extern void lion_ask(LionAsk *a, Relation rel, BlockNumber blk, bool once);

/*
 * A readahead of LION_READAHEAD_STREAM reads at most this many blocks ahead
 * and holds as many pins (its ring's limit, the reader's choice); given
 * fewer than LION_READAHEAD_MIN_PINS, which a pool too small for its
 * connections leaves (lion_pin_fair_share()), it asks the kernel instead.
 */
#define LION_READAHEAD_MIN_PINS	4

/* The refill a LION_READAHEAD_STREAM readahead calls (LionRingOps.refill). */
typedef bool (*LionReadaheadRefill) (void *arg);

/*
 * The next block of a list a reader knows whole and reads in its order
 * (lion_readahead_begin_list()), InvalidBlockNumber past its end, and at
 * every call after.  It runs inside the read stream's callback, or, while
 * the list rests, inside lion_readahead_next(): it must not read, wait or
 * fail.
 */
typedef BlockNumber (*LionReadaheadNext) (void *arg);

typedef struct LionReadahead LionReadahead;

/* What a readahead has done since its counts were last taken. */
typedef struct LionReadaheadCounts
{
	int64		nstreamed;		/* reads the stream served */
	int64		napart;			/* reads of blocks not pushed ahead */
	int64		nwasted;		/* blocks it read ahead, released unread */
	int64		nread;			/* blocks it read in, not in shared buffers */
	int64		nrestarts;		/* times its stream ran dry and started over */
} LionReadaheadCounts;

extern LionReadahead *lion_readahead_begin(Relation rel, int mode, int pins,
										   LionAsk *ask,
										   LionReadaheadRefill refill,
										   void *refill_arg);
extern LionReadahead *lion_readahead_begin_list(Relation rel, int mode,
												LionReadaheadNext next,
												void *next_arg);
extern bool lion_readahead_next(LionReadahead *s, BlockNumber *blk,
								Buffer *buf);
extern int	lion_readahead_mode(const LionReadahead *s);
extern bool lion_readahead_ask(LionReadahead *s, BlockNumber blk, bool urgent,
							   bool once);
extern Buffer lion_readahead_read(LionReadahead *s, BlockNumber blk);
extern bool lion_readahead_dormant(const LionReadahead *s);
extern void lion_readahead_awaken(LionReadahead *s);
extern int	lion_readahead_depth(const LionReadahead *s);
extern void lion_readahead_counts(LionReadahead *s, LionReadaheadCounts *c);
extern void lion_readahead_reset(LionReadahead *s);
extern void lion_readahead_end(LionReadahead *s);

#endif							/* LION_READAHEAD_H */
