/*-------------------------------------------------------------------------
 * sparse_test.c
 *	  Standalone unit tests for src/lion_sparse.c (DESIGN.md §13).
 *
 *	  Built by "make unit PG_CONFIG=..." with -DFRONTEND, so this program
 *	  links against nothing but libc, the container library and the segment
 *	  library.
 *
 *	  Every segment under test is shadowed by a brute-force reference: a
 *	  plain sorted array of (ckey, lo) pairs maintained by insertion sort,
 *	  and an encoder of its own that writes the bytes the format says those
 *	  pairs are (lion_sparse.h).  After each operation the segment is
 *	  compared against the reference byte for byte - the WAL redo of an
 *	  in-place insert depends on there being one encoding - and for the
 *	  answers of find/count/contains/iterate and the first and last ckey,
 *	  and lion_sparse_check() must accept it.
 *
 *	  Exit status is 0 only if every check passed.
 *-------------------------------------------------------------------------
 */
#include "c.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lion_sparse.h"

/* ----------------------------------------------------------------
 *							bookkeeping
 * ----------------------------------------------------------------
 */

static const char *cur_phase = "startup";
static long nchecks = 0;
static long nfail = 0;
static long npair_cmps = 0;

static void
check_impl(bool ok, int line, const char *msg)
{
	nchecks++;
	if (!ok)
	{
		nfail++;
		if (nfail <= 40)
			printf("FAIL [%s] sparse_test.c:%d: %s\n", cur_phase, line, msg);
		else if (nfail == 41)
			printf("... further failures suppressed\n");
	}
}

#define CHECK(ok, msg)	check_impl((ok), __LINE__, (msg))

static void
phase(const char *name)
{
	cur_phase = name;
}

/* ----------------------------------------------------------------
 *						fixed-seed PRNG (xorshift64*)
 * ----------------------------------------------------------------
 */

static uint64 rng_state = 1;

static void
rng_seed(uint64 s)
{
	rng_state = s ? s : UINT64CONST(0x9E3779B97F4A7C15);
}

static uint32
rng_next(void)
{
	uint64		x = rng_state;

	x ^= x >> 12;
	x ^= x << 25;
	x ^= x >> 27;
	rng_state = x;
	return (uint32) ((x * UINT64CONST(2685821657736338717)) >> 32);
}

static uint32
rng_below(uint32 n)
{
	return rng_next() % n;
}

/* ----------------------------------------------------------------
 *						work buffers and reference
 * ----------------------------------------------------------------
 */

typedef union SBuf
{
	LionContainer c;
	uint64		force_align;
	char		data[LION_CONTAINER_MAX_SIZE];
} SBuf;

typedef struct Pair
{
	uint32		ckey;
	uint16		lo;
} Pair;

typedef struct Ref
{
	Pair		p[LION_SPARSE_MAX_PAIRS + 8];
	uint32		n;
} Ref;

/* The largest container key a heap TID makes. */
#define MAX_CKEY	((uint32) (LION_SPARSE_MAX_CODE >> LION_CONTAINER_BITS))

static void
ref_init(Ref *r)
{
	r->n = 0;
}

static uint64
pair_code(const Pair *p)
{
	return (((uint64) p->ckey) << LION_CONTAINER_BITS) | p->lo;
}

static int
pair_cmp(uint32 ak, uint16 al, uint32 bk, uint16 bl)
{
	if (ak != bk)
		return ak < bk ? -1 : 1;
	if (al != bl)
		return al < bl ? -1 : 1;
	return 0;
}

/* The reference's own varint: written out longhand, not the library's. */
static uint32
ref_varint(uint8 *out, uint64 v)
{
	uint32		n = 0;

	do
	{
		uint8		b = (uint8) (v & 0x7F);

		v >>= 7;
		if (v != 0)
			b |= 0x80;
		if (out != NULL)
			out[n] = b;
		n++;
	} while (v != 0);
	return n;
}

/*
 * The bytes the format says r's pairs are, into out (when not NULL);
 * returns the stream's length.
 */
static uint32
ref_encode(const Ref *r, SBuf *out)
{
	uint8		stream[LION_SPARSE_MAX_PAIRS * 10];
	uint32		len = 0;
	uint64		prev;
	uint32		i;

	if (r->n == 0)
	{
		if (out != NULL)
		{
			memset(out, 0, sizeof(*out));
			out->c.type = LION_CT_SPARSE;
		}
		return 0;
	}

	prev = ((uint64) r->p[0].ckey) << LION_CONTAINER_BITS;
	for (i = 0; i < r->n; i++)
	{
		uint64		code = pair_code(&r->p[i]);

		len += ref_varint(stream + len, code - prev);
		prev = code;
	}

	if (out != NULL)
	{
		memset(out, 0, sizeof(*out));
		out->c.ckey = r->p[0].ckey;
		out->c.cardinality = (uint16) r->n;
		out->c.type = LION_CT_SPARSE;
		out->c.flags = (uint8) len;
		memcpy(out->data + LION_CONTAINER_HDRSZ, stream, Min(len, 255));
	}
	return len;
}

/* Insertion position of (ckey, lo) in the reference. */
static uint32
ref_pos(const Ref *r, uint32 ckey, uint16 lo)
{
	uint32		i;

	for (i = 0; i < r->n; i++)
	{
		if (pair_cmp(r->p[i].ckey, r->p[i].lo, ckey, lo) >= 0)
			break;
	}
	return i;
}

static bool
ref_contains(const Ref *r, uint32 ckey, uint16 lo)
{
	uint32		i = ref_pos(r, ckey, lo);

	return i < r->n && r->p[i].ckey == ckey && r->p[i].lo == lo;
}

/* Insert; false for a duplicate and for a pair that does not fit. */
static bool
ref_insert(Ref *r, uint32 ckey, uint16 lo)
{
	uint32		i = ref_pos(r, ckey, lo);
	uint32		j;

	if (i < r->n && r->p[i].ckey == ckey && r->p[i].lo == lo)
		return false;			/* duplicate */
	if (r->n >= LION_SPARSE_MAX_PAIRS)
		return false;			/* full */

	for (j = r->n; j > i; j--)
		r->p[j] = r->p[j - 1];
	r->p[i].ckey = ckey;
	r->p[i].lo = lo;
	r->n++;

	if (ref_encode(r, NULL) > LION_SPARSE_MAX_BYTES)
	{
		/* full by bytes: take it out again */
		for (j = i; j + 1 < r->n; j++)
			r->p[j] = r->p[j + 1];
		r->n--;
		return false;
	}
	return true;
}

static bool
ref_remove(Ref *r, uint32 ckey, uint16 lo)
{
	uint32		i = ref_pos(r, ckey, lo);
	uint32		j;

	if (i >= r->n || r->p[i].ckey != ckey || r->p[i].lo != lo)
		return false;

	for (j = i; j + 1 < r->n; j++)
		r->p[j] = r->p[j + 1];
	r->n--;
	return true;
}

static uint32
ref_count(const Ref *r, uint32 ckey)
{
	uint32		i;
	uint32		n = 0;

	for (i = 0; i < r->n; i++)
	{
		if (r->p[i].ckey == ckey)
			n++;
	}
	return n;
}

/* ----------------------------------------------------------------
 *						comparison against the reference
 * ----------------------------------------------------------------
 */

typedef struct IterState
{
	const Ref  *ref;
	uint32		i;
	bool		ok;
	uint32		stop_after;		/* stop the iteration after this many pairs */
} IterState;

static bool
iter_cb(uint32 ckey, uint16 lo, void *arg)
{
	IterState  *st = (IterState *) arg;

	if (st->i >= st->ref->n ||
		st->ref->p[st->i].ckey != ckey || st->ref->p[st->i].lo != lo)
		st->ok = false;
	st->i++;

	return st->i < st->stop_after;
}

static void
compare(const LionContainer *s, const Ref *r, int line)
{
	const char *err = NULL;
	IterState	it;
	SBuf		expect;
	uint32		nbytes = ref_encode(r, &expect);
	uint32		i;

	/* an empty segment is a work state only: it is never stored */
	if (r->n > 0)
		check_impl(lion_sparse_check(s, lion_sparse_size_for(nbytes), &err), line,
				   err ? err : "check() rejected a good segment");
	else
		check_impl(!lion_sparse_check(s, lion_sparse_size_for(nbytes), &err) &&
				   err != NULL, line, "check() refuses an empty segment");
	check_impl(s->cardinality == r->n, line, "cardinality matches");
	check_impl(lion_sparse_nbytes(s) == nbytes, line,
			   "the stream has the reference's length");
	check_impl(lion_sparse_size(s) == lion_sparse_size_for(nbytes), line,
			   "size matches the stream");
	check_impl(s->type == LION_CT_SPARSE, line, "type is LION_CT_SPARSE");
	check_impl(lion_item_size(s) == lion_sparse_size(s), line,
			   "lion_item_size() dispatches to the segment size");

	if (r->n > 0)
	{
		npair_cmps += r->n;
		check_impl(memcmp(s, &expect, lion_sparse_size_for(nbytes)) == 0, line,
				   "the bytes are the reference's encoding of the pairs");
		check_impl(lion_item_first_ckey(s) == r->p[0].ckey, line,
				   "first ckey matches");
		check_impl(lion_item_last_ckey(s) == r->p[r->n - 1].ckey, line,
				   "last ckey matches");
		check_impl(lion_item_covers(s, r->p[0].ckey), line,
				   "the segment covers its first ckey");
		check_impl(lion_item_covers(s, r->p[r->n - 1].ckey), line,
				   "the segment covers its last ckey");
	}

	if (s->cardinality != r->n)
		return;					/* the answers below would be nonsense */

	/* contains() and count() for every ckey present, and for its neighbours */
	for (i = 0; i < r->n; i++)
	{
		uint32		first;
		uint32		count;

		npair_cmps++;
		if (!lion_sparse_contains(s, r->p[i].ckey, r->p[i].lo))
		{
			check_impl(false, line, "contains() finds a member");
			break;
		}
		if (lion_sparse_contains(s, r->p[i].ckey, (uint16) (r->p[i].lo ^ 0x4000)) !=
			ref_contains(r, r->p[i].ckey, (uint16) (r->p[i].lo ^ 0x4000)))
		{
			check_impl(false, line, "contains() agrees about a non-member");
			break;
		}
		lion_sparse_find(s, r->p[i].ckey, &first, &count);
		if (count != ref_count(r, r->p[i].ckey) ||
			first >= r->n || r->p[first].ckey != r->p[i].ckey ||
			(first > 0 && r->p[first - 1].ckey == r->p[i].ckey))
		{
			check_impl(false, line, "find() returns the right range");
			break;
		}
		if (lion_sparse_count(s, r->p[i].ckey + 1) != ref_count(r, r->p[i].ckey + 1))
		{
			check_impl(false, line, "count() agrees for the next ckey");
			break;
		}
	}

	it.ref = r;
	it.i = 0;
	it.ok = true;
	it.stop_after = UINT32_MAX;
	lion_sparse_iterate(s, iter_cb, &it);
	check_impl(it.ok && it.i == r->n, line, "iterate() visits every pair in order");
}

#define COMPARE(s, r)	compare(&(s)->c, (r), __LINE__)

/* Fill a segment and its reference with the pairs of codes[0 .. n). */
static void
fill_codes(SBuf *b, Ref *r, const uint64 *codes, uint32 n)
{
	uint32		i;

	ref_init(r);
	lion_sparse_init(&b->c, 0);
	for (i = 0; i < n; i++)
	{
		uint32		ckey = (uint32) (codes[i] >> LION_CONTAINER_BITS);
		uint16		lo = (uint16) (codes[i] & LION_LO_MASK);

		CHECK(lion_sparse_insert(&b->c, ckey, lo, NULL), "a fixture pair goes in");
		ref_insert(r, ckey, lo);
	}
}

/* ----------------------------------------------------------------
 *								tests
 * ----------------------------------------------------------------
 */

static void
test_empty(void)
{
	SBuf		b;
	Ref			r;
	const char *err = NULL;

	phase("empty segment");
	ref_init(&r);
	lion_sparse_init(&b.c, 0);

	CHECK(b.c.cardinality == 0, "a fresh segment is empty");
	CHECK(b.c.type == LION_CT_SPARSE, "a fresh segment is a segment");
	CHECK(b.c.flags == 0, "a fresh segment has no stream");
	CHECK(lion_sparse_size(&b.c) == LION_CONTAINER_HDRSZ, "an empty segment is a header");
	CHECK(!lion_sparse_check(&b.c, LION_CONTAINER_HDRSZ, &err) && err != NULL,
		  "check() refuses an empty segment, which is never stored");
	CHECK(lion_item_last_ckey(&b.c) == 0,
		  "an empty segment's last ckey is its header's");
	CHECK(!lion_sparse_contains(&b.c, 0, 0), "an empty segment contains nothing");
	CHECK(lion_sparse_count(&b.c, 17) == 0, "count() of an empty segment is 0");
	CHECK(!lion_sparse_remove(&b.c, 1, 1), "remove() from an empty segment says no");
	CHECK(lion_sparse_insert_size(&b.c, 5, 3) == LION_CONTAINER_HDRSZ + 1,
		  "insert_size() of a first pair is a header and its lo");
	COMPARE(&b, &r);
}

static void
test_one_pair(void)
{
	SBuf		b;
	Ref			r;
	bool		dup = true;

	phase("one pair");
	ref_init(&r);
	lion_sparse_init(&b.c, 0);

	CHECK(lion_sparse_insert(&b.c, 12345, 777, &dup), "insert succeeds");
	CHECK(!dup, "the first insert is not a duplicate");
	ref_insert(&r, 12345, 777);
	COMPARE(&b, &r);

	CHECK(b.c.ckey == 12345, "the header carries the first ckey");
	CHECK(lion_sparse_size(&b.c) == LION_CONTAINER_HDRSZ + 2,
		  "a lo of 777 is a two-byte varint");
	CHECK(lion_sparse_size(&b.c) <= LION_SPARSE_SINGLETON_MAX,
		  "a one-pair segment fits LION_SPARSE_SINGLETON_MAX");

	CHECK(lion_sparse_insert(&b.c, 12345, 777, &dup), "re-inserting is not an error");
	CHECK(dup, "re-inserting reports a duplicate");
	CHECK(lion_sparse_insert_size(&b.c, 12345, 777) == lion_sparse_size(&b.c),
		  "insert_size() of a pair already there is the size");
	COMPARE(&b, &r);

	CHECK(lion_sparse_remove(&b.c, 12345, 777), "remove finds the pair");
	ref_remove(&r, 12345, 777);
	COMPARE(&b, &r);
	CHECK(b.c.cardinality == 0, "the segment is empty again");

	/* the first lo's varint is one, two or three bytes, never more */
	{
		static const uint16 los[] = {0, 127, 128, 16383, 16384, 32767};
		static const Size sizes[] = {1, 1, 2, 2, 3, 3};
		int			i;

		for (i = 0; i < (int) lengthof(los); i++)
		{
			ref_init(&r);
			lion_sparse_init(&b.c, 0);
			CHECK(lion_sparse_insert(&b.c, 9, los[i], NULL), "insert of a single lo");
			ref_insert(&r, 9, los[i]);
			COMPARE(&b, &r);
			CHECK(lion_sparse_size(&b.c) == LION_CONTAINER_HDRSZ + sizes[i],
				  "the first pair's varint is as long as its lo needs");
			CHECK(lion_sparse_size(&b.c) <= LION_SPARSE_SINGLETON_MAX,
				  "and within LION_SPARSE_SINGLETON_MAX");
		}
	}
}

/*
 * The cap: 255 bytes of stream, so at most 255 pairs (deltas of one byte),
 * and fewer when the deltas are long; the codes at either end of what a heap
 * TID makes; and deltas of every varint length.
 */
static void
test_boundaries(void)
{
	SBuf		b;
	Ref			r;
	uint64		codes[LION_SPARSE_MAX_PAIRS];
	uint32		i;

	phase("boundaries: 255 one-byte pairs, and the 256th");
	for (i = 0; i < LION_SPARSE_MAX_PAIRS; i++)
		codes[i] = (((uint64) 1000) << LION_CONTAINER_BITS) + i;	/* lo 0 .. 254 */
	fill_codes(&b, &r, codes, LION_SPARSE_MAX_PAIRS);
	COMPARE(&b, &r);
	CHECK(b.c.cardinality == LION_SPARSE_MAX_PAIRS, "the segment holds 255 pairs");
	CHECK(lion_sparse_size(&b.c) == LION_SPARSE_MAX_SIZE, "255 pairs of one byte fill 263 bytes");

	CHECK(lion_sparse_insert_size(&b.c, 999, 1) == 0, "insert_size() says full (before)");
	CHECK(!lion_sparse_insert(&b.c, 999, 1, NULL), "the 256th pair is refused (before)");
	CHECK(!lion_sparse_insert(&b.c, 500000, 1, NULL), "the 256th pair is refused (after)");
	CHECK(!lion_sparse_insert(&b.c, 1000, 9999, NULL), "the 256th pair is refused (inside)");
	COMPARE(&b, &r);

	/* a duplicate is still reported as such, and does not count as full */
	{
		bool		dup = false;

		CHECK(lion_sparse_insert(&b.c, 1000, 0, &dup), "a duplicate in a full segment is fine");
		CHECK(dup, "... and is reported as a duplicate");
	}

	/* one out, one in */
	CHECK(lion_sparse_remove(&b.c, 1000, 7), "removing from a full segment");
	ref_remove(&r, 1000, 7);
	CHECK(lion_sparse_insert(&b.c, 1000, 7, NULL), "and there is room again");
	ref_insert(&r, 1000, 7);
	COMPARE(&b, &r);

	phase("boundaries: full by bytes");
	{
		/* six-byte deltas (2^35 and more apart): 1 + 6k bytes */
		uint64		step = UINT64CONST(1) << 35;
		uint32		n = 0;

		ref_init(&r);
		lion_sparse_init(&b.c, 0);
		while (ref_insert(&r, (uint32) ((n * step) >> LION_CONTAINER_BITS), 3))
		{
			CHECK(lion_sparse_insert(&b.c, (uint32) ((n * step) >> LION_CONTAINER_BITS), 3, NULL),
				  "a long delta goes in while the reference says it fits");
			n++;
		}
		COMPARE(&b, &r);
		CHECK(n == 1 + (LION_SPARSE_MAX_BYTES - 1) / 6, "43 pairs of six-byte deltas fit");
		CHECK(!lion_sparse_insert(&b.c, (uint32) ((n * step) >> LION_CONTAINER_BITS), 3, NULL),
			  "and the next one is refused, though the pair count is far from the cap");
		CHECK(lion_sparse_insert_size(&b.c, (uint32) ((n * step) >> LION_CONTAINER_BITS), 3) == 0,
			  "insert_size() agrees");
		/* a pair inside the range may still fit: it splits a delta */
		CHECK(lion_sparse_insert_size(&b.c, 0, 4) != 0,
			  "a pair right after the first still fits");
		COMPARE(&b, &r);
	}

	phase("boundaries: the codes a heap TID can make");
	{
		uint64		maxcode = LION_SPARSE_MAX_CODE;
		const char *err = NULL;

		codes[0] = 0;
		codes[1] = maxcode;
		fill_codes(&b, &r, codes, 2);
		COMPARE(&b, &r);
		CHECK(lion_item_last_ckey(&b.c) == MAX_CKEY, "the last ckey is the largest there is");
		CHECK(lion_sparse_size(&b.c) == LION_CONTAINER_HDRSZ + 1 + LION_SPARSE_VARINT_MAX,
			  "the largest delta takes LION_SPARSE_VARINT_MAX bytes");
		CHECK(lion_sparse_contains(&b.c, MAX_CKEY, (uint16) (maxcode & LION_LO_MASK)),
			  "the largest code is found");

		/* a code past it is refused by check() */
		{
			SBuf		x = b;
			uint8	   *st = LION_SPARSE_STREAM(&x.c);

			/* add one to the second delta: its first byte is 0x7F | 0x80 */
			CHECK(st[1] == 0xFF, "the fixture's delta is all ones");
			st[1] = 0x80;
			st[2] = (uint8) (st[2] + 1);
			err = NULL;
			CHECK(!lion_sparse_check(&x.c, lion_sparse_size(&x.c), &err) && err != NULL,
				  "a code past the largest heap TID is rejected");
		}

		/* the largest ckey as a segment's first: its base is still a code */
		codes[0] = maxcode;
		fill_codes(&b, &r, codes, 1);
		COMPARE(&b, &r);
		CHECK(b.c.ckey == MAX_CKEY, "a segment can start at the largest ckey");
	}

	phase("boundaries: deltas of every varint length");
	{
		uint32		len;

		for (len = 1; len <= LION_SPARSE_VARINT_MAX; len++)
		{
			uint64		lo_delta = (len == 1) ? 1 : (UINT64CONST(1) << (7 * (len - 1)));
			uint64		hi_delta = (UINT64CONST(1) << (7 * len)) - 1;

			if (hi_delta > LION_SPARSE_MAX_CODE)
				hi_delta = LION_SPARSE_MAX_CODE;
			codes[0] = 0;
			codes[1] = lo_delta;
			codes[2] = lo_delta + hi_delta;
			if (codes[2] > LION_SPARSE_MAX_CODE)
				continue;
			fill_codes(&b, &r, codes, 3);
			COMPARE(&b, &r);
			CHECK(lion_sparse_size(&b.c) == LION_CONTAINER_HDRSZ + 1 + 2 * len,
				  "the shortest and the longest delta of a length take that length");
		}
	}
}

/* Inserting at the front, in the middle and at the back, in every order. */
static void
test_insert_positions(void)
{
	static const uint32 ckeys[] = {50, 10, 90, 30, 70, 20, 80, 40, 60, 10, 90};
	SBuf		b;
	Ref			r;
	uint32		i;

	phase("insert positions");
	ref_init(&r);
	lion_sparse_init(&b.c, 0);

	for (i = 0; i < lengthof(ckeys); i++)
	{
		uint16		lo = (uint16) (i * 111);

		if (lion_sparse_insert(&b.c, ckeys[i], lo, NULL))
			ref_insert(&r, ckeys[i], lo);
		COMPARE(&b, &r);
	}

	/* same ckey, descending lo: the pairs still come out ascending */
	for (i = 0; i < 3; i++)
	{
		uint16		lo = (uint16) (3000 - i * 100);

		lion_sparse_insert(&b.c, 55, lo, NULL);
		ref_insert(&r, 55, lo);
		COMPARE(&b, &r);
	}
	CHECK(lion_sparse_count(&b.c, 55) == 3, "three members of one ckey");

	/* a new first pair below the header's ckey, and one inside it */
	lion_sparse_insert(&b.c, 3, 32767, NULL);
	ref_insert(&r, 3, 32767);
	COMPARE(&b, &r);
	CHECK(b.c.ckey == 3, "the header follows a new first pair");
	lion_sparse_insert(&b.c, 3, 0, NULL);
	ref_insert(&r, 3, 0);
	COMPARE(&b, &r);
}

/*
 * The property the WAL delta of an INLINE insert depends on (DESIGN.md §25):
 * a pair appended above every other changes the header's count and length
 * and adds bytes at the end, and nothing else.
 */
static void
test_append_tail(void)
{
	SBuf		b;
	SBuf		before;
	Ref			r;
	uint32		i;
	uint64		code = ((uint64) 70000) << LION_CONTAINER_BITS;

	phase("an append changes only the header and the tail");
	ref_init(&r);
	lion_sparse_init(&b.c, 0);
	rng_seed(UINT64CONST(0x5EED2400));
	for (i = 0; i < 200; i++)
	{
		Size		oldsize = lion_sparse_size(&b.c);
		bool		ok;

		code += 1 + rng_below(i % 3 == 0 ? 400000 : 3000);
		before = b;
		ok = lion_sparse_insert(&b.c, (uint32) (code >> LION_CONTAINER_BITS),
								(uint16) (code & LION_LO_MASK), NULL);
		CHECK(ok == ref_insert(&r, (uint32) (code >> LION_CONTAINER_BITS),
							   (uint16) (code & LION_LO_MASK)),
			  "an append agrees with the reference");
		if (!ok)
			break;
		/* the first pair sets the header ckey of the empty segment */
		if (i > 0)
			CHECK(memcmp(&before, &b, offsetof(LionContainer, cardinality)) == 0,
				  "an append leaves the header ckey alone");
		CHECK(before.c.type == b.c.type, "an append leaves the type alone");
		CHECK(memcmp(before.data + LION_CONTAINER_HDRSZ, b.data + LION_CONTAINER_HDRSZ,
					 oldsize - LION_CONTAINER_HDRSZ) == 0,
			  "an append leaves every byte of the old stream where it was");
		CHECK(lion_sparse_size(&b.c) - oldsize <= LION_SPARSE_VARINT_MAX,
			  "an append adds one varint");
	}
	COMPARE(&b, &r);
	CHECK(r.n > 50, "the fixture appended a fair number of pairs");
}

/* lion_sparse_append(), the bulk build's way, against insert(). */
static void
test_append(void)
{
	SBuf		a;
	SBuf		b;
	uint64		last = 0;
	uint64		code = 0;
	uint32		i;

	phase("append() builds what insert() builds");
	rng_seed(UINT64CONST(0x5EED2500));
	lion_sparse_init(&a.c, 0);
	lion_sparse_init(&b.c, 0);
	for (i = 0; i < 1000; i++)
	{
		bool		r1;
		bool		r2;

		code += 1 + rng_below(i % 5 == 0 ? 3000000 : 20000);
		r1 = lion_sparse_append(&a.c, &last, (uint32) (code >> LION_CONTAINER_BITS),
								(uint16) (code & LION_LO_MASK));
		r2 = lion_sparse_insert(&b.c, (uint32) (code >> LION_CONTAINER_BITS),
								(uint16) (code & LION_LO_MASK), NULL);
		CHECK(r1 == r2, "append() and insert() agree on what fits");
		CHECK(memcmp(&a, &b, lion_sparse_size(&b.c)) == 0,
			  "append() and insert() write the same bytes");
		if (!r1)
		{
			/* start a new segment, as the build does */
			lion_sparse_init(&a.c, 0);
			lion_sparse_init(&b.c, 0);
			CHECK(lion_sparse_append(&a.c, &last, (uint32) (code >> LION_CONTAINER_BITS),
									 (uint16) (code & LION_LO_MASK)),
				  "a pair always fits an empty segment");
			(void) lion_sparse_insert(&b.c, (uint32) (code >> LION_CONTAINER_BITS),
									  (uint16) (code & LION_LO_MASK), NULL);
		}
		CHECK(last == code, "append() keeps the last code");
	}

	/* a repeat or a pair out of order is still handled */
	{
		uint64		save = last;

		CHECK(lion_sparse_append(&a.c, &last, (uint32) (code >> LION_CONTAINER_BITS),
								 (uint16) (code & LION_LO_MASK)),
			  "appending the last pair again is a no-op");
		CHECK(last == save, "and leaves the last code");
		CHECK(memcmp(&a, &b, lion_sparse_size(&b.c)) == 0, "and the bytes");
	}
}

static void
test_remove(void)
{
	SBuf		b;
	Ref			r;
	uint32		i;

	phase("remove");
	ref_init(&r);
	lion_sparse_init(&b.c, 0);

	for (i = 0; i < 100; i++)
	{
		lion_sparse_insert(&b.c, 10 + i / 3, (uint16) (i * 7), NULL);
		ref_insert(&r, 10 + i / 3, (uint16) (i * 7));
	}
	COMPARE(&b, &r);

	CHECK(!lion_sparse_remove(&b.c, 9, 0), "removing a ckey that is not there");
	CHECK(!lion_sparse_remove(&b.c, 10, 12345), "removing a lo that is not there");
	COMPARE(&b, &r);

	/* remove from the front, the middle and the back */
	while (r.n > 0)
	{
		uint32		idx = (r.n > 2) ? (rng_below(3) == 0 ? 0 :
									   (rng_below(2) ? r.n / 2 : r.n - 1)) : 0;
		uint32		ckey = r.p[idx].ckey;
		uint16		lo = r.p[idx].lo;
		Size		before = lion_sparse_size(&b.c);

		CHECK(lion_sparse_remove(&b.c, ckey, lo), "remove finds the pair");
		/* two deltas become one, which may be as long: 127 + 1 is 128 */
		CHECK(lion_sparse_size(&b.c) <= before, "a removal never grows the segment");
		ref_remove(&r, ckey, lo);
		if ((r.n % 7) == 0)
			COMPARE(&b, &r);
	}
	COMPARE(&b, &r);
	CHECK(b.c.cardinality == 0, "everything was removed");
}

/* lion_sparse_remove_if() against the same predicate applied to the reference. */
typedef struct PredArg
{
	uint32		mod;
	uint32		rem;
	long		ncalls;
} PredArg;

static bool
pred_cb(uint32 ckey, uint16 lo, void *arg)
{
	PredArg    *pa = (PredArg *) arg;

	pa->ncalls++;
	return ((ckey + lo) % pa->mod) == pa->rem;
}

static void
test_remove_if(void)
{
	uint32		mod;

	phase("remove_if");

	for (mod = 1; mod <= 5; mod++)
	{
		SBuf		b;
		Ref			r;
		Ref			expect;
		PredArg		pa;
		uint32		i;
		uint32		removed;
		uint32		expected = 0;

		ref_init(&r);
		lion_sparse_init(&b.c, 0);
		for (i = 0; i < 400; i++)
		{
			uint32		ckey = 5 + i / 2;
			uint16		lo = (uint16) (i * 13 + mod);

			if (lion_sparse_insert(&b.c, ckey, lo, NULL))
				ref_insert(&r, ckey, lo);
		}
		COMPARE(&b, &r);

		pa.mod = mod;
		pa.rem = 0;
		pa.ncalls = 0;

		expect = r;
		expect.n = 0;
		for (i = 0; i < r.n; i++)
		{
			if (((r.p[i].ckey + r.p[i].lo) % mod) == 0)
				expected++;
			else
				expect.p[expect.n++] = r.p[i];
		}

		removed = lion_sparse_remove_if(&b.c, pred_cb, &pa);
		CHECK(pa.ncalls == (long) r.n, "the predicate is called once per pair");
		CHECK(removed == expected, "remove_if removes what the predicate says");
		COMPARE(&b, &expect);
	}

	/* removing nothing must not touch the segment */
	{
		SBuf		b;
		Ref			r;
		PredArg		pa;
		uint32		i;

		ref_init(&r);
		lion_sparse_init(&b.c, 0);
		for (i = 0; i < 50; i++)
		{
			lion_sparse_insert(&b.c, 100 + i, (uint16) i, NULL);
			ref_insert(&r, 100 + i, (uint16) i);
		}
		pa.mod = 1000000;
		pa.rem = 999999;
		pa.ncalls = 0;
		CHECK(lion_sparse_remove_if(&b.c, pred_cb, &pa) == 0, "nothing matches");
		COMPARE(&b, &r);

		pa.mod = 1;
		pa.rem = 0;
		CHECK(lion_sparse_remove_if(&b.c, pred_cb, &pa) == 50, "everything matches");
		ref_init(&r);
		COMPARE(&b, &r);
		CHECK(b.c.cardinality == 0, "the segment is empty");
	}
}

static void
test_extract(void)
{
	SBuf		b;
	SBuf		cont;
	Ref			r;
	Ref			expect;
	uint32		i;
	uint32		n;

	phase("extract");
	ref_init(&r);
	lion_sparse_init(&b.c, 0);

	/* ckey 20 has three members, its neighbours one each */
	lion_sparse_insert(&b.c, 10, 5, NULL);
	ref_insert(&r, 10, 5);
	lion_sparse_insert(&b.c, 20, 300, NULL);
	ref_insert(&r, 20, 300);
	lion_sparse_insert(&b.c, 20, 100, NULL);
	ref_insert(&r, 20, 100);
	lion_sparse_insert(&b.c, 20, 200, NULL);
	ref_insert(&r, 20, 200);
	lion_sparse_insert(&b.c, 30, 7, NULL);
	ref_insert(&r, 30, 7);
	COMPARE(&b, &r);

	n = lion_sparse_extract(&b.c, 20, &cont.c);
	CHECK(n == 3, "extract moves every member of the ckey");
	CHECK(cont.c.type == LION_CT_ARRAY, "the extracted container is an array");
	CHECK(cont.c.ckey == 20, "the extracted container has the right ckey");
	CHECK(cont.c.cardinality == 3, "the extracted container has three members");
	CHECK(lion_container_contains(&cont.c, 100) &&
		  lion_container_contains(&cont.c, 200) &&
		  lion_container_contains(&cont.c, 300), "and holds the right members");

	expect = r;
	expect.n = 0;
	for (i = 0; i < r.n; i++)
	{
		if (r.p[i].ckey != 20)
			expect.p[expect.n++] = r.p[i];
	}
	COMPARE(&b, &expect);
	CHECK(lion_sparse_count(&b.c, 20) == 0, "the ckey is gone from the segment");

	/* extracting a ckey that is not there gives an empty container */
	n = lion_sparse_extract(&b.c, 25, &cont.c);
	CHECK(n == 0, "extracting an absent ckey moves nothing");
	CHECK(cont.c.cardinality == 0, "and produces an empty container");
	COMPARE(&b, &expect);

	/* extracting the first and the last ckey keeps the header honest */
	n = lion_sparse_extract(&b.c, 10, &cont.c);
	CHECK(n == 1, "extracting the first ckey");
	expect.n = 0;
	expect.p[expect.n].ckey = 30;
	expect.p[expect.n].lo = 7;
	expect.n++;
	COMPARE(&b, &expect);
	CHECK(b.c.ckey == 30, "the header moved to the new first ckey");

	n = lion_sparse_extract(&b.c, 30, &cont.c);
	CHECK(n == 1, "extracting the last ckey");
	ref_init(&expect);
	COMPARE(&b, &expect);
}

static void
test_split_half(void)
{
	SBuf		b;
	SBuf		l;
	SBuf		rr;
	Ref			r;
	uint32		i;

	phase("split_half");

	/* an empty or single-pair segment cannot be split */
	ref_init(&r);
	lion_sparse_init(&b.c, 0);
	CHECK(!lion_sparse_split_half(&b.c, &l.c, &rr.c), "an empty segment does not split");
	lion_sparse_insert(&b.c, 1, 1, NULL);
	CHECK(!lion_sparse_split_half(&b.c, &l.c, &rr.c), "a one-pair segment does not split");

	/* every pair of one ckey: no boundary to split at */
	lion_sparse_init(&b.c, 0);
	for (i = 0; i < 10; i++)
		lion_sparse_insert(&b.c, 42, (uint16) i, NULL);
	CHECK(!lion_sparse_split_half(&b.c, &l.c, &rr.c),
		  "a segment of one ckey does not split");

	/* the normal case: a full segment with three members per ckey */
	ref_init(&r);
	lion_sparse_init(&b.c, 0);
	for (i = 0; i < 2 * LION_SPARSE_MAX_PAIRS; i++)
	{
		uint32		ckey = 1000 + i / 3;
		uint16		lo = (uint16) (i % 3 + 1);

		if (!lion_sparse_insert(&b.c, ckey, lo, NULL))
			break;
		ref_insert(&r, ckey, lo);
	}
	COMPARE(&b, &r);
	CHECK(lion_sparse_insert_size(&b.c, 1000 + i / 3, (uint16) (i % 3 + 1)) == 0,
		  "the fixture is full");
	CHECK(lion_sparse_split_half(&b.c, &l.c, &rr.c), "a full segment splits");
	CHECK(l.c.cardinality + rr.c.cardinality == r.n, "the halves hold every pair");
	CHECK(l.c.cardinality > 0 && rr.c.cardinality > 0, "neither half is empty");
	CHECK(lion_item_last_ckey(&l.c) < lion_item_first_ckey(&rr.c),
		  "the halves do not overlap");
	CHECK(lion_sparse_nbytes(&l.c) < LION_SPARSE_MAX_BYTES - 2 * LION_SPARSE_VARINT_MAX &&
		  lion_sparse_nbytes(&rr.c) < LION_SPARSE_MAX_BYTES - 2 * LION_SPARSE_VARINT_MAX,
		  "both halves have room");

	{
		Ref			lref;
		Ref			rref;
		uint32		m = l.c.cardinality;

		ref_init(&lref);
		ref_init(&rref);
		for (i = 0; i < r.n; i++)
		{
			if (i < m)
				lref.p[lref.n++] = r.p[i];
			else
				rref.p[rref.n++] = r.p[i];
		}
		COMPARE(&l, &lref);
		COMPARE(&rr, &rref);
	}

	/* the pairs of the boundary ckey are all on one side */
	{
		uint32		bck = lion_item_last_ckey(&l.c);

		CHECK(lion_sparse_count(&rr.c, bck) == 0,
			  "the boundary ckey is not in the right half");
	}
}

static void
test_split_at(void)
{
	SBuf		b;
	SBuf		l;
	SBuf		rr;
	Ref			r;
	Ref			lref;
	Ref			rref;
	uint32		i;

	phase("split_at");
	ref_init(&r);
	lion_sparse_init(&b.c, 0);
	for (i = 0; i < 20; i++)
	{
		lion_sparse_insert(&b.c, 100 + i * 2, (uint16) (i + 1), NULL);
		ref_insert(&r, 100 + i * 2, (uint16) (i + 1));
	}

	/* split in the middle, at a ckey the segment does not hold */
	lion_sparse_split_at(&b.c, 121, &l.c, &rr.c);
	ref_init(&lref);
	ref_init(&rref);
	for (i = 0; i < r.n; i++)
	{
		if (r.p[i].ckey < 121)
			lref.p[lref.n++] = r.p[i];
		else
			rref.p[rref.n++] = r.p[i];
	}
	COMPARE(&l, &lref);
	COMPARE(&rr, &rref);
	CHECK(l.c.cardinality + rr.c.cardinality == r.n, "split_at keeps every pair");

	/* split before the first ckey: the left part is empty */
	lion_sparse_split_at(&b.c, 1, &l.c, &rr.c);
	CHECK(l.c.cardinality == 0, "nothing below the first ckey");
	COMPARE(&rr, &r);

	/* split after the last ckey: the right part is empty */
	lion_sparse_split_at(&b.c, 100000, &l.c, &rr.c);
	CHECK(rr.c.cardinality == 0, "nothing above the last ckey");
	COMPARE(&l, &r);
}

static void
test_merge(void)
{
	SBuf		a;
	SBuf		b;
	SBuf		out;
	Ref			ra;
	Ref			rb;
	Ref			rout;
	uint32		i;

	phase("merge");
	ref_init(&ra);
	ref_init(&rb);
	lion_sparse_init(&a.c, 0);
	lion_sparse_init(&b.c, 0);

	for (i = 0; i < 10; i++)
	{
		lion_sparse_insert(&a.c, 10 + i, (uint16) i, NULL);
		ref_insert(&ra, 10 + i, (uint16) i);
		lion_sparse_insert(&b.c, 100 + i, (uint16) (i * 3), NULL);
		ref_insert(&rb, 100 + i, (uint16) (i * 3));
	}

	CHECK(lion_sparse_merge(&a.c, &b.c, &out.c), "adjacent segments merge");
	rout = ra;
	for (i = 0; i < rb.n; i++)
		rout.p[rout.n++] = rb.p[i];
	COMPARE(&out, &rout);

	CHECK(!lion_sparse_merge(&b.c, &a.c, &out.c), "the wrong order does not merge");

	/* touching ckeys do not merge: that would put one ckey in two items */
	{
		SBuf		c;

		lion_sparse_init(&c.c, 0);
		lion_sparse_insert(&c.c, 19, 1, NULL);
		lion_sparse_insert(&c.c, 20, 1, NULL);
		CHECK(!lion_sparse_merge(&a.c, &c.c, &out.c),
			  "segments sharing a ckey do not merge");
	}

	/* too big to merge */
	{
		SBuf		x;
		SBuf		y;

		lion_sparse_init(&x.c, 0);
		lion_sparse_init(&y.c, 0);
		for (i = 0; i < 200; i++)
			lion_sparse_insert(&x.c, 1, (uint16) i, NULL);
		for (i = 0; i < 200; i++)
			lion_sparse_insert(&y.c, 2, (uint16) i, NULL);
		CHECK(!lion_sparse_merge(&x.c, &y.c, &out.c), "400 pairs do not fit one segment");
		/*
		 * x is 200 bytes; y's first pair is 32569 past x's last, three
		 * bytes, and each later one a byte: 53 pairs of y make 255.
		 */
		for (i = 0; i < 147; i++)
			lion_sparse_remove(&y.c, 2, (uint16) (199 - i));
		CHECK(lion_sparse_merge(&x.c, &y.c, &out.c), "253 pairs in 255 bytes do");
		CHECK(out.c.cardinality == 253 &&
			  lion_sparse_nbytes(&out.c) == LION_SPARSE_MAX_BYTES,
			  "and fill the segment exactly");
		lion_sparse_insert(&y.c, 2, 53, NULL);
		CHECK(!lion_sparse_merge(&x.c, &y.c, &out.c), "a byte more does not");
	}

	/* merging with an empty segment */
	{
		SBuf		e;

		lion_sparse_init(&e.c, 0);
		CHECK(lion_sparse_merge(&a.c, &e.c, &out.c), "merging an empty tail");
		COMPARE(&out, &ra);
		CHECK(lion_sparse_merge(&e.c, &a.c, &out.c), "merging an empty head");
		COMPARE(&out, &ra);
	}
}

static void
test_iterate_early_stop(void)
{
	SBuf		b;
	Ref			r;
	IterState	it;
	uint32		i;

	phase("iterate early stop");
	ref_init(&r);
	lion_sparse_init(&b.c, 0);
	for (i = 0; i < 30; i++)
	{
		lion_sparse_insert(&b.c, 7 + i, (uint16) (i * 5), NULL);
		ref_insert(&r, 7 + i, (uint16) (i * 5));
	}

	it.ref = &r;
	it.i = 0;
	it.ok = true;
	it.stop_after = 10;
	lion_sparse_iterate(&b.c, iter_cb, &it);
	CHECK(it.ok, "the pairs seen before the stop were right");
	CHECK(it.i == 10, "iterate() stops when the callback says so");
}

/*
 * check() has to reject corrupted images, not crash on them.
 */
static void
test_check_rejects(void)
{
	SBuf		b;
	const char *err;
	uint32		i;

	phase("check rejects");
	lion_sparse_init(&b.c, 0);
	for (i = 0; i < 10; i++)
		lion_sparse_insert(&b.c, 100 + i / 2, (uint16) (i * 3 + 200), NULL);

	err = NULL;
	CHECK(lion_sparse_check(&b.c, lion_sparse_size(&b.c), &err), "the good image passes");
	/* the fixture: pair 0 is (100, 200), a two-byte varint; pair 1 a one-byte delta 3 */
	CHECK(LION_SPARSE_STREAM(&b.c)[0] == (uint8) (0x80 | (200 & 0x7F)) &&
		  LION_SPARSE_STREAM(&b.c)[1] == (uint8) (200 >> 7) &&
		  LION_SPARSE_STREAM(&b.c)[2] == 3,
		  "the fixture's first pairs are what the format says");

	{
		SBuf		x = b;

		x.c.type = LION_CT_ARRAY;
		err = NULL;
		CHECK(!lion_sparse_check(&x.c, lion_sparse_size(&b.c), &err) && err != NULL,
			  "a container is not a segment");
	}
	{
		SBuf		x = b;

		x.c.cardinality = (uint16) (LION_SPARSE_MAX_PAIRS + 1);
		err = NULL;
		CHECK(!lion_sparse_check(&x.c, LION_CONTAINER_MAX_SIZE, &err) && err != NULL,
			  "too many pairs are rejected");
	}
	{
		SBuf		x = b;

		x.c.cardinality = (uint16) (lion_sparse_nbytes(&x.c) + 1);
		err = NULL;
		CHECK(!lion_sparse_check(&x.c, LION_CONTAINER_MAX_SIZE, &err) && err != NULL,
			  "more pairs than bytes are rejected");
	}
	{
		SBuf		x = b;

		x.c.cardinality--;
		err = NULL;
		CHECK(!lion_sparse_check(&x.c, lion_sparse_size(&b.c), &err) && err != NULL,
			  "a stream with bytes after the last pair the header counts is rejected");
	}
	{
		SBuf		x = b;

		x.c.cardinality++;
		err = NULL;
		CHECK(!lion_sparse_check(&x.c, lion_sparse_size(&b.c), &err) && err != NULL,
			  "a header counting more pairs than the stream holds is rejected");
	}
	{
		SBuf		x = b;

		x.c.flags--;
		err = NULL;
		CHECK(!lion_sparse_check(&x.c, lion_sparse_size(&b.c), &err) && err != NULL,
			  "a stream cut inside its last varint is rejected");
	}
	{
		SBuf		x = b;

		err = NULL;
		CHECK(!lion_sparse_check(&x.c, lion_sparse_size(&b.c) - 1, &err) && err != NULL,
			  "a segment that does not fit its slot is rejected");
		err = NULL;
		CHECK(!lion_sparse_check(&x.c, 4, &err) && err != NULL,
			  "a segment whose header does not fit is rejected");
	}
	{
		SBuf		x = b;

		uint8	   *st = LION_SPARSE_STREAM(&x.c);
		uint32		v = LION_CONTAINER_RANGE + 200;

		/*
		 * The same codes from a header ckey one lower: the first delta is
		 * then 32768 + 200, three bytes, and no lo.  (Changing the header
		 * ckey alone would shift every code and make another valid segment.)
		 */
		x.c.ckey = 99;
		memmove(st + 3, st + 2, lion_sparse_nbytes(&x.c) - 2);
		st[0] = (uint8) (0x80 | (v & 0x7F));
		st[1] = (uint8) (0x80 | ((v >> 7) & 0x7F));
		st[2] = (uint8) (v >> 14);
		x.c.flags++;
		err = NULL;
		CHECK(!lion_sparse_check(&x.c, lion_sparse_size(&x.c), &err) && err != NULL,
			  "a header key that is not the first ckey is rejected");
	}
	{
		SBuf		x = b;

		/* a delta of 0: a duplicate pair, which unique codes never make */
		LION_SPARSE_STREAM(&x.c)[2] = 0;
		err = NULL;
		CHECK(!lion_sparse_check(&x.c, lion_sparse_size(&b.c), &err) && err != NULL,
			  "a zero delta (a duplicate pair) is rejected");
	}
	{
		SBuf		x = b;
		uint8	   *st = LION_SPARSE_STREAM(&x.c);

		/* the one-byte delta 3 written long, as 0x83 0x00: one byte more */
		memmove(st + 4, st + 3, lion_sparse_nbytes(&x.c) - 3);
		st[2] = 0x83;
		st[3] = 0x00;
		x.c.flags++;
		err = NULL;
		CHECK(!lion_sparse_check(&x.c, lion_sparse_size(&x.c), &err) && err != NULL,
			  "a delta not in its shortest form is rejected");
	}
	{
		SBuf		x = b;
		uint8	   *st = LION_SPARSE_STREAM(&x.c);
		uint32		k;

		/* a delta of more bytes than any code needs */
		x.c.cardinality = 2;
		st[2] = 0x81;
		for (k = 3; k < 2 + LION_SPARSE_VARINT_MAX; k++)
			st[k] = 0x80;
		st[k] = 0x01;
		x.c.flags = (uint8) (k + 1);
		err = NULL;
		CHECK(!lion_sparse_check(&x.c, lion_sparse_size(&x.c), &err) && err != NULL,
			  "a delta longer than LION_SPARSE_VARINT_MAX bytes is rejected");
	}

	/* the container check must refuse a segment, and say so */
	{
		err = NULL;
		CHECK(!lion_container_check(&b.c, LION_CONTAINER_MAX_SIZE, &err) && err != NULL,
			  "lion_container_check() rejects a sparse segment");
		if (err != NULL)
			CHECK(strstr(err, "sparse") != NULL,
				  "... with a message that names the segment");
	}
}

/*
 * lion_sparse_check_offsets(): every pair's lo is a heap tuple of its block,
 * offsets 1 .. MaxHeapTuplesPerPage - which this frontend program computes
 * for its BLCKSZ as the server does, 291 at 8K.
 */
#define TEST_MAXOFF		((uint32) ((BLCKSZ - 24) / (24 + 4)))

static uint16
tuple_lo(uint32 blk, uint32 off)
{
	return (uint16) ((blk << LION_OFFSET_BITS) | off);
}

static void
test_check_offsets(void)
{
	SBuf		b;
	const char *err;
	uint32		maxlo = (1U << LION_OFFSET_BITS) - 1;
	int			iter;

	phase("check_offsets");
	lion_sparse_init(&b.c, 0);
	lion_sparse_insert(&b.c, 100, tuple_lo(0, 1), NULL);
	lion_sparse_insert(&b.c, 100, tuple_lo(0, TEST_MAXOFF), NULL);
	lion_sparse_insert(&b.c, 101, tuple_lo(3, 7), NULL);
	lion_sparse_insert(&b.c, 250, tuple_lo(LION_BLOCKS_PER_CONTAINER - 1, 1), NULL);

	err = NULL;
	CHECK(lion_sparse_check(&b.c, lion_sparse_size(&b.c), &err), "the good image passes");
	CHECK(lion_sparse_check_offsets(&b.c, TEST_MAXOFF, &err) && err == NULL,
		  "pairs of tuples pass");

	{
		SBuf		x = b;

		lion_sparse_insert(&x.c, 101, tuple_lo(1, 0), NULL);
		err = NULL;
		CHECK(lion_sparse_check(&x.c, lion_sparse_size(&x.c), &err),
			  "offset 0 is a structurally valid lo");
		CHECK(!lion_sparse_check_offsets(&x.c, TEST_MAXOFF, &err) && err != NULL,
			  "a pair at offset 0 is rejected");
	}
	{
		SBuf		x = b;

		lion_sparse_insert(&x.c, 250, tuple_lo(2, TEST_MAXOFF + 1), NULL);
		err = NULL;
		CHECK(!lion_sparse_check_offsets(&x.c, TEST_MAXOFF, &err) && err != NULL,
			  "a pair past MaxHeapTuplesPerPage is rejected");
	}
	{
		SBuf		x = b;

		lion_sparse_insert(&x.c, 99, tuple_lo(0, maxlo), NULL);
		err = NULL;
		CHECK(!lion_sparse_check_offsets(&x.c, TEST_MAXOFF, &err) && err != NULL,
			  "a pair at the last lo of a block is rejected");
	}

	/* the answer is "every lo is a tuple", for any segment */
	for (iter = 0; iter < 500; iter++)
	{
		uint32		n = 1 + rng_below(60);
		bool		expect = true;
		uint32		i;

		lion_sparse_init(&b.c, 0);
		for (i = 0; i < n; i++)
		{
			uint32		off = 1 + rng_below(TEST_MAXOFF);
			uint16		lo;

			/* now and then one that is not a tuple: offset 0, or any */
			if (rng_below(40) == 0)
				off = rng_below(2) ? 0 : rng_below(maxlo + 1);
			lo = tuple_lo(rng_below(LION_BLOCKS_PER_CONTAINER), off);

			if (lion_sparse_insert(&b.c, rng_below(1000), lo, NULL) &&
				(off == 0 || off > TEST_MAXOFF))
				expect = false;
		}
		err = NULL;
		CHECK(lion_sparse_check_offsets(&b.c, TEST_MAXOFF, &err) == expect,
			  "check_offsets() agrees with the pairs, one by one");
	}
}

/* ----------------------------------------------------------------
 *		damaged segments, and growth in place (DESIGN.md §13)
 *
 * As in container_test.c: every buffer is a heap allocation of exactly the
 * size under test, so that -fsanitize=address reports any access past it,
 * and otherwise a guard of LION_CONTAINER_MAX_SIZE bytes follows it that
 * guard_ok() checks.
 * ----------------------------------------------------------------
 */

#if defined(__SANITIZE_ADDRESS__)
#define UNIT_ASAN 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define UNIT_ASAN 1
#endif
#endif

/* a variable, not a macro: 0 would make the loops below "always false" */
#ifdef UNIT_ASAN
static Size guard_bytes = 0;
#else
static Size guard_bytes = LION_CONTAINER_MAX_SIZE;
#endif
#define GUARD_FILL	0xA5

static void *
exact_alloc(Size size)
{
	unsigned char *p = malloc(size + guard_bytes);
	Size		i;

	if (p == NULL)
	{
		printf("out of memory\n");
		exit(2);
	}
	for (i = 0; i < guard_bytes; i++)
		p[size + i] = GUARD_FILL;
	return p;
}

static bool
guard_ok(const void *p, Size size)
{
	const unsigned char *g = (const unsigned char *) p + size;
	Size		i;

	for (i = 0; i < guard_bytes; i++)
		if (g[i] != GUARD_FILL)
			return false;
	return true;
}

static uint32 damage_pairs;
static uint32 damage_bad_lo;	/* lo values handed out past the range */

static bool
damage_count_cb(uint32 ckey, uint16 lo, void *arg)
{
	damage_pairs++;
	if ((uint32) lo >= LION_CONTAINER_RANGE)
		damage_bad_lo++;
	return true;
}

static uint32 damage_seed;

static bool
damage_pair_pred(uint32 ckey, uint16 lo, void *arg)
{
	damage_pairs++;
	if ((uint32) lo >= LION_CONTAINER_RANGE)
		damage_bad_lo++;
	return (((ckey ^ lo) * 2654435761U ^ damage_seed) >> 30) == 0;
}

/*
 * What a caller does with an iterated lo: index a bitset image of exactly
 * LION_BITSET_BYTES with it, as the window scan's lion_window_pair_cb() does.
 */
static uint64 *damage_img;

static bool
damage_img_cb(uint32 ckey, uint16 lo, void *arg)
{
	damage_img[lo >> 6] |= UINT64CONST(1) << (lo & 63);
	return true;
}

static bool
damage_lo_cb(uint16 lo, void *arg)
{
	if ((uint32) lo >= LION_CONTAINER_RANGE)
		(*(uint32 *) arg)++;
	return true;
}

/*
 * Every function of the segment library over s, which has
 * LION_SPARSE_MAX_SIZE bytes, with work buffers of exactly that size,
 * checking what is promised for any bytes: no access outside a buffer, at
 * most LION_SPARSE_MAX_PAIRS pairs looked at, and a segment a mutator leaves
 * behind no larger than LION_SPARSE_MAX_SIZE.  s is left unchanged.
 */
static void
damage_exercise_segment(const LionContainer *s, LionContainer *work,
						LionContainer *left, LionContainer *right,
						LionContainer *merged, LionContainer *cont)
{
	uint32		ckey = s->ckey + rng_below(64);
	uint16		lo = (uint16) rng_below(LION_CONTAINER_RANGE);
	uint32		first;
	uint32		count;
	uint32		bad = 0;

	(void) lion_sparse_contains(s, ckey, lo);
	lion_sparse_find(s, ckey, &first, &count);
	CHECK(first + count <= LION_SPARSE_MAX_PAIRS, "damaged: find() stays within the pairs");
	(void) lion_sparse_count(s, ckey + 1);
	(void) lion_sparse_insert_size(s, ckey, lo);
	damage_pairs = 0;
	damage_bad_lo = 0;
	lion_sparse_iterate(s, damage_count_cb, NULL);
	CHECK(damage_pairs <= LION_SPARSE_MAX_PAIRS, "damaged: iterate() visits at most 255 pairs");
	CHECK(damage_bad_lo == 0, "damaged: iterate() hands out only lo values in range");
	memset(damage_img, 0, LION_BITSET_BYTES);
	lion_sparse_iterate(s, damage_img_cb, NULL);
	CHECK(guard_ok(damage_img, LION_BITSET_BYTES),
		  "damaged: an iterated lo indexes a 4096-byte image inside it");

	/* an empty segment covers its header ckey, and nothing reads a pair */
	if (s->cardinality == 0)
		CHECK(lion_item_last_ckey(s) == s->ckey,
			  "damaged: an empty segment's last ckey is its header ckey");
	else
		(void) lion_item_last_ckey(s);
	(void) lion_item_covers(s, ckey);
	{
		const char *err = NULL;

		if (s->cardinality == 0)
			CHECK(!lion_sparse_check(s, LION_SPARSE_MAX_SIZE, &err) && err != NULL,
				  "damaged: check() refuses an empty segment");
		else
			(void) lion_sparse_check(s, LION_SPARSE_MAX_SIZE, &err);
	}

	if (lion_sparse_split_half(s, left, right))
		CHECK(lion_sparse_size(left) <= LION_SPARSE_MAX_SIZE &&
			  lion_sparse_size(right) <= LION_SPARSE_MAX_SIZE,
			  "damaged: split_half() halves fit a work buffer");
	CHECK(guard_ok(left, LION_SPARSE_MAX_SIZE) && guard_ok(right, LION_SPARSE_MAX_SIZE),
		  "damaged: split_half() stays inside its buffers");
	lion_sparse_split_at(s, ckey, left, right);
	CHECK(guard_ok(left, LION_SPARSE_MAX_SIZE) && guard_ok(right, LION_SPARSE_MAX_SIZE),
		  "damaged: split_at() stays inside its buffers");
	(void) lion_sparse_merge(left, right, merged);
	CHECK(guard_ok(merged, LION_SPARSE_MAX_SIZE), "damaged: merge() stays inside its buffer");
	(void) lion_sparse_merge(s, s, merged);
	CHECK(guard_ok(merged, LION_SPARSE_MAX_SIZE), "damaged: merge() stays inside its buffer");

#define DAMAGE_MUTATE(what, stmt) \
	do { \
		memcpy(work, s, LION_SPARSE_MAX_SIZE); \
		stmt; \
		CHECK(lion_sparse_size(work) <= LION_SPARSE_MAX_SIZE, \
			  "damaged: " what " leaves a segment of at most 263 bytes"); \
		CHECK(guard_ok(work, LION_SPARSE_MAX_SIZE), \
			  "damaged: " what " stays inside its buffer"); \
	} while (0)

	DAMAGE_MUTATE("insert()", (void) lion_sparse_insert(work, ckey, lo, NULL));
	{
		Size		need = lion_sparse_insert_size(s, ckey, lo);

		memcpy(work, s, LION_SPARSE_MAX_SIZE);
		if (need != 0 && lion_sparse_insert(work, ckey, lo, NULL))
			CHECK(lion_sparse_size(work) == need ||
				  lion_sparse_size(work) == lion_sparse_size(s),
				  "damaged: insert() writes what insert_size() said");
	}
	DAMAGE_MUTATE("remove()", (void) lion_sparse_remove(work, ckey, lo));
	damage_seed = rng_next();
	damage_pairs = 0;
	damage_bad_lo = 0;
	DAMAGE_MUTATE("remove_if()", (void) lion_sparse_remove_if(work, damage_pair_pred, NULL));
	CHECK(damage_pairs <= LION_SPARSE_MAX_PAIRS,
		  "damaged: remove_if() asks about at most 255 pairs");
	CHECK(damage_bad_lo == 0,
		  "damaged: remove_if() asks only about lo values in range");
	DAMAGE_MUTATE("extract()", (void) lion_sparse_extract(work, ckey, cont));
	CHECK(lion_container_size(cont) <= LION_CONTAINER_MAX_SIZE &&
		  guard_ok(cont, LION_CONTAINER_MAX_SIZE),
		  "damaged: extract() builds a container inside its buffer");
	lion_container_iterate(cont, damage_lo_cb, &bad);
	CHECK(bad == 0, "damaged: extract() builds a container of lo values in range");
#undef DAMAGE_MUTATE
}

static void
test_damaged(uint32 iters, uint64 seed)
{
	LionContainer *s = exact_alloc(LION_SPARSE_MAX_SIZE);
	LionContainer *work = exact_alloc(LION_SPARSE_MAX_SIZE);
	LionContainer *left = exact_alloc(LION_SPARSE_MAX_SIZE);
	LionContainer *right = exact_alloc(LION_SPARSE_MAX_SIZE);
	LionContainer *merged = exact_alloc(LION_SPARSE_MAX_SIZE);
	LionContainer *cont = exact_alloc(LION_CONTAINER_MAX_SIZE);
	uint32		it;
	uint32		i;

	damage_img = exact_alloc(LION_BITSET_BYTES);

	/*
	 * The case a 2026-09 review found for the old fixed-width format:
	 * VACUUM copies a page item into a work buffer by its line pointer's
	 * length, and a header claiming more pairs than there are must not walk
	 * any function past the stream.
	 */
	phase("damaged: a segment claiming 1000 pairs, filtered");
	rng_seed(seed);
	lion_sparse_init(s, 0);
	for (i = 0; i < 2 * LION_SPARSE_MAX_PAIRS; i++)
		(void) lion_sparse_insert(s, i / 2, (uint16) (i * 7), NULL);
	s->cardinality = 1000;
	memcpy(work, s, LION_SPARSE_MAX_SIZE);
	damage_seed = 0;
	(void) lion_sparse_remove_if(work, damage_pair_pred, NULL);
	CHECK(work->cardinality <= LION_SPARSE_MAX_PAIRS, "remove_if() leaves at most 255 pairs");
	CHECK(guard_ok(work, LION_SPARSE_MAX_SIZE), "remove_if() stays inside its buffer");
	damage_exercise_segment(s, work, left, right, merged, cont);
	s->cardinality = 65535;
	damage_exercise_segment(s, work, left, right, merged, cont);

	/*
	 * Deltas that wrap the code around: pairs out of order and repeated, as
	 * the damaged fixed-width segments had them.
	 */
	phase("damaged: a stream of wrapping deltas");
	lion_sparse_init(s, 5);
	{
		uint8	   *st = LION_SPARSE_STREAM(s);
		uint32		k = 0;

		while (k + 10 <= LION_SPARSE_MAX_BYTES)
		{
			uint32		j;

			/* ten-byte varints of 2^64 - 1 - rng: a step backwards */
			uint64		v = ~((uint64) rng_below(1 << 20));

			for (j = 0; j < 9; j++)
			{
				st[k++] = (uint8) (v | 0x80);
				v >>= 7;
			}
			st[k++] = (uint8) v;
			s->cardinality++;
		}
		s->flags = (uint8) k;
	}
	damage_exercise_segment(s, work, left, right, merged, cont);

	/*
	 * An empty segment, whose last ckey was read 16 GiB past it in the old
	 * format (2026-09-27 review).
	 */
	phase("damaged: an empty segment");
	for (i = 0; i < LION_SPARSE_MAX_SIZE; i++)
		((char *) s)[i] = (char) 0xFF;
	lion_sparse_init(s, 12345);
	CHECK(lion_item_last_ckey(s) == 12345 && lion_item_covers(s, 12345) &&
		  !lion_item_covers(s, 12346),
		  "an empty segment covers its header ckey alone");
	damage_pairs = 0;
	lion_sparse_iterate(s, damage_count_cb, NULL);
	CHECK(damage_pairs == 0, "iterate() of an empty segment visits nothing");
	damage_exercise_segment(s, work, left, right, merged, cont);

	/* a stream that ends inside a varint: every byte has the high bit */
	phase("damaged: a stream of continuation bytes");
	lion_sparse_init(s, 77);
	memset(LION_SPARSE_STREAM(s), 0xFF, LION_SPARSE_MAX_BYTES);
	s->flags = LION_SPARSE_MAX_BYTES;
	s->cardinality = 3;
	damage_pairs = 0;
	lion_sparse_iterate(s, damage_count_cb, NULL);
	CHECK(damage_pairs == 0, "iterate() of a stream with no last byte visits nothing");
	damage_exercise_segment(s, work, left, right, merged, cont);

	phase("damaged: random segments");
	for (it = 0; it < iters; it++)
	{
		for (i = 0; i < LION_SPARSE_MAX_SIZE; i++)
			((char *) s)[i] = (char) rng_next();
		s->type = LION_CT_SPARSE;
		s->cardinality = (uint16) ((it % 3 == 0) ? rng_below(LION_SPARSE_MAX_PAIRS + 1) : rng_next());
		if (it % 3 == 1)
		{
			/* plausible: short deltas, most of them one byte */
			for (i = 0; i < LION_SPARSE_MAX_BYTES; i++)
				LION_SPARSE_STREAM(s)[i] = (uint8) (rng_below(8) == 0 ?
													0x80 | rng_next() : rng_below(128));
		}
		damage_exercise_segment(s, work, left, right, merged, cont);
	}

	free(s);
	free(work);
	free(left);
	free(right);
	free(merged);
	free(cont);
	free(damage_img);
}

/*
 * lion_sparse.h's contract for the in-place insert and its WAL redo
 * (lion_insert_segment_inplace(), LION_OP_SPARSE_INS): insert() writes
 * exactly lion_sparse_insert_size() bytes and nothing past them.  Each
 * insert runs on an allocation of exactly that, and must leave the bytes an
 * insert into a full-size buffer does.
 */
static void
test_inplace_growth(void)
{
	SBuf		full;
	SBuf		ref;
	uint32		k;
	uint32		i;

	phase("insert() within the in-place growth contract");
	rng_seed(UINT64CONST(0x5EED2100));
	for (k = 0; k < 400; k++)
	{
		uint32		n = rng_below(LION_SPARSE_MAX_PAIRS);
		uint32		spread = (k % 2) ? 2 * n + 1 : 50000;
		LionContainer *p;
		uint32		ckey;
		uint16		lo;

		lion_sparse_init(&ref.c, 0);
		for (i = 0; i < n; i++)
			(void) lion_sparse_insert(&ref.c, 1000 + rng_below(spread),
									  (uint16) rng_below(LION_CONTAINER_RANGE), NULL);

		for (i = 0; i < 4; i++)
		{
			Size		alloc;
			bool		dup1 = false;
			bool		dup2 = false;
			bool		r1;
			bool		r2;

			/* before, among and after the ckeys there are */
			ckey = (i == 0) ? 999 : ((i == 3) ? 1000 + spread : 1000 + rng_below(spread));
			lo = (uint16) rng_below(LION_CONTAINER_RANGE);
			alloc = lion_sparse_insert_size(&ref.c, ckey, lo);

			memcpy(&full, &ref, sizeof(full));
			r2 = lion_sparse_insert(&full.c, ckey, lo, &dup2);
			if (alloc == 0)
			{
				CHECK(!r2, "insert() refuses what insert_size() says does not fit");
				continue;
			}

			p = exact_alloc(alloc);
			memcpy(p, &ref.c, Min(alloc, lion_sparse_size(&ref.c)));
			r1 = lion_sparse_insert(p, ckey, lo, &dup1);
			CHECK(r1 && r2 && dup1 == dup2, "insert() in place succeeds as it does in a full buffer");
			CHECK(lion_sparse_size(p) == alloc, "insert() in place fills what insert_size() said");
			CHECK(guard_ok(p, alloc), "insert() in place writes nothing past the item");
			CHECK(memcmp(p, &full, lion_sparse_size(p)) == 0,
				  "insert() in place leaves the bytes it leaves in a full buffer");
			free(p);
		}
	}
}

/*
 * Randomized insert/remove/extract/split/merge sequences against the
 * reference.
 */
static void
test_random_ops(const char *name, uint32 nckeys, uint32 nops, uint64 seed)
{
	SBuf		b;
	Ref			r;
	uint32		op;

	phase(name);
	rng_seed(seed);
	ref_init(&r);
	lion_sparse_init(&b.c, 0);

	for (op = 0; op < nops; op++)
	{
		uint32		ckey = 1000 + rng_below(nckeys);
		uint16		lo = (uint16) rng_below(LION_CONTAINER_RANGE);
		uint32		what = rng_below(100);

		if (what < 55)
		{
			bool		dup = false;
			Size		need = lion_sparse_insert_size(&b.c, ckey, lo);
			bool		ok = lion_sparse_insert(&b.c, ckey, lo, &dup);
			bool		refok = ref_insert(&r, ckey, lo);

			if (ok && !dup)
				CHECK(refok && need == lion_sparse_size(&b.c),
					  "insert agrees with the reference and with insert_size()");
			else if (dup)
				CHECK(!refok && ref_contains(&r, ckey, lo),
					  "a duplicate is a duplicate for the reference too");
			else
				CHECK(!refok && need == 0, "insert only fails when full");
		}
		else if (what < 80)
		{
			bool		ok = lion_sparse_remove(&b.c, ckey, lo);
			bool		refok = ref_remove(&r, ckey, lo);

			CHECK(ok == refok, "remove agrees with the reference");
		}
		else if (what < 88)
		{
			SBuf		cont;
			uint32		expect = ref_count(&r, ckey);
			uint32		got = lion_sparse_extract(&b.c, ckey, &cont.c);
			Ref			after;
			uint32		i;

			CHECK(got == expect, "extract moves as many members as the reference has");
			CHECK(cont.c.cardinality == expect, "the container holds them all");

			ref_init(&after);
			for (i = 0; i < r.n; i++)
			{
				if (r.p[i].ckey == ckey)
					CHECK(lion_container_contains(&cont.c, r.p[i].lo),
						  "the container holds the right members");
				else
					after.p[after.n++] = r.p[i];
			}
			r = after;
		}
		else if (what < 94)
		{
			SBuf		l;
			SBuf		rr;

			if (lion_sparse_split_half(&b.c, &l.c, &rr.c))
			{
				Ref			lref;
				Ref			rref;
				uint32		i;
				uint32		m = l.c.cardinality;

				CHECK(l.c.cardinality + rr.c.cardinality == r.n,
					  "the halves hold every pair");
				CHECK(lion_item_last_ckey(&l.c) < lion_item_first_ckey(&rr.c),
					  "the halves do not overlap");

				ref_init(&lref);
				ref_init(&rref);
				for (i = 0; i < r.n; i++)
				{
					if (i < m)
						lref.p[lref.n++] = r.p[i];
					else
						rref.p[rref.n++] = r.p[i];
				}
				COMPARE(&l, &lref);
				COMPARE(&rr, &rref);

				/* ... and merging them again gives back the original */
				{
					SBuf		out;

					CHECK(lion_sparse_merge(&l.c, &rr.c, &out.c),
						  "the halves merge back together");
					COMPARE(&out, &r);
				}
			}
			else
				CHECK(r.n < 2 || r.p[0].ckey == r.p[r.n - 1].ckey,
					  "split only fails on one ckey or too few pairs");
		}
		else
		{
			SBuf		l;
			SBuf		rr;
			Ref			lref;
			Ref			rref;
			uint32		i;
			uint32		cut = 1000 + rng_below(nckeys);

			/* split_at needs the ckey gone from the segment */
			{
				SBuf		cont;

				lion_sparse_extract(&b.c, cut, &cont.c);
				{
					Ref			after;

					ref_init(&after);
					for (i = 0; i < r.n; i++)
					{
						if (r.p[i].ckey != cut)
							after.p[after.n++] = r.p[i];
					}
					r = after;
				}
			}

			lion_sparse_split_at(&b.c, cut, &l.c, &rr.c);
			ref_init(&lref);
			ref_init(&rref);
			for (i = 0; i < r.n; i++)
			{
				if (r.p[i].ckey < cut)
					lref.p[lref.n++] = r.p[i];
				else
					rref.p[rref.n++] = r.p[i];
			}
			COMPARE(&l, &lref);
			COMPARE(&rr, &rref);
		}

		if ((op % 32) == 0)
			COMPARE(&b, &r);
	}

	COMPARE(&b, &r);
}

/*
 * What the format buys, printed for the record.
 */
static void
report_sizes(void)
{
	SBuf		b;
	uint32		i;
	uint64		code;

	printf("\nsizes (bytes):\n");
	printf("  %-52s %6zu\n", "segment header", (size_t) LION_CONTAINER_HDRSZ);
	printf("  %-52s %6zu\n", "largest segment (255 bytes of stream)",
		   (size_t) LION_SPARSE_MAX_SIZE);
	printf("  %-52s %6zu\n", "one pair, largest", (size_t) LION_SPARSE_SINGLETON_MAX);
	printf("  %-52s %6u\n", "longest delta", (unsigned) LION_SPARSE_VARINT_MAX);

	/* pairs 512 heap blocks apart (a key of ~1 row in 25,000) */
	lion_sparse_init(&b.c, 0);
	code = 0;
	for (i = 0; lion_sparse_insert(&b.c, (uint32) (code >> LION_CONTAINER_BITS),
								   (uint16) (code & LION_LO_MASK), NULL); i++)
		code += ((uint64) 512) << LION_OFFSET_BITS;
	printf("  %-52s %6zu (%u pairs)\n", "full segment, rows 512 heap blocks apart",
		   (size_t) lion_sparse_size(&b.c), (unsigned) b.c.cardinality);
	printf("  %-52s %6zu\n", "the same pairs as uint32 ckey + uint16 lo",
		   (size_t) (LION_CONTAINER_HDRSZ + 6 * b.c.cardinality));
	printf("  %-52s %6zu\n", "3 one-member ARRAY containers instead of 3 pairs",
		   (size_t) (3 * MAXALIGN(LION_CONTAINER_HDRSZ + sizeof(uint16)) + 3 * 4));
}

int
main(void)
{
	printf("pg_lion sparse segment unit tests\n");
	printf("  LION_SPARSE_MAX_PAIRS=%u LION_SPARSE_MAX_BYTES=%d "
		   "LION_SPARSE_THRESHOLD=%d LION_SPARSE_VARINT_MAX=%d\n",
		   LION_SPARSE_MAX_PAIRS, LION_SPARSE_MAX_BYTES, LION_SPARSE_THRESHOLD,
		   (int) LION_SPARSE_VARINT_MAX);

	CHECK(sizeof(LionContainer) == 8, "LionContainer header is 8 bytes");
	CHECK(LION_SPARSE_MAX_PAIRS == 255, "LION_SPARSE_MAX_PAIRS is 255");
	CHECK(LION_SPARSE_MAX_SIZE <= LION_CONTAINER_MAX_SIZE,
		  "a full segment fits a container work buffer");
	CHECK(lion_sparse_varint_size(LION_SPARSE_MAX_CODE) == LION_SPARSE_VARINT_MAX,
		  "LION_SPARSE_VARINT_MAX is the varint of the largest code");

	test_empty();
	test_one_pair();
	test_boundaries();
	test_insert_positions();
	test_append_tail();
	test_append();

	rng_seed(UINT64CONST(0x5EED2000));
	test_remove();
	test_remove_if();
	test_extract();
	test_split_half();
	test_split_at();
	test_merge();
	test_iterate_early_stop();
	test_check_rejects();
	test_check_offsets();
	test_damaged(3000, UINT64CONST(0x5EED2200));
	test_inplace_growth();

	test_random_ops("random ops, 8 ckeys (dense)", 8, 20000,
					UINT64CONST(0x5EED2001));
	test_random_ops("random ops, 200 ckeys", 200, 20000,
					UINT64CONST(0x5EED2002));
	test_random_ops("random ops, 5000 ckeys (sparse)", 5000, 20000,
					UINT64CONST(0x5EED2003));
	test_random_ops("random ops, 4M ckeys (far apart)", 4000000, 20000,
					UINT64CONST(0x5EED2005));
	test_random_ops("random ops, 1 ckey", 1, 5000,
					UINT64CONST(0x5EED2004));

	report_sizes();

	printf("\n%ld checks, %ld pair comparisons against the reference, %ld failures\n",
		   nchecks, npair_cmps, nfail);
	if (nfail == 0)
		printf("ALL TESTS PASSED\n");
	else
		printf("*** %ld TESTS FAILED ***\n", nfail);
	return nfail == 0 ? 0 : 1;
}
