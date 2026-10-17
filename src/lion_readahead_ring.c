/*-------------------------------------------------------------------------
 *
 * lion_readahead_ring.c
 *		The bookkeeping of a read stream over predicted blocks
 *		(lion_readahead_ring.h; DESIGN.md §44.4, "Read streams").
 *
 * Frontend-safe, so that test/unit/readahead_test.c runs it against a model
 * of core's read stream: no palloc, no elog; what the ring cannot go on
 * from it reports through ops->fail.
 *
 *-------------------------------------------------------------------------
 */
#ifndef FRONTEND
#include "postgres.h"
#else
#include "c.h"
#endif

#include <string.h>

#include "common/hashfn.h"

#include "lion_readahead_ring.h"

/* the least power of two at or past n, n at least 1 */
static uint32
lion_ring_pow2(uint32 n)
{
	uint32		p = 1;

	while (p < n)
		p <<= 1;
	return p;
}

/* The parts' sizes for limits of pins and pending blocks (both at least 1). */
static void
lion_ring_sizes(int pins, int pending, uint32 *gsize, uint32 *psize,
				uint32 *hsize)
{
	uint32		p = (uint32) Max(pins, 1);
	uint32		q = (uint32) Max(pending, 1);

	*gsize = lion_ring_pow2(p);
	*psize = lion_ring_pow2(q);
	/* every tracked block has a key: at most half full */
	*hsize = lion_ring_pow2(2 * (p + 2 * q));
}

size_t
lion_ring_memsize(int pins, int pending)
{
	uint32		gsize;
	uint32		psize;
	uint32		hsize;

	lion_ring_sizes(pins, pending, &gsize, &psize, &hsize);
	return MAXALIGN(sizeof(LionRingSlot) * gsize) +
		MAXALIGN(sizeof(BlockNumber) * psize) * 2 +
		MAXALIGN(sizeof(LionRingKey) * hsize);
}

/*
 * A ring of at most pins held and given blocks and pending blocks of each
 * kind, in mem, which holds lion_ring_memsize() of them.  pins and pending
 * are taken at 1 at least.
 */
void
lion_ring_init(LionRing *r, int pins, int pending, void *mem,
			   const LionRingOps *ops, void *arg)
{
	uint32		gsize;
	uint32		psize;
	uint32		hsize;
	char	   *p = (char *) mem;
	uint32		i;

	lion_ring_sizes(pins, pending, &gsize, &psize, &hsize);
	memset(r, 0, sizeof(LionRing));
	r->ops = ops;
	r->arg = arg;
	r->g = (LionRingSlot *) p;
	p += MAXALIGN(sizeof(LionRingSlot) * gsize);
	r->urgent.b = (BlockNumber *) p;
	p += MAXALIGN(sizeof(BlockNumber) * psize);
	r->predicted.b = (BlockNumber *) p;
	p += MAXALIGN(sizeof(BlockNumber) * psize);
	r->hash = (LionRingKey *) p;
	r->gmask = gsize - 1;
	r->glimit = (uint32) Max(pins, 1);
	r->urgent.mask = r->predicted.mask = psize - 1;
	r->plimit = (uint32) Max(pending, 1);
	r->hmask = hsize - 1;
	for (i = 0; i < gsize; i++)
		r->g[i].state = LION_RING_HOLE;
	for (i = 0; i < hsize; i++)
		r->hash[i].where = LION_RING_NOWHERE;
}

/* ----------------------------------------------------------------
 * The hash: a tracked block's place, by its number
 * ----------------------------------------------------------------
 */

static inline uint32
lion_ring_slot_of(const LionRing *r, BlockNumber blk)
{
	return murmurhash32(blk) & r->hmask;
}

static LionRingKey *
lion_ring_find(const LionRing *r, BlockNumber blk)
{
	uint32		i = lion_ring_slot_of(r, blk);

	for (;;)
	{
		LionRingKey *k = &r->hash[i];

		if (k->where == LION_RING_NOWHERE)
			return NULL;
		if (k->blk == blk)
			return k;
		i = (i + 1) & r->hmask;
	}
}

/* A key for blk, which no key has; the hash has room by its size. */
static void
lion_ring_key_add(LionRing *r, BlockNumber blk, uint8 where, uint32 pos)
{
	uint32		i = lion_ring_slot_of(r, blk);

	while (r->hash[i].where != LION_RING_NOWHERE)
	{
		Assert(r->hash[i].blk != blk);
		i = (i + 1) & r->hmask;
	}
	r->hash[i].blk = blk;
	r->hash[i].where = where;
	r->hash[i].pos = pos;
}

/*
 * Remove a key, moving back the keys after it that its slot kept from
 * theirs (linear probing's deletion, which leaves no tombstone).
 */
static void
lion_ring_key_remove(LionRing *r, LionRingKey *k)
{
	uint32		i = (uint32) (k - r->hash);
	uint32		j = i;

	Assert(i <= r->hmask && k->where != LION_RING_NOWHERE);
	r->hash[i].where = LION_RING_NOWHERE;
	for (;;)
	{
		uint32		home;

		j = (j + 1) & r->hmask;
		if (r->hash[j].where == LION_RING_NOWHERE)
			break;
		home = lion_ring_slot_of(r, r->hash[j].blk);
		/* j's key stays if its home lies cyclically in (i, j] */
		if (i <= j ? (home > i && home <= j) : (home > i || home <= j))
			continue;
		r->hash[i] = r->hash[j];
		r->hash[j].where = LION_RING_NOWHERE;
		i = j;
	}
}

bool
lion_ring_tracks(const LionRing *r, BlockNumber blk)
{
	return lion_ring_find(r, blk) != NULL;
}

/* ----------------------------------------------------------------
 * The pending queues
 * ----------------------------------------------------------------
 */

/* Past the holes at its head. */
static void
lion_ring_deque_trim(LionRingDeque *d)
{
	while (d->head != d->tail && d->b[d->head & d->mask] == InvalidBlockNumber)
		d->head++;
}

static LionRingDeque *
lion_ring_deque(LionRing *r, uint8 where)
{
	Assert(where == LION_RING_URGENT || where == LION_RING_PREDICTED);
	return where == LION_RING_URGENT ? &r->urgent : &r->predicted;
}

/* blk at the back (front: at the front); false if it is full */
static bool
lion_ring_enqueue(LionRing *r, uint8 where, BlockNumber blk, bool front)
{
	LionRingDeque *d = lion_ring_deque(r, where);
	uint32		pos;

	lion_ring_deque_trim(d);
	if (d->tail - d->head >= r->plimit)
		return false;
	if (front)
		pos = --d->head;
	else
		pos = d->tail++;
	d->b[pos & d->mask] = blk;
	lion_ring_key_add(r, blk, where, pos);
	r->npending++;
	return true;
}

/* Take a pending block out of its queue, leaving a hole; and its key. */
static void
lion_ring_unqueue(LionRing *r, LionRingKey *k)
{
	LionRingDeque *d = lion_ring_deque(r, k->where);

	Assert(k->pos - d->head < d->tail - d->head);
	Assert(d->b[k->pos & d->mask] == k->blk);
	d->b[k->pos & d->mask] = InvalidBlockNumber;
	lion_ring_deque_trim(d);
	lion_ring_key_remove(r, k);
	r->npending--;
	Assert(r->npending >= 0);
}

/* The next pending block, urgent ones first, out of its queue; or none. */
static BlockNumber
lion_ring_dequeue(LionRing *r)
{
	LionRingDeque *d = &r->urgent;

	lion_ring_deque_trim(d);
	if (d->head == d->tail)
	{
		d = &r->predicted;
		lion_ring_deque_trim(d);
		if (d->head == d->tail)
			return InvalidBlockNumber;
	}
	return d->b[d->head & d->mask];
}

/* ----------------------------------------------------------------
 * The given part
 * ----------------------------------------------------------------
 */

/* Past the holes at its head. */
static void
lion_ring_g_trim(LionRing *r)
{
	while (r->ghead != r->gpull &&
		   r->g[r->ghead & r->gmask].state == LION_RING_HOLE)
		r->ghead++;
}

/*
 * Room for one more given block: the oldest held one released unread if
 * the ring is full.  False if every pin is a given block's.
 */
static bool
lion_ring_g_room(LionRing *r)
{
	while (r->gtail - r->ghead >= r->glimit)
	{
		LionRingSlot *s;
		LionRingKey *k;

		if (r->ghead == r->gpull)
			return false;
		s = &r->g[r->ghead & r->gmask];
		Assert(s->state == LION_RING_HELD);
		k = lion_ring_find(r, s->blk);
		Assert(k != NULL && k->where == LION_RING_G && k->pos == r->ghead);
		if (k != NULL)
			lion_ring_key_remove(r, k);
		s->state = LION_RING_HOLE;
		r->nheld--;
		r->nwasted++;
		r->ops->release(r->arg, s->buf);
		r->ghead++;
		lion_ring_g_trim(r);
	}
	return true;
}

/*
 * The stream's callback: the next block to read, urgent ones first, then
 * predicted ones, then what refill pushes; InvalidBlockNumber, which ends
 * the stream, when there is none or no pin is left for one.
 */
BlockNumber
lion_ring_give(LionRing *r)
{
	BlockNumber blk;
	LionRingKey *k;
	LionRingSlot *s;

	if (!lion_ring_g_room(r))
	{
		r->ended = true;
		return InvalidBlockNumber;
	}
	blk = lion_ring_dequeue(r);
	if (blk == InvalidBlockNumber && r->ops->refill != NULL &&
		r->ops->refill(r->arg))
		blk = lion_ring_dequeue(r);
	if (blk == InvalidBlockNumber)
	{
		r->ended = true;
		return InvalidBlockNumber;
	}
	k = lion_ring_find(r, blk);
	Assert(k != NULL && k->where != LION_RING_G);
	{
		LionRingDeque *d = lion_ring_deque(r, k->where);

		Assert(k->pos == d->head);
		d->b[d->head & d->mask] = InvalidBlockNumber;
		d->head++;
		r->npending--;
	}
	s = &r->g[r->gtail & r->gmask];
	Assert(s->state == LION_RING_HOLE);
	s->blk = blk;
	s->buf = 0;
	s->state = LION_RING_GIVEN;
	k->where = LION_RING_G;
	k->pos = r->gtail;
	r->gtail++;
	r->ngiven++;
	return blk;
}

/*
 * Pull the stream's next buffer, which is the oldest given block's, and
 * hold it.  The stream may ask for more blocks meanwhile (lion_ring_give()),
 * which may release held blocks at the head; never the one pulled, which
 * is given until it comes back.
 */
static bool
lion_ring_pull(LionRing *r)
{
	LionRingBuf buf = r->ops->next(r->arg);
	LionRingSlot *s;

	if (buf == 0 || r->gpull == r->gtail)
	{
		r->ops->fail(r->arg,
					 "the read stream ended before the blocks it was given");
		return false;
	}
	s = &r->g[r->gpull & r->gmask];
	Assert(s->state == LION_RING_GIVEN);
	if (r->ops->block(r->arg, buf) != s->blk)
	{
		r->ops->release(r->arg, buf);
		r->ops->fail(r->arg,
					 "the read stream lost its place in the blocks it was given");
		return false;
	}
	s->state = LION_RING_HELD;
	s->buf = buf;
	r->gpull++;
	r->nheld++;
	return true;
}

/* A held block's pin, handed to its read. */
static LionRingBuf
lion_ring_take(LionRing *r, LionRingKey *k)
{
	LionRingSlot *s = &r->g[k->pos & r->gmask];
	LionRingBuf buf = s->buf;

	Assert(s->state == LION_RING_HELD && s->blk == k->blk);
	s->state = LION_RING_HOLE;
	s->buf = 0;
	r->nheld--;
	lion_ring_key_remove(r, k);
	lion_ring_g_trim(r);
	r->nstreamed++;
	return buf;
}

/*
 * Push a block the reader expects to read: urgent, a run it is reading,
 * given before anything predicted; or predicted.  A block tracked already
 * stays where it is, but an urgent push of a predicted one brings it to
 * the urgent queue's back, behind the run before it.  True if the block was
 * taken in; false if it was tracked already or its queue is full.
 */
bool
lion_ring_push(LionRing *r, BlockNumber blk, bool urgent)
{
	LionRingKey *k;

	Assert(BlockNumberIsValid(blk));
	if (!BlockNumberIsValid(blk))
		return false;
	k = lion_ring_find(r, blk);
	if (k != NULL)
	{
		if (urgent && k->where == LION_RING_PREDICTED)
		{
			lion_ring_unqueue(r, k);
			if (!lion_ring_enqueue(r, LION_RING_URGENT, blk, false))
				(void) lion_ring_enqueue(r, LION_RING_PREDICTED, blk, true);
		}
		return false;
	}
	if (!lion_ring_enqueue(r, urgent ? LION_RING_URGENT : LION_RING_PREDICTED,
						   blk, false))
		return false;
	r->npushed++;
	return true;
}

/*
 * Read blk: its pin, from the stream if the ring tracks it, apart if not
 * (the header).  Each pull takes the oldest given block, so a given block
 * comes after those given before it, which are held for their own reads;
 * a pending one is first moved to the front, so it is given next, and the
 * stream is restarted if it had ended - once the blocks it was given are
 * all pulled, which restarting would release.
 */
LionRingBuf
lion_ring_read(LionRing *r, BlockNumber blk)
{
	bool		refilled = false;

	Assert(BlockNumberIsValid(blk));
	for (;;)
	{
		LionRingKey *k = lion_ring_find(r, blk);

		if (k == NULL)
		{
			/*
			 * Not tracked.  An idle stream - nothing given, maybe ended -
			 * would stay so through a read apart: refill first, if nothing
			 * is pending, which may push this very block (a stream fed only
			 * by refill, ended by its pins, starts again so); and with
			 * blocks pending read through the stream, which sets it going
			 * on them too.
			 */
			if (r->gpull == r->gtail)
			{
				if (r->npending == 0 && r->ops->refill != NULL && !refilled)
				{
					refilled = true;
					if (r->ops->refill(r->arg))
						continue;
				}
				if (r->npending > 0 &&
					lion_ring_enqueue(r, LION_RING_URGENT, blk, true))
					continue;
			}
			r->napart++;
			return r->ops->direct(r->arg, blk);
		}
		if (k->where == LION_RING_G)
		{
			if (r->g[k->pos & r->gmask].state == LION_RING_HELD)
				return lion_ring_take(r, k);
			/* given: pull up to it */
			Assert(r->g[k->pos & r->gmask].state == LION_RING_GIVEN);
			if (!lion_ring_pull(r))
				return 0;
			continue;
		}

		/* pending: to the front, so that it is given next */
		lion_ring_deque_trim(&r->urgent);
		if (k->where != LION_RING_URGENT || k->pos != r->urgent.head)
		{
			lion_ring_unqueue(r, k);
			if (!lion_ring_enqueue(r, LION_RING_URGENT, blk, true))
			{
				/* no room even there (a full urgent queue): apart */
				r->napart++;
				return r->ops->direct(r->arg, blk);
			}
			continue;
		}
		if (r->gpull != r->gtail)
		{
			/* the blocks given before it come first */
			if (!lion_ring_pull(r))
				return 0;
			continue;
		}
		if (r->ended)
		{
			r->ops->restart(r->arg);
			r->ended = false;
			r->nrestarts++;
		}
		if (!lion_ring_pull(r))
			return 0;
	}
}

/*
 * Forget every block: the held ones released, the given ones released by
 * restarting the stream, the pending ones dropped.  The stream is ready to
 * start again.
 */
void
lion_ring_clear(LionRing *r)
{
	uint32		pos;
	uint32		i;

	for (pos = r->ghead; pos != r->gpull; pos++)
	{
		LionRingSlot *s = &r->g[pos & r->gmask];

		if (s->state == LION_RING_HELD)
		{
			r->nwasted++;
			r->ops->release(r->arg, s->buf);
		}
		s->state = LION_RING_HOLE;
		s->buf = 0;
	}
	if (r->gpull != r->gtail || r->ended)
	{
		r->nwasted += r->gtail - r->gpull;
		for (pos = r->gpull; pos != r->gtail; pos++)
			r->g[pos & r->gmask].state = LION_RING_HOLE;
		r->ops->restart(r->arg);
	}
	r->ghead = r->gpull = r->gtail = 0;
	r->nheld = 0;
	r->urgent.head = r->urgent.tail = 0;
	r->predicted.head = r->predicted.tail = 0;
	r->npending = 0;
	r->ended = false;
	for (i = 0; i <= r->hmask; i++)
		r->hash[i].where = LION_RING_NOWHERE;
}

/* ----------------------------------------------------------------
 * The invariants, for the unit tests and assert-enabled builds
 * ----------------------------------------------------------------
 */

/* Is k the key of the tracked block it names, where it says? */
static const char *
lion_ring_check_key(const LionRing *r, const LionRingKey *k)
{
	if (k->where == LION_RING_G)
	{
		const LionRingSlot *s = &r->g[k->pos & r->gmask];

		if (k->pos - r->ghead >= r->gtail - r->ghead)
			return "a key's given position lies outside the given part";
		if (s->state == LION_RING_HOLE || s->blk != k->blk)
			return "a key's given slot does not hold its block";
		if ((s->state == LION_RING_HELD) != (k->pos - r->ghead < r->gpull - r->ghead))
			return "a held block lies past gpull, or a given one before it";
	}
	else if (k->where == LION_RING_URGENT || k->where == LION_RING_PREDICTED)
	{
		const LionRingDeque *d = k->where == LION_RING_URGENT ?
			&r->urgent : &r->predicted;

		if (k->pos - d->head >= d->tail - d->head)
			return "a key's pending position lies outside its queue";
		if (d->b[k->pos & d->mask] != k->blk)
			return "a key's pending slot does not hold its block";
	}
	else
		return "a key is nowhere";
	if (lion_ring_find(r, k->blk) != k)
		return "a key cannot be found by its block";
	return NULL;
}

/*
 * NULL if the ring is consistent, else what is not: every part within its
 * limits and in its order, every block in it once with a key that finds it,
 * and the counts what the parts hold.
 */
const char *
lion_ring_check(const LionRing *r)
{
	uint32		pos;
	uint32		i;
	int			nheld = 0;
	int			npending = 0;
	uint32		nkeys = 0;
	uint32		ntracked = 0;
	const char *bad;
	int			q;

	if (r->gpull - r->ghead > r->gtail - r->ghead)
		return "gpull lies outside [ghead, gtail]";
	if (r->gtail - r->ghead > r->glimit)
		return "the given part holds more than its limit";
	if (r->ghead != r->gpull && r->g[r->ghead & r->gmask].state != LION_RING_HELD)
		return "the given part's head is a hole";
	for (pos = r->ghead; pos != r->gtail; pos++)
	{
		const LionRingSlot *s = &r->g[pos & r->gmask];
		bool		pulled = pos - r->ghead < r->gpull - r->ghead;

		if (pulled && s->state == LION_RING_GIVEN)
			return "a given block lies before gpull";
		if (!pulled && s->state != LION_RING_GIVEN)
			return "a held block or a hole lies after gpull";
		if (s->state == LION_RING_HELD && s->buf == 0)
			return "a held block has no pin";
		if (s->state == LION_RING_HOLE)
			continue;
		nheld += s->state == LION_RING_HELD;
		ntracked++;
		{
			const LionRingKey *k = lion_ring_find(r, s->blk);

			if (k == NULL || k->where != LION_RING_G || k->pos != pos)
				return "a given or held block has no key to its slot";
		}
	}
	for (pos = r->gtail; pos != r->ghead + r->gmask + 1; pos++)
		if (r->g[pos & r->gmask].state != LION_RING_HOLE)
			return "a slot outside the given part is in use";
	if (nheld != r->nheld)
		return "nheld is not the held blocks' count";
	for (q = 0; q < 2; q++)
	{
		const LionRingDeque *d = q == 0 ? &r->urgent : &r->predicted;
		uint8		where = q == 0 ? LION_RING_URGENT : LION_RING_PREDICTED;

		if (d->tail - d->head > r->plimit)
			return "a pending queue holds more than its limit";
		for (pos = d->head; pos != d->tail; pos++)
		{
			BlockNumber blk = d->b[pos & d->mask];
			const LionRingKey *k;

			if (blk == InvalidBlockNumber)
				continue;
			npending++;
			ntracked++;
			k = lion_ring_find(r, blk);
			if (k == NULL || k->where != where || k->pos != pos)
				return "a pending block has no key to its slot";
		}
	}
	if (npending != r->npending)
		return "npending is not the pending blocks' count";
	for (i = 0; i <= r->hmask; i++)
	{
		if (r->hash[i].where == LION_RING_NOWHERE)
			continue;
		nkeys++;
		if ((bad = lion_ring_check_key(r, &r->hash[i])) != NULL)
			return bad;
	}
	if (nkeys != ntracked)
		return "the hash holds keys of blocks the ring does not track";
	if (2 * nkeys > r->hmask + 1)
		return "the hash is more than half full";
	return NULL;
}
