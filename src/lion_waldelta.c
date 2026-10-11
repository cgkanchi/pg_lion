/*-------------------------------------------------------------------------
 * lion_waldelta.c
 *	  Encoding a rewritten item against its old image, and decoding it
 *	  again at redo (DESIGN.md §25, LION_OP_DELTA and LION_OP_SPLICE).
 *
 *	  See lion_waldelta.h for the two forms.  Everything here is a pure
 *	  function of its arguments - no page, no buffer, no backend state - so
 *	  that the writer and the redo run the same code on the same bytes, and
 *	  so that test/unit/waldelta_test.c can check the round trip on every
 *	  shape without a server.
 *
 *	  The writer runs this inside its critical section (rmgr mode), on every
 *	  item it rewrites, so the scans compare eight bytes at a time where they
 *	  can and the SPLICE search is skipped when RANGES is already as small as
 *	  a SPLICE could be.
 *-------------------------------------------------------------------------
 */
#include "c.h"

#include <string.h>

#include "lion_waldelta.h"

/* The old image read zero-extended past its end. */
static inline char
lion_delta_ext(const char *old, Size oldlen, Size i)
{
	return (i < oldlen) ? old[i] : 0;
}

int
lion_delta_ranges(const char *base, const char *cur, Size len,
				  char *out, Size cap)
{
	Size		o = 0;
	Size		i = 0;

	while (i < len)
	{
		Size		start;
		Size		end;
		Size		equal;
		uint16		hdr[2];

		/* Skip what did not change, eight bytes at a time. */
		while (i + sizeof(uint64) <= len &&
			   memcmp(cur + i, base + i, sizeof(uint64)) == 0)
			i += sizeof(uint64);
		while (i < len && cur[i] == base[i])
			i++;
		if (i >= len)
			break;

		/* A run of changed bytes, swallowing short runs of equal ones. */
		start = i;
		end = i + 1;
		equal = 0;
		for (i = start + 1; i < len; i++)
		{
			if (cur[i] != base[i])
			{
				end = i + 1;
				equal = 0;
			}
			else if (++equal >= LION_DELTA_GAP)
				break;
		}

		if (o + LION_DELTA_RANGE_HDR + (end - start) > cap)
			return -1;
		if (out != NULL)
		{
			hdr[0] = (uint16) start;
			hdr[1] = (uint16) (end - start);
			memcpy(out + o, hdr, LION_DELTA_RANGE_HDR);
			memcpy(out + o + LION_DELTA_RANGE_HDR, cur + start, end - start);
		}
		o += LION_DELTA_RANGE_HDR + (end - start);
		i = end;
	}

	return (int) o;
}

bool
lion_delta_apply_ranges(char *target, Size len, const char *ranges, Size rlen)
{
	Size		p = 0;

	while (p < rlen)
	{
		uint16		hdr[2];

		if (p + LION_DELTA_RANGE_HDR > rlen)
			return false;
		memcpy(hdr, ranges + p, LION_DELTA_RANGE_HDR);
		p += LION_DELTA_RANGE_HDR;
		if (hdr[1] == 0 || p + hdr[1] > rlen || (Size) hdr[0] + hdr[1] > len)
			return false;
		memcpy(target + hdr[0], ranges + p, hdr[1]);
		p += hdr[1];
	}

	return true;
}

bool
lion_splice_find(const char *old, Size oldlen, const char *cur, Size curlen,
				 Size shift, Size *at)
{
	Size		k = curlen;		/* cur[k, curlen) is the old image moved up */

	if (shift == 0 || curlen < shift)
		return false;

	/*
	 * Walk down while cur[k - 1] is the old image's byte k - 1 - shift: past
	 * the old image's end that is a zero, which is where the slack of both
	 * lies, and inside it eight bytes at a time.
	 */
	while (k > shift && k - shift > oldlen)
	{
		if (cur[k - 1] != 0)
			break;
		k--;
	}
	if (k > shift && k - shift <= oldlen)
	{
		while (k >= shift + sizeof(uint64) &&
			   memcmp(cur + k - sizeof(uint64),
					  old + k - shift - sizeof(uint64),
					  sizeof(uint64)) == 0)
			k -= sizeof(uint64);
		while (k > shift && cur[k - 1] == lion_delta_ext(old, oldlen, k - 1 - shift))
			k--;
	}

	if (k - shift > oldlen)
		return false;			/* an append past the old image: RANGES */

	*at = k - shift;
	return true;
}

void
lion_splice_base(const char *old, Size oldlen, Size at,
				 const char *ins, Size inslen, char *base, Size newlen)
{
	Size		p;
	Size		n;

	Assert(at <= oldlen);

	n = Min(at, newlen);
	memcpy(base, old, n);
	p = n;
	if (p < newlen)
	{
		n = Min(inslen, newlen - p);
		memcpy(base + p, ins, n);
		p += n;
	}
	if (p < newlen && at < oldlen)
	{
		n = Min(oldlen - at, newlen - p);
		memcpy(base + p, old + at, n);
		p += n;
	}
	if (p < newlen)
		memset(base + p, 0, newlen - p);
}

/* The RANGES base: the old image cut to newlen or zero-extended to it. */
static void
lion_delta_plain_base(const char *old, Size oldlen, char *base, Size newlen)
{
	Size		n = Min(oldlen, newlen);

	memcpy(base, old, n);
	if (newlen > n)
		memset(base + n, 0, newlen - n);
}

Size
lion_delta_encode_item(const char *old, Size oldlen, const char *cur, Size len,
					   char *out, Size cap, char *scratch, int *kind)
{
	int			rlen;
	Size		best_at = 0;
	Size		best_shift = 0;
	Size		s;
	int			slen = -1;

	*kind = LION_DELTA_KIND_NONE;

	lion_delta_plain_base(old, oldlen, scratch, len);
	rlen = lion_delta_ranges(scratch, cur, len, NULL, cap);

	/*
	 * Can a SPLICE beat it?  Not when the ranges are already as small as the
	 * smallest SPLICE that still changes a byte below its insertion point,
	 * which is the common case of a member appended to an ARRAY: its count
	 * and its last two bytes.
	 */
	if (rlen < 0 ||
		rlen > LION_SPLICE_HDR + 1 + LION_DELTA_RANGE_HDR + 1)
	{
		for (s = 1; s <= LION_SPLICE_MAX_SHIFT; s++)
		{
			Size		at;

			if (lion_splice_find(old, oldlen, cur, len, s, &at) &&
				(best_shift == 0 || at < best_at))
			{
				best_at = at;
				best_shift = s;
			}
		}
	}

	if (best_shift > 0)
	{
		Size		scap;

		scap = (rlen >= 0) ? (Size) rlen : cap;
		if (scap > LION_SPLICE_HDR + best_shift)
		{
			int			r;

			lion_splice_base(old, oldlen, best_at, cur + best_at, best_shift,
							 scratch, len);
			/* [best_at, len) is equal by construction */
			r = lion_delta_ranges(scratch, cur, best_at, NULL,
								  scap - LION_SPLICE_HDR - best_shift);
			if (r >= 0 &&
				(rlen < 0 || LION_SPLICE_HDR + best_shift + (Size) r < (Size) rlen))
				slen = (int) (LION_SPLICE_HDR + best_shift) + r;
		}
	}

	if (slen >= 0)
	{
		uint16		hdr[2];

		/* scratch still holds the SPLICE base */
		hdr[0] = (uint16) best_at;
		hdr[1] = (uint16) best_shift;
		memcpy(out, hdr, LION_SPLICE_HDR);
		memcpy(out + LION_SPLICE_HDR, cur + best_at, best_shift);
		(void) lion_delta_ranges(scratch, cur, best_at,
								 out + LION_SPLICE_HDR + best_shift,
								 (Size) slen - LION_SPLICE_HDR - best_shift);
		*kind = LION_DELTA_KIND_SPLICE;
		return (Size) slen;
	}

	if (rlen >= 0)
	{
		lion_delta_plain_base(old, oldlen, scratch, len);
		(void) lion_delta_ranges(scratch, cur, len, out, (Size) rlen);
		*kind = LION_DELTA_KIND_RANGES;
		return (Size) rlen;
	}

	return 0;
}

bool
lion_delta_decode_item(const char *old, Size oldlen, int kind,
					   const char *payload, Size plen,
					   char *out, Size newlen)
{
	if (kind == LION_DELTA_KIND_RANGES)
	{
		lion_delta_plain_base(old, oldlen, out, newlen);
		return lion_delta_apply_ranges(out, newlen, payload, plen);
	}

	if (kind == LION_DELTA_KIND_SPLICE)
	{
		uint16		hdr[2];
		Size		at;
		Size		inslen;

		if (plen < LION_SPLICE_HDR)
			return false;
		memcpy(hdr, payload, LION_SPLICE_HDR);
		at = hdr[0];
		inslen = hdr[1];
		if (inslen == 0 || at > oldlen || at + inslen > newlen ||
			LION_SPLICE_HDR + inslen > plen)
			return false;
		lion_splice_base(old, oldlen, at, payload + LION_SPLICE_HDR, inslen,
						 out, newlen);
		return lion_delta_apply_ranges(out, newlen,
									   payload + LION_SPLICE_HDR + inslen,
									   plen - LION_SPLICE_HDR - inslen);
	}

	return false;
}
