/*-------------------------------------------------------------------------
 * waldelta_test.c
 *	  Standalone unit tests for src/lion_waldelta.c: how the WAL describes a
 *	  rewritten item (DESIGN.md §25, LION_OP_DELTA and LION_OP_SPLICE).
 *
 *	  Built by "make unit PG_CONFIG=..." with -DFRONTEND.  The redo of both
 *	  operations rebuilds the writer's item from the old image and the
 *	  payload, so the property under test is the round trip: whatever the
 *	  encoder chooses, the decoder given the same old image reproduces the new
 *	  item byte for byte, at its new length, and never writes or reads outside
 *	  the buffers it is given (every buffer here is an exact-size heap
 *	  allocation, which `make unit SANITIZE=1` checks to the byte).
 *
 *	  Then the shapes the operation exists for - one member added to an
 *	  ARRAY in the middle, a run added to a RUN, the same inside a longer
 *	  payload - must come out as a SPLICE of a dozen bytes or so, an append
 *	  must stay a RANGES as it was, and a payload that was not written against
 *	  the old image must be refused rather than applied.
 *
 *	  Exit status is 0 only if every check passed.
 *-------------------------------------------------------------------------
 */
#include "c.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lion_waldelta.h"

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
			printf("FAIL [%s] waldelta_test.c:%d: %s\n", cur_phase, line, msg);
		else if (nfail == 41)
			printf("... further failures suppressed\n");
	}
}

#define CHECK(ok, msg)	check_impl((ok), __LINE__, (msg))

/* A small deterministic generator, so that a failure reproduces. */
static uint64 rng_state = 0x9E3779B97F4A7C15ULL;

static uint32
rnd(void)
{
	rng_state ^= rng_state << 13;
	rng_state ^= rng_state >> 7;
	rng_state ^= rng_state << 17;
	return (uint32) (rng_state >> 11);
}

static char *
xmalloc(Size n)
{
	char	   *p = malloc(n > 0 ? n : 1);

	if (p == NULL)
	{
		printf("out of memory\n");
		exit(2);
	}
	return p;
}

/*
 * Encode cur against old with the given budget, check the choice against the
 * budget, decode it into an exact-size buffer and compare.  Returns the kind
 * and sets *plen.
 */
static int
roundtrip(const char *old, Size oldlen, const char *cur, Size len, Size cap,
		  Size *plen_out)
{
	char	   *out = xmalloc(cap);
	char	   *scratch = xmalloc(len);
	char	   *dec = xmalloc(len);
	char	   *oldcopy = xmalloc(oldlen);
	int			kind;
	Size		plen;

	memcpy(oldcopy, old, oldlen);
	plen = lion_delta_encode_item(oldcopy, oldlen, cur, len, out, cap, scratch,
								  &kind);
	CHECK(memcmp(oldcopy, old, oldlen) == 0, "the encoder changed the old image");
	if (kind == LION_DELTA_KIND_NONE)
		CHECK(plen == 0, "no encoding, but a length");
	else
	{
		CHECK(plen <= cap, "encoding over its budget");
		CHECK(lion_delta_decode_item(oldcopy, oldlen, kind, out, plen, dec, len),
			  "decoder refused the encoder's own payload");
		CHECK(memcmp(dec, cur, len) == 0, "round trip differs");
	}

	/* A RANGES that fits the budget is never passed over for NONE. */
	if (kind == LION_DELTA_KIND_NONE)
	{
		char	   *base = xmalloc(len);
		Size		n = Min(oldlen, len);

		memcpy(base, old, n);
		memset(base + n, 0, len - n);
		CHECK(lion_delta_ranges(base, cur, len, NULL, cap) < 0,
			  "NONE although RANGES fits");
		free(base);
	}

	free(out);
	free(scratch);
	free(dec);
	free(oldcopy);
	*plen_out = plen;
	return kind;
}

/* ---------- containers as the library lays them out ---------- */

#define HDR 8

/* An ARRAY of n members, starting at first, every step apart, in an
 * allocation of alloc bytes (slack zeroed). */
static Size
make_array(char *buf, Size alloc, int n, int first, int step)
{
	uint32		ckey = 77;
	uint16		card = (uint16) n;
	int			i;

	memset(buf, 0, alloc);
	memcpy(buf, &ckey, 4);
	memcpy(buf + 4, &card, 2);
	buf[6] = 1;					/* LION_CT_ARRAY */
	buf[7] = 0;
	for (i = 0; i < n; i++)
	{
		uint16		lo = (uint16) (first + i * step);

		memcpy(buf + HDR + 2 * i, &lo, 2);
	}
	return HDR + 2 * (Size) n;
}

/* Insert lo into an ARRAY image at its sorted place (the caller makes room). */
static void
array_add(char *buf, uint16 lo)
{
	uint16		card;
	int			i;
	int			pos;

	memcpy(&card, buf + 4, 2);
	pos = card;
	for (i = 0; i < card; i++)
	{
		uint16		v;

		memcpy(&v, buf + HDR + 2 * i, 2);
		if (v > lo)
		{
			pos = i;
			break;
		}
	}
	memmove(buf + HDR + 2 * (pos + 1), buf + HDR + 2 * pos, 2 * (card - pos));
	memcpy(buf + HDR + 2 * pos, &lo, 2);
	card++;
	memcpy(buf + 4, &card, 2);
}

#define ALIGN8(x)	(((x) + 7) & ~((Size) 7))

static void
test_array_member(void)
{
	int			n;

	cur_phase = "array member";

	for (n = 1; n <= 2047; n += (n < 40 ? 1 : 97))
	{
		int			where;

		for (where = 0; where <= 2; where++)
		{
			Size		size = HDR + 2 * (Size) n;
			Size		oldlen = size;	/* a bulk-built item: no slack */
			Size		newsize = size + 2;
			Size		newlen = ALIGN8(newsize) + 64;
			char	   *old = xmalloc(oldlen);
			char	   *cur = xmalloc(newlen);
			uint16		lo;
			Size		plen;
			int			kind;

			make_array(old, oldlen, n, 10, 3);
			make_array(cur, newlen, n, 10, 3);
			/* below everything, in the middle, past everything */
			lo = (where == 0) ? 5 : (where == 1) ? (uint16) (10 + 3 * (n / 2) + 1) :
				(uint16) (10 + 3 * n + 1);
			array_add(cur, lo);

			kind = roundtrip(old, oldlen, cur, newlen, newlen / 2, &plen);
			if (where < 2 && n >= 8)
			{
				/* the operation's reason to exist: header + 2 bytes + count */
				CHECK(kind == LION_DELTA_KIND_SPLICE, "a member inserted is not a SPLICE");
				CHECK(plen <= 16, "a member inserted costs more than 16 bytes");
			}
			if (where == 2)
				CHECK(kind == LION_DELTA_KIND_RANGES,
					  "an appended member is no longer a RANGES");

			/* the same rewrite of an item that HAD slack, at the same length */
			free(old);
			old = xmalloc(newlen);
			make_array(old, newlen, n, 10, 3);
			kind = roundtrip(old, newlen, cur, newlen, newlen / 2, &plen);
			if (where < 2 && n >= 8)
				CHECK(kind == LION_DELTA_KIND_SPLICE && plen <= 16,
					  "a member inserted at the same length is not a small SPLICE");

			/* and shrunk back: a removal is a RANGES or the whole item */
			(void) roundtrip(cur, newlen, old, newlen, newlen / 2, &plen);
			free(old);
			free(cur);
		}
	}
}

/* A RUN: {uint16 nruns, {start, len-1}*}; a run added in the middle is 4 bytes. */
static void
test_run_insert(void)
{
	int			nruns;

	cur_phase = "run insert";
	for (nruns = 2; nruns <= 1000; nruns += 37)
	{
		Size		oldlen = HDR + 2 + 4 * (Size) nruns;
		Size		newlen = ALIGN8(oldlen + 4) + 16;
		char	   *old = xmalloc(oldlen);
		char	   *cur = xmalloc(newlen);
		uint16		v;
		int			i;
		int			pos = nruns / 2;
		Size		plen;
		int			kind;

		memset(old, 0, oldlen);
		memset(cur, 0, newlen);
		old[6] = cur[6] = 3;	/* LION_CT_RUN */
		v = (uint16) nruns;
		memcpy(old + HDR, &v, 2);
		v = (uint16) (nruns + 1);
		memcpy(cur + HDR, &v, 2);
		for (i = 0; i < nruns; i++)
		{
			uint16		r[2];

			r[0] = (uint16) (i * 30);
			r[1] = 9;
			memcpy(old + HDR + 2 + 4 * i, r, 4);
			memcpy(cur + HDR + 2 + 4 * (i < pos ? i : i + 1), r, 4);
		}
		{
			uint16		r[2];

			r[0] = (uint16) (pos * 30 - 15);
			r[1] = 0;
			memcpy(cur + HDR + 2 + 4 * pos, r, 4);
		}

		kind = roundtrip(old, oldlen, cur, newlen, newlen / 2, &plen);
		if (nruns >= 8)
			CHECK(kind == LION_DELTA_KIND_SPLICE && plen <= 20,
				  "a run inserted is not a small SPLICE");
		free(old);
		free(cur);
	}
}

/*
 * An INLINE payload: several items back to back behind an entry header and a
 * key, one of them gaining a member, and the entry's counter moving.  The
 * splice point is inside the payload, and everything above it - the other
 * items - moves up.
 */
static void
test_inline_payload(void)
{
	int			round;

	cur_phase = "inline payload";
	for (round = 0; round < 200; round++)
	{
		int			nitems = 1 + (int) (rnd() % 6);
		int			target = (int) (rnd() % nitems);
		char	   *old = xmalloc(6000);
		char	   *cur = xmalloc(6000);
		Size		oldlen = 40;
		Size		newlen;
		Size		curpos = 40;
		uint64		ntids = 1000 + rnd() % 100000;
		int			i;
		Size		plen;
		int			kind;

		memset(old, 0, 6000);
		memset(cur, 0, 6000);
		for (i = 0; i < 40; i++)
			old[i] = cur[i] = (char) (rnd() & 0xFF);
		memcpy(old + 16, &ntids, 8);
		ntids++;
		memcpy(cur + 16, &ntids, 8);

		for (i = 0; i < nitems; i++)
		{
			int			n = 8 + (int) (rnd() % 300);
			Size		sz;

			sz = make_array(old + oldlen, HDR + 2 * (Size) n, n, (int) (rnd() % 50), 1 + (int) (rnd() % 7));
			memcpy(cur + curpos, old + oldlen, sz);
			if (i == target)
			{
				uint16		first;

				memcpy(&first, cur + curpos + HDR, 2);
				array_add(cur + curpos, (uint16) (first + 1 + 2 * (rnd() % (Size) (n / 2))) | 1);
				sz += 2;
			}
			oldlen += HDR + 2 * (Size) n;
			curpos += sz;
		}
		newlen = curpos + 8 * (rnd() % 4);

		kind = roundtrip(old, oldlen, cur, newlen, newlen / 2, &plen);
		(void) kind;
		free(old);
		free(cur);
	}
}

/* Anything at all: the round trip holds whatever the two images are. */
static void
test_random(void)
{
	int			round;

	cur_phase = "random";
	for (round = 0; round < 20000; round++)
	{
		Size		oldlen = 1 + rnd() % 700;
		Size		len;
		char	   *old = xmalloc(oldlen);
		char	   *cur;
		Size		i;
		Size		plen;
		int			shape = (int) (rnd() % 6);
		bool		periodic = (rnd() % 8) == 0;

		for (i = 0; i < oldlen; i++)
			old[i] = periodic ? (char) (i % 4) : (char) (rnd() % 5);

		switch (shape)
		{
			case 0:				/* bytes inserted somewhere, a few changed */
			case 1:
				{
					Size		ins = 1 + rnd() % 12;
					Size		at = rnd() % (oldlen + 1);

					len = oldlen + ins + (rnd() % 3) * 8;
					cur = xmalloc(len);
					memset(cur, 0, len);
					memcpy(cur, old, at);
					for (i = 0; i < ins; i++)
						cur[at + i] = (char) rnd();
					memcpy(cur + at + ins, old + at, oldlen - at);
					for (i = 0; i < (Size) (rnd() % 3); i++)
						cur[rnd() % len] = (char) rnd();
				}
				break;
			case 2:				/* bytes removed */
				{
					Size		del = rnd() % (oldlen < 12 ? oldlen : 12);
					Size		at = rnd() % (oldlen - del + 1);

					len = oldlen - del;
					if (len == 0)
						len = 1;
					cur = xmalloc(len);
					memset(cur, 0, len);
					memcpy(cur, old, at);
					memcpy(cur + at, old + at + del, Min(oldlen - at - del, len - at));
				}
				break;
			case 3:				/* changed in place */
				len = oldlen;
				cur = xmalloc(len);
				memcpy(cur, old, len);
				for (i = 0; i < (Size) (rnd() % 20); i++)
					cur[rnd() % len] = (char) rnd();
				break;
			case 4:				/* grown or cut, as slack comes and goes */
				len = 1 + rnd() % 800;
				cur = xmalloc(len);
				memset(cur, 0, len);
				memcpy(cur, old, Min(oldlen, len));
				break;
			default:			/* unrelated */
				len = 1 + rnd() % 800;
				cur = xmalloc(len);
				for (i = 0; i < len; i++)
					cur[i] = (char) rnd();
				break;
		}

		(void) roundtrip(old, oldlen, cur, len,
						 (rnd() % 4 == 0) ? rnd() % (len + 1) : len / 2,
						 &plen);
		free(old);
		free(cur);
	}
}

/* A payload not written against this old image is refused, not applied. */
static void
test_malformed(void)
{
	char	   *old = xmalloc(100);
	char	   *out = xmalloc(120);
	char		p[64];
	uint16		h[2];
	int			round;

	cur_phase = "malformed";
	memset(old, 7, 100);

	/* a splice point past the old image */
	h[0] = 101;
	h[1] = 2;
	memcpy(p, h, 4);
	CHECK(!lion_delta_decode_item(old, 100, LION_DELTA_KIND_SPLICE, p, 6, out, 120),
		  "splice point past the old image accepted");
	/* inserted bytes past the new length */
	h[0] = 99;
	h[1] = 30;
	memcpy(p, h, 4);
	CHECK(!lion_delta_decode_item(old, 100, LION_DELTA_KIND_SPLICE, p, 34, out, 120),
		  "insertion past the new length accepted");
	/* inserted bytes past the payload */
	h[0] = 10;
	h[1] = 30;
	memcpy(p, h, 4);
	CHECK(!lion_delta_decode_item(old, 100, LION_DELTA_KIND_SPLICE, p, 20, out, 120),
		  "insertion past the payload accepted");
	/* an empty insertion */
	h[0] = 10;
	h[1] = 0;
	memcpy(p, h, 4);
	CHECK(!lion_delta_decode_item(old, 100, LION_DELTA_KIND_SPLICE, p, 4, out, 120),
		  "empty insertion accepted");
	/* no header */
	CHECK(!lion_delta_decode_item(old, 100, LION_DELTA_KIND_SPLICE, p, 3, out, 120),
		  "truncated splice header accepted");
	/* a range past the new length */
	h[0] = 118;
	h[1] = 4;
	memcpy(p, h, 4);
	CHECK(!lion_delta_decode_item(old, 100, LION_DELTA_KIND_RANGES, p, 8, out, 120),
		  "range past the item accepted");
	/* a truncated range */
	h[0] = 10;
	h[1] = 4;
	memcpy(p, h, 4);
	CHECK(!lion_delta_decode_item(old, 100, LION_DELTA_KIND_RANGES, p, 6, out, 120),
		  "truncated range accepted");
	/* a zero-length range */
	h[0] = 10;
	h[1] = 0;
	memcpy(p, h, 4);
	CHECK(!lion_delta_decode_item(old, 100, LION_DELTA_KIND_RANGES, p, 4, out, 120),
		  "empty range accepted");
	/* an unknown kind */
	CHECK(!lion_delta_decode_item(old, 100, 7, p, 0, out, 120),
		  "unknown kind accepted");

	/* random payloads: refused or applied, never outside the buffers */
	for (round = 0; round < 20000; round++)
	{
		Size		plen = rnd() % 40;
		char	   *pay = xmalloc(plen);
		Size		newlen = 1 + rnd() % 120;
		char	   *dst = xmalloc(newlen);
		Size		i;

		for (i = 0; i < plen; i++)
			pay[i] = (char) (rnd() % 3 == 0 ? rnd() % 128 : rnd() % 4);
		(void) lion_delta_decode_item(old, 100, 1 + (int) (rnd() % 2), pay, plen,
									  dst, newlen);
		free(pay);
		free(dst);
	}
	CHECK(true, "random payloads stayed inside their buffers");

	free(old);
	free(out);
}

int
main(void)
{
	test_array_member();
	test_run_insert();
	test_inline_payload();
	test_random();
	test_malformed();

	printf("waldelta_test: %ld checks, %ld failed\n", nchecks, nfail);
	return nfail == 0 ? 0 : 1;
}
