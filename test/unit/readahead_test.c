/*-------------------------------------------------------------------------
 * readahead_test.c
 *	  Standalone unit tests for src/lion_readahead_ring.c: the bookkeeping of a
 *	  read stream over predicted blocks (DESIGN.md §44.4, "Read streams").
 *
 *	  Built by "make unit PG_CONFIG=..." with -DFRONTEND.  The ring is driven
 *	  through its ops against a model of core's read stream: a queue of the
 *	  blocks its callback named, pinned in that order, handed out one a
 *	  pull, a look-ahead distance that doubles when a read "waits" and
 *	  shrinks when it does not, the callback asked only during a pull, a
 *	  block now and then taken from the callback and held back unstarted
 *	  (core's read_stream_unget_block()), and an end once the callback names
 *	  no block, until the stream is reset.  Every buffer is a pin the model
 *	  counts, so a pin taken twice, released twice or left behind shows.
 *	  The properties under test:
 *
 *	  - every read returns a pin of the block asked for, whether the ring
 *		holds it, the stream was given it, it is pending, or it is not
 *		tracked at all, and a block is never pinned by the ring twice;
 *	  - a list read in the order it was pushed is read through the stream
 *		alone, nothing apart, nothing wasted, and so is one fed by refill;
 *	  - urgent blocks are given before predicted ones, an urgent push of a
 *		predicted block moves it, and a read of a pending block brings it
 *		first;
 *	  - pins never pass the ring's limit; a full ring releases its oldest
 *		held block, and with every pin given names no block, which ends the
 *		stream, and a later read restarts it;
 *	  - a stream that ends is restarted only once it holds nothing, so no
 *		block given to it is lost;
 *	  - clearing releases every pin and leaves a ring that works again;
 *	  - a stream that hands back the wrong block, or none, is reported;
 *	  - under random pushes, reads, refills and clears, at several limits,
 *		lion_ring_check() holds after every step and the counts add up:
 *		every block given is read through the stream or released unread.
 *
 *	  Exit status is 0 only if every check passed.
 *-------------------------------------------------------------------------
 */
#include "c.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lion_readahead_ring.h"

static const char *cur_phase = "startup";
static long nchecks = 0;
static long nfail = 0;

static void
check_impl(bool ok, int line, const char *msg)
{
	nchecks++;
	if (!ok)
	{
		nfail++;
		if (nfail <= 40)
			printf("FAIL [%s] readahead_test.c:%d: %s\n", cur_phase, line, msg);
		else if (nfail == 41)
			printf("... further failures suppressed\n");
	}
}

#define CHECK(ok, msg)	check_impl((ok), __LINE__, (msg))

/* A small deterministic generator, so that a failure reproduces. */
static uint64 rng_state = 0x9E3779B97F4A7C15ULL;

static uint64
rnd64(void)
{
	rng_state ^= rng_state << 13;
	rng_state ^= rng_state >> 7;
	rng_state ^= rng_state << 17;
	return rng_state;
}

static int
rnd(int n)
{
	return (int) (rnd64() % (uint64) n);
}

static void *
xmalloc(size_t n)
{
	void	   *p = malloc(n > 0 ? n : 1);

	if (p == NULL)
	{
		printf("out of memory\n");
		exit(2);
	}
	return p;
}

/* ----------------------------------------------------------------
 * The model of a read stream, and of the buffer pool's pins
 * ----------------------------------------------------------------
 */

#define MAXBUFS		(1 << 20)
#define MODEL_Q		4096

typedef struct Model
{
	LionRing   *ring;

	/* the buffers: each a pin of one block, until released */
	BlockNumber *bufblk;		/* [MAXBUFS] */
	bool	   *pinned;			/* [MAXBUFS] */
	int			nbufs;
	int			npins;			/* pinned now */

	/* the stream: blocks named, in order, each pinned (q), and held back */
	LionRingBuf q[MODEL_Q];
	int			qhead;
	int			qn;
	BlockNumber held;			/* named, not started: read_stream_unget_block() */
	int			distance;		/* 0: ended */
	int			maxd;			/* the most it looks ahead */
	int			waitpct;		/* how often a pull "waits" */
	int			ungetpct;		/* ... and a block is held back */
	int			nrestarts;

	/* misbehaviour, for the failure tests */
	bool		lose;			/* hand back the wrong block next */
	bool		dry;			/* hand back none next */
	const char *failed;			/* what fail reported */

	/* refill: a list the ring is fed from, a few blocks at a time */
	BlockNumber *list;
	int			nlist;
	int			listpos;
	int			chunk;
	int			nrefills;
} Model;

static Model *M;

static LionRingBuf
model_pin(BlockNumber blk)
{
	int			b;

	if (M->nbufs + 1 >= MAXBUFS)
	{
		printf("readahead_test: out of model buffers\n");
		exit(2);
	}
	b = ++M->nbufs;
	M->bufblk[b] = blk;
	M->pinned[b] = true;
	M->npins++;
	return b;
}

static void
model_unpin(LionRingBuf b)
{
	CHECK(b > 0 && b <= M->nbufs, "a release of no buffer");
	if (b <= 0 || b > M->nbufs)
		return;
	CHECK(M->pinned[b], "a buffer released twice");
	if (M->pinned[b])
	{
		M->pinned[b] = false;
		M->npins--;
	}
}

/* Ask the callback for blocks while the distance allows, as core does. */
static void
model_look_ahead(void)
{
	while (M->distance > 0 && M->qn + (M->held != InvalidBlockNumber) < M->distance)
	{
		BlockNumber blk;

		if (M->held != InvalidBlockNumber)
		{
			blk = M->held;
			M->held = InvalidBlockNumber;
		}
		else
		{
			blk = lion_ring_give(M->ring);
			if (blk == InvalidBlockNumber)
			{
				M->distance = 0;	/* ended */
				break;
			}
			if (rnd(100) < M->ungetpct)
			{
				/* an I/O limit: named, put back, started on a later pull */
				M->held = blk;
				break;
			}
		}
		CHECK(M->qn < MODEL_Q, "the model's queue overflowed");
		M->q[(M->qhead + M->qn) % MODEL_Q] = model_pin(blk);
		M->qn++;
	}
}

static LionRingBuf
op_next(void *arg)
{
	LionRingBuf b;

	if (M->qn == 0 && M->held == InvalidBlockNumber)
	{
		if (M->distance == 0)
			return 0;
		model_look_ahead();
	}
	if (M->qn == 0 && M->held != InvalidBlockNumber)
	{
		/* only a held-back block: start it now */
		M->q[M->qhead] = model_pin(M->held);
		M->held = InvalidBlockNumber;
		M->qn = 1;
	}
	if (M->qn == 0)
		return 0;
	if (M->dry)
	{
		M->dry = false;
		return 0;
	}
	b = M->q[M->qhead];
	M->qhead = (M->qhead + 1) % MODEL_Q;
	M->qn--;
	if (M->lose)
	{
		/* the wrong block: a pin of another */
		M->lose = false;
		model_unpin(b);
		b = model_pin(M->bufblk[b] + 1000003);
	}
	/* an ended stream (distance 0) stays so until reset */
	if (M->distance > 0 && rnd(100) < M->waitpct)
		M->distance = Min(M->distance * 2, M->maxd);
	else if (M->distance > 1)
		M->distance--;
	model_look_ahead();
	return b;
}

static void
op_restart(void *arg)
{
	while (M->qn > 0)
	{
		model_unpin(M->q[M->qhead]);
		M->qhead = (M->qhead + 1) % MODEL_Q;
		M->qn--;
	}
	M->held = InvalidBlockNumber;
	M->distance = 1;
	M->nrestarts++;
}

static LionRingBuf
op_direct(void *arg, BlockNumber blk)
{
	return model_pin(blk);
}

static void
op_release(void *arg, LionRingBuf buf)
{
	model_unpin(buf);
}

static BlockNumber
op_block(void *arg, LionRingBuf buf)
{
	CHECK(buf > 0 && buf <= M->nbufs && M->pinned[buf],
		  "the ring asked the block of a buffer it does not hold");
	return buf > 0 && buf <= M->nbufs ? M->bufblk[buf] : InvalidBlockNumber;
}

static bool
op_refill(void *arg)
{
	bool		any = false;
	int			i;

	if (M->list == NULL)
		return false;
	M->nrefills++;
	for (i = 0; i < M->chunk && M->listpos < M->nlist; i++)
	{
		if (!lion_ring_push(M->ring, M->list[M->listpos], false))
			break;
		M->listpos++;
		any = true;
	}
	return any;
}

static void
op_fail(void *arg, const char *what)
{
	M->failed = what;
}

static const LionRingOps ops = {
	.next = op_next,
	.restart = op_restart,
	.direct = op_direct,
	.release = op_release,
	.block = op_block,
	.refill = op_refill,
	.fail = op_fail
};

/* A fresh model and ring of these limits (the ring's memory exact-size). */
static LionRing *
setup(int pins, int pending, int maxd, int waitpct, int ungetpct)
{
	LionRing   *r = xmalloc(sizeof(LionRing));
	void	   *mem = xmalloc(lion_ring_memsize(pins, pending));
	BlockNumber *bufblk;
	bool	   *pinned;

	if (M == NULL)
	{
		M = xmalloc(sizeof(Model));
		M->bufblk = xmalloc(sizeof(BlockNumber) * MAXBUFS);
		M->pinned = xmalloc(sizeof(bool) * MAXBUFS);
		M->nbufs = MAXBUFS - 1;
	}
	/* only the buffers the last test used need clearing */
	memset(M->pinned, 0, sizeof(bool) * (M->nbufs + 1));
	bufblk = M->bufblk;
	pinned = M->pinned;
	memset(M, 0, sizeof(Model));
	M->bufblk = bufblk;
	M->pinned = pinned;
	M->ring = r;
	M->held = InvalidBlockNumber;
	M->distance = 1;
	M->maxd = maxd;
	M->waitpct = waitpct;
	M->ungetpct = ungetpct;
	lion_ring_init(r, pins, pending, mem, &ops, NULL);
	return r;
}

static void
teardown(LionRing *r)
{
	lion_ring_clear(r);
	CHECK(lion_ring_check(r) == NULL, "a cleared ring is inconsistent");
	CHECK(M->npins == 0, "pins left after the ring was cleared");
	CHECK(r->ngiven == r->nstreamed + r->nwasted,
		  "a block given was neither read through the stream nor released");
	free(r->g);
	free(r);
}

static void
check_ring(LionRing *r)
{
	const char *bad = lion_ring_check(r);

	CHECK(bad == NULL, bad != NULL ? bad : "");
	CHECK(lion_ring_pins(r) <= (int) r->glimit, "the ring holds more pins than its limit");
	/* what the ring holds plus what the stream holds is every pin out */
	CHECK(M->npins == r->nheld + M->qn, "pins out are not the ring's and the stream's");
}

/* Read blk as a reader does: the pin checked and released. */
static void
read_block(LionRing *r, BlockNumber blk)
{
	LionRingBuf b = lion_ring_read(r, blk);

	CHECK(b > 0 && b <= M->nbufs, "a read returned no buffer");
	if (b <= 0 || b > M->nbufs)
		return;
	CHECK(M->bufblk[b] == blk, "a read returned another block's buffer");
	CHECK(M->pinned[b], "a read returned a buffer not pinned");
	model_unpin(b);
	check_ring(r);
}

/* ----------------------------------------------------------------
 * The tests
 * ----------------------------------------------------------------
 */

/* A list pushed and read in order: all through the stream. */
static void
test_in_order(void)
{
	int			pins;

	cur_phase = "in order";
	for (pins = 4; pins <= 64; pins *= 2)
	{
		LionRing   *r = setup(pins, pins, 4 * pins, 50, 10);
		BlockNumber next = 100;
		BlockNumber blk;

		for (blk = 100; blk < 400; blk++)
		{
			/* keep the list pushed ahead of the reads, as far as it takes */
			while (next < 400 && lion_ring_push(r, next, false))
				next++;
			check_ring(r);
			read_block(r, blk);
		}
		CHECK(r->napart == 0, "a block of a list read in order was read apart");
		CHECK(r->nwasted == 0, "a block of a list read in order was wasted");
		CHECK(r->nstreamed == 300, "not every block came from the stream");
		teardown(r);
	}
}

/* ... fed by refill as the stream asks, a few blocks at a time. */
static void
test_refill(void)
{
	int			chunk;

	cur_phase = "refill";
	for (chunk = 1; chunk <= 16; chunk *= 2)
	{
		LionRing   *r = setup(16, 16, 64, 60, 10);
		BlockNumber list[500];
		int			i;

		for (i = 0; i < 500; i++)
			list[i] = 1000 + 3 * i + (i % 7 == 0);	/* runs and gaps */
		M->list = list;
		M->nlist = 500;
		M->chunk = chunk;
		/* the first block pushed sets it going; refill does the rest */
		CHECK(lion_ring_push(r, list[0], false), "a push into an empty ring failed");
		M->listpos = 1;
		for (i = 0; i < 500; i++)
			read_block(r, list[i]);
		CHECK(r->nstreamed == 500, "a refilled list was not all read through the stream");
		CHECK(r->napart == 0 && r->nwasted == 0, "a refilled list was read apart or wasted");
		CHECK(M->nrefills > 0, "refill was never called");
		M->list = NULL;
		teardown(r);
	}
}

/* Urgent before predicted; an urgent push moves a predicted block. */
static void
test_urgent(void)
{
	LionRing   *r = setup(32, 32, 32, 0, 0);
	BlockNumber b;

	cur_phase = "urgent";
	/* never waits, so it looks one block ahead: the order shows */
	for (b = 10; b < 15; b++)
		CHECK(lion_ring_push(r, b, false), "a predicted push failed");
	for (b = 50; b < 53; b++)
		CHECK(lion_ring_push(r, b, true), "an urgent push failed");
	CHECK(!lion_ring_push(r, 12, false), "a tracked block was pushed twice");
	CHECK(!lion_ring_push(r, 13, true), "a tracked block was pushed twice, urgent");
	check_ring(r);
	/* 13 moved behind the urgent run; the first pull gives 50 */
	CHECK(lion_ring_give(r) == 50, "the first block given is not the urgent run's");
	CHECK(lion_ring_give(r) == 51, "the urgent run was not given in order");
	CHECK(lion_ring_give(r) == 52, "the urgent run was not given in order");
	CHECK(lion_ring_give(r) == 13, "an urgent push of a predicted block did not move it");
	CHECK(lion_ring_give(r) == 10, "the predicted blocks were not given in order");
	check_ring(r);

	/*
	 * Those were given behind the model's back: tell it, so that its queue
	 * holds what the ring gave, in order, pinned
	 */
	M->q[0] = model_pin(50);
	M->q[1] = model_pin(51);
	M->q[2] = model_pin(52);
	M->q[3] = model_pin(13);
	M->q[4] = model_pin(10);
	M->qn = 5;
	M->distance = 5;
	/* a read of a pending block brings it first: after those given */
	read_block(r, 14);
	CHECK(r->nheld == 5, "the blocks given before a pending one were not held");
	read_block(r, 13);
	read_block(r, 50);
	read_block(r, 11);
	read_block(r, 777);			/* never pushed: apart */
	CHECK(r->napart == 1, "an untracked block was not read apart");
	teardown(r);
}

/*
 * A full ring: a stream that always waits wants more than four pins, so it
 * ends; blocks pulled through by a later read are held, or released to
 * make room, and read apart after; and every read is still right.
 */
static void
test_full(void)
{
	LionRing   *r = setup(4, 8, 1000, 100, 0);
	bool		ended = false;
	int			i;

	cur_phase = "full";
	for (i = 0; i < 8; i++)
		CHECK(lion_ring_push(r, 200 + i, false), "a push within the limit failed");
	CHECK(!lion_ring_push(r, 300, false), "a push past the pending limit was taken");
	read_block(r, 200);
	ended |= r->ended;
	read_block(r, 203);			/* 201 and 202 pulled through */
	ended |= r->ended;
	for (i = 4; i < 8; i++)
	{
		read_block(r, 200 + i);
		ended |= r->ended;
	}
	read_block(r, 201);
	read_block(r, 202);
	CHECK(ended, "a stream that wanted more than the pins allow never ended");
	CHECK(r->nstreamed + r->napart == 8, "the reads are not the stream's and those apart");
	teardown(r);
}

/*
 * A stream ended by its pins, with blocks still pending, restarts when
 * they are read - after the blocks it was given are pulled, so none is
 * lost - and reads them all through itself.
 */
static void
test_restart(void)
{
	LionRing   *r = setup(4, 16, 1000, 100, 0);
	bool		ended = false;
	int			i;

	cur_phase = "restart";
	for (i = 0; i < 16; i++)
		CHECK(lion_ring_push(r, 500 + 2 * i, false), "a push within the limit failed");
	for (i = 0; i < 16; i++)
	{
		read_block(r, 500 + 2 * i);
		ended |= r->ended;
	}
	CHECK(ended, "a stream that wanted more than its pins never ended");
	CHECK(r->nrestarts >= 1, "an ended stream was not restarted for its pending blocks");
	CHECK(r->nstreamed == 16 && r->napart == 0 && r->nwasted == 0,
		  "an ended stream lost blocks it had been given");
	teardown(r);
}

/*
 * A reader of chains: each run of a few blocks pushed urgent as it starts,
 * and the next runs predicted, a share of them never read - the partitions
 * a scan expected to probe and did not.  Every read right; most through the
 * stream; the ones never read released.
 */
static void
test_runs(void)
{
	int			round;

	cur_phase = "runs";
	for (round = 0; round < 20; round++)
	{
		LionRing   *r = setup(16 + round, 16 + round, 64, 50, 10);
		BlockNumber start[200];
		int			len[200];
		bool		skip[200];
		int			i;
		int			j;
		uint64		reads = 0;

		for (i = 0; i < 200; i++)
		{
			start[i] = 10000 + (BlockNumber) i * 50 + (BlockNumber) rnd(40);
			len[i] = 1 + rnd(5);
			skip[i] = rnd(5) == 0;
		}
		for (i = 0; i < 200; i++)
		{
			if (skip[i])
				continue;
			for (j = 0; j < len[i]; j++)
				(void) lion_ring_push(r, start[i] + j, true);
			/* the next two runs, predicted, as far as they fit */
			for (j = 1; j <= 2 && i + j < 200; j++)
			{
				int			k;

				for (k = 0; k < len[i + j]; k++)
					(void) lion_ring_push(r, start[i + j] + k, false);
			}
			check_ring(r);
			for (j = 0; j < len[i]; j++)
			{
				read_block(r, start[i] + j);
				reads++;
			}
		}
		CHECK(r->nstreamed + r->napart == reads, "the reads are not the stream's and those apart");
		CHECK(r->napart * 10 <= reads, "more than a tenth of a chain reader's reads were apart");
		teardown(r);
	}
}

/* A stream that hands back the wrong block, or none, is reported. */
static void
test_fail(void)
{
	LionRing   *r;
	LionRingBuf b;

	cur_phase = "fail";
	r = setup(8, 8, 8, 0, 0);
	lion_ring_push(r, 5, false);
	lion_ring_push(r, 6, false);
	M->lose = true;
	b = lion_ring_read(r, 5);
	CHECK(b == 0, "a read from a stream that lost its place returned a buffer");
	CHECK(M->failed != NULL && strstr(M->failed, "lost its place") != NULL,
		  "a stream that lost its place was not reported");
	teardown(r);

	r = setup(8, 8, 8, 0, 0);
	lion_ring_push(r, 5, false);
	M->dry = true;
	b = lion_ring_read(r, 5);
	CHECK(b == 0, "a read from a stream that ran dry returned a buffer");
	CHECK(M->failed != NULL && strstr(M->failed, "ended before") != NULL,
		  "a stream that ran dry was not reported");
	teardown(r);
}

/* Clearing in every state, and the ring working again after. */
static void
test_clear(void)
{
	int			round;

	cur_phase = "clear";
	for (round = 0; round < 50; round++)
	{
		LionRing   *r = setup(8, 8, 32, 50, 20);
		int			i;

		for (i = 0; i < 8; i++)
			lion_ring_push(r, 40 + i, rnd(2) == 0);
		for (i = 0; i < rnd(6); i++)
			read_block(r, 40 + rnd(8));
		lion_ring_clear(r);
		check_ring(r);
		CHECK(M->npins == 0, "a clear left pins");
		CHECK(r->npending == 0 && r->nheld == 0 && lion_ring_ahead(r) == 0,
			  "a clear left blocks tracked");
		/* again, from scratch */
		for (i = 0; i < 8; i++)
			lion_ring_push(r, 40 + i, false);
		for (i = 0; i < 8; i++)
			read_block(r, 40 + i);
		CHECK(r->ngiven == r->nstreamed + r->nwasted + (uint64) (r->gtail - r->gpull),
			  "the counts do not add up after a clear");
		teardown(r);
	}
}

/*
 * Random walks: pushes urgent and predicted, reads of tracked and
 * untracked blocks, refills and clears, over a small range of blocks so
 * that they collide, at several limits and stream behaviours.
 */
static void
test_random(void)
{
	static const int pinsets[] = {1, 2, 3, 4, 7, 16, 33, 64};
	int			seed;

	cur_phase = "random";
	for (seed = 0; seed < 400; seed++)
	{
		int			pins = pinsets[seed % lengthof(pinsets)];
		int			pending = pinsets[(seed / 3) % lengthof(pinsets)];
		LionRing   *r;
		BlockNumber list[64];
		int			range = 8 + rnd(120);
		int			step;
		int			i;
		long		failed = nfail;

		rng_state = 0x9E3779B97F4A7C15ULL + (uint64) seed * 0x2545F4914F6CDD1DULL;
		r = setup(pins, pending, 1 + rnd(64), rnd(101), rnd(40));
		for (i = 0; i < 64; i++)
			list[i] = (BlockNumber) rnd(range);
		if (seed % 2 == 0)
		{
			M->list = list;
			M->nlist = 64;
			M->chunk = 1 + rnd(8);
		}
		for (step = 0; step < 600; step++)
		{
			int			what = rnd(100);

			if (what < 35)
				(void) lion_ring_push(r, (BlockNumber) rnd(range), rnd(4) == 0);
			else if (what < 90)
			{
				/* mostly what was pushed, so that reads find their blocks */
				BlockNumber blk = (BlockNumber) rnd(range);

				read_block(r, blk);
			}
			else if (what < 92)
			{
				lion_ring_clear(r);
				M->listpos = 0;
			}
			else if (what < 96 && M->list != NULL && M->listpos >= M->nlist)
				M->listpos = 0;
			check_ring(r);
			if (nfail > failed)
				break;
		}
		M->list = NULL;
		teardown(r);
		if (nfail > failed)
		{
			printf("readahead_test: random seed %d (pins %d, pending %d) failed\n",
				   seed, pins, pending);
			break;
		}
	}
}

/* Many blocks over a long walk: the positions wrap their uint32s' low bits. */
static void
test_long(void)
{
	LionRing   *r = setup(8, 8, 16, 30, 10);
	BlockNumber blk;
	BlockNumber next = 0;

	cur_phase = "long";
	for (blk = 0; blk < 200000; blk++)
	{
		while (next < 200000 && next < blk + 12 && lion_ring_push(r, next, false))
			next++;
		if (blk % 1000 == 0)
			check_ring(r);
		read_block(r, blk);
	}
	CHECK(r->napart == 0 && r->nwasted == 0, "a long list was read apart, or wasted");
	teardown(r);
}

int
main(void)
{
	test_in_order();
	test_refill();
	test_urgent();
	test_full();
	test_restart();
	test_runs();
	test_fail();
	test_clear();
	test_random();
	test_long();
	free(M->bufblk);
	free(M->pinned);
	free(M);
	printf("readahead_test: %ld checks, %ld failed\n", nchecks, nfail);
	return nfail == 0 ? 0 : 1;
}
