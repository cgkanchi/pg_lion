/*-------------------------------------------------------------------------
 * lion_sparse.c
 *	  Sparse segments for pg_lion.  See DESIGN.md §13 and the header
 *	  comment of lion_sparse.h, which states the format and the
 *	  non-interleaving rule this file maintains.
 *
 *	  The payload is a stream of delta varints, so a pair has no fixed place:
 *	  the readers decode from the start, and the mutators either rewrite the
 *	  stream from the point they change (lion_sparse_insert(), the one called
 *	  on a page item in place) or decode the pairs into an array, change the
 *	  array and encode it again (everything else; a segment is at most 255
 *	  pairs).  The encoding of a set of pairs is unique - sorted codes,
 *	  shortest varints - so both ways produce the same bytes.
 *
 *	  Like lion_container.c this file depends only on c.h (plus the container
 *	  library) so that it builds standalone with -DFRONTEND for
 *	  test/unit/sparse_test.c.
 *-------------------------------------------------------------------------
 */
#ifndef FRONTEND
#include "postgres.h"
#else
#include "c.h"
#endif

#include <string.h>

#include "lion_sparse.h"

#define LION_SPARSE_CHECK_FAIL(msg) \
	do { *errmsg = (msg); return false; } while (0)


/* ----------------------------------------------------------------
 *							internal helpers
 *
 * A segment read from disk is data, exactly as a container is
 * (lion_container.c, "untrusted containers").  Every reader here goes
 * through LionSparseIter, which never reads past the stream length in the
 * header (at most 255 bytes) nor decodes more than LION_SPARSE_MAX_PAIRS
 * pairs; a mutator works on what that decodes to and never writes more than
 * LION_SPARSE_MAX_SIZE bytes, dropping whatever pairs of a damaged segment
 * would not fit.  A lo handed on - to the container library, to an
 * iterate() or remove_if() callback - is the low bits of a code and so
 * always in range.  For a well-formed segment all of this is a no-op.
 * ----------------------------------------------------------------
 */

/* Write v as a varint at p; returns the byte after it. */
static inline uint8 *
sparse_put_varint(uint8 *p, uint64 v)
{
	while (v >= 0x80)
	{
		*p++ = (uint8) (v | 0x80);
		v >>= 7;
	}
	*p++ = (uint8) v;
	return p;
}

/* The base a segment whose first code is code decodes from. */
static inline uint64
sparse_base(uint64 code)
{
	return ((uint64) lion_code_ckey(code)) << LION_CONTAINER_BITS;
}

/* Decode the pairs into codes[] (LION_SPARSE_MAX_PAIRS entries). */
static uint32
sparse_decode(const LionContainer *s, uint64 *codes)
{
	LionSparseIter it;
	uint32		n = 0;

	lion_sparse_iter_init(&it, s);
	while (lion_sparse_iter_next(&it))
		codes[n++] = it.code;

	Assert(n <= LION_SPARSE_MAX_PAIRS);
	return n;
}

/* Stream bytes codes[0 .. n) take. */
static uint32
sparse_encoded_bytes(const uint64 *codes, uint32 n)
{
	uint64		prev;
	uint32		bytes = 0;
	uint32		i;

	if (n == 0)
		return 0;
	prev = sparse_base(codes[0]);
	for (i = 0; i < n; i++)
	{
		bytes += lion_sparse_varint_size(codes[i] - prev);
		prev = codes[i];
	}
	return bytes;
}

/*
 * Make s the segment of codes[0 .. n), which must not alias s.  Pairs that
 * would take the stream past LION_SPARSE_MAX_BYTES are dropped, which only a
 * damaged segment's codes can need (the callers have checked the size of
 * anything else).  n = 0 makes the empty work state.
 */
static void
sparse_encode(LionContainer *s, const uint64 *codes, uint32 n)
{
	uint8	   *start = LION_SPARSE_STREAM(s);
	uint8	   *p = start;
	uint64		prev;
	uint32		i;

	s->type = LION_CT_SPARSE;
	if (n == 0)
	{
		s->ckey = 0;
		s->cardinality = 0;
		s->flags = 0;
		return;
	}

	s->ckey = lion_code_ckey(codes[0]);
	prev = sparse_base(codes[0]);
	for (i = 0; i < n; i++)
	{
		uint64		v = codes[i] - prev;

		if ((uint32) (p - start) + lion_sparse_varint_size(v) >
			LION_SPARSE_MAX_BYTES)
			break;				/* damaged input only */
		p = sparse_put_varint(p, v);
		prev = codes[i];
	}

	s->cardinality = (uint16) i;
	s->flags = (uint8) (p - start);
}

/*
 * Index of the first code at or above target in codes[0 .. n): the position
 * at which target would be inserted.
 */
static uint32
sparse_lower_bound(const uint64 *codes, uint32 n, uint64 target)
{
	uint32		low = 0;
	uint32		high = n;

	while (low < high)
	{
		uint32		mid = low + (high - low) / 2;

		if (codes[mid] < target)
			low = mid + 1;
		else
			high = mid;
	}
	return low;
}

/*
 * The varint at the insertion point and the new pair's replace it, so the
 * stream with the pair inserted takes the old stream's bytes less that
 * varint's, plus the new pair's delta from the code before it (its lo, from
 * the base, when it opens the stream) and, when there is a pair at the point,
 * that pair's delta from the new one.
 */
static inline uint32
sparse_probe_newbytes(const LionSparseProbe *pr)
{
	uint32		n = pr->end - pr->len;

	if (pr->front)
		n += lion_sparse_varint_size(pr->code - sparse_base(pr->code));
	else
		n += lion_sparse_varint_size(pr->code - pr->prev);
	if (pr->len > 0)
		n += lion_sparse_varint_size(pr->next - pr->code);
	return n;
}

/* ----------------------------------------------------------------
 *								API
 * ----------------------------------------------------------------
 */

void
lion_sparse_init(LionContainer *s, uint32 ckey)
{
	s->ckey = ckey;
	s->cardinality = 0;
	s->type = LION_CT_SPARSE;
	s->flags = 0;
}

void
lion_sparse_probe(const LionContainer *s, uint32 ckey, uint16 lo,
				  LionSparseProbe *pr)
{
	LionSparseIter it;
	const uint8 *start = LION_SPARSE_STREAM_CONST(s);
	const uint8 *before;
	uint64		code = lion_make_code(ckey, lo);

	Assert(s->type == LION_CT_SPARSE);
	Assert((uint32) lo < LION_CONTAINER_RANGE);

	/* every byte, padding included: lion_sparse_insert_probed() compares */
	memset(pr, 0, sizeof(LionSparseProbe));
	pr->code = code;
	pr->front = true;

	lion_sparse_iter_init(&it, s);
	for (;;)
	{
		before = it.p;
		if (!lion_sparse_iter_next(&it))
			break;
		pr->npairs++;
		if (lion_code_ckey(it.code) == ckey)
			pr->ckey_pairs++;
		if (it.code >= code)
		{
			/* the insertion point: this pair's varint is rewritten */
			pr->off = (uint32) (before - start);
			pr->len = (uint32) (it.p - before);
			pr->next = it.code;
			pr->found = (it.code == code);
			/* the rest of the stream is only measured, and its ckey counted */
			while (lion_sparse_iter_next(&it))
			{
				pr->npairs++;
				if (lion_code_ckey(it.code) == ckey)
					pr->ckey_pairs++;
			}
			break;
		}
		pr->prev = it.code;
		pr->front = false;
	}
	pr->end = (uint32) (it.p - start);
	if (pr->len == 0)
		pr->off = pr->end;

	/* as lion_sparse_last_ckey(): the iterator stops on the last pair */
	pr->last_ckey = (pr->npairs > 0) ? lion_code_ckey(it.code) : s->ckey;
	pr->newbytes = sparse_probe_newbytes(pr);
}

uint32
lion_sparse_last_ckey(const LionContainer *s)
{
	LionSparseIter it;
	bool		any = false;

	lion_sparse_iter_init(&it, s);
	while (lion_sparse_iter_next(&it))
		any = true;

	return any ? lion_code_ckey(it.code) : s->ckey;
}

void
lion_sparse_find(const LionContainer *s, uint32 ckey, uint32 *first,
				uint32 *count)
{
	LionSparseIter it;
	uint32		i = 0;
	uint32		n = 0;

	Assert(s->type == LION_CT_SPARSE);

	lion_sparse_iter_init(&it, s);
	while (lion_sparse_iter_next(&it))
	{
		uint32		k = lion_code_ckey(it.code);

		if (k < ckey)
			i++;
		else if (k == ckey)
			n++;
		else
			break;
	}

	*first = i;
	*count = n;
}

uint32
lion_sparse_count(const LionContainer *s, uint32 ckey)
{
	uint32		first;
	uint32		count;

	lion_sparse_find(s, ckey, &first, &count);
	return count;
}

bool
lion_sparse_contains(const LionContainer *s, uint32 ckey, uint16 lo)
{
	LionSparseIter it;
	uint64		code = lion_make_code(ckey, lo);

	Assert(s->type == LION_CT_SPARSE);

	lion_sparse_iter_init(&it, s);
	while (lion_sparse_iter_next(&it))
	{
		if (it.code >= code)
			return it.code == code;
	}
	return false;
}

Size
lion_sparse_insert_size(const LionContainer *s, uint32 ckey, uint16 lo)
{
	LionSparseProbe pr;

	lion_sparse_probe(s, ckey, lo, &pr);
	return lion_sparse_probe_insert_size(s, &pr);
}

bool
lion_sparse_insert(LionContainer *s, uint32 ckey, uint16 lo, bool *dup)
{
	LionSparseProbe pr;

	lion_sparse_probe(s, ckey, lo, &pr);
	return lion_sparse_insert_probed(s, &pr, dup);
}

bool
lion_sparse_insert_probed(LionContainer *s, const LionSparseProbe *pr,
						  bool *dup)
{
	uint8	   *stream = LION_SPARSE_STREAM(s);
	uint8	   *p;
	uint32		taillen;
	uint32		head;

	Assert(s->type == LION_CT_SPARSE);
#ifdef USE_ASSERT_CHECKING
	{
		/* the probe is of these very bytes */
		LionSparseProbe again;

		lion_sparse_probe(s, lion_code_ckey(pr->code), lion_code_lo(pr->code),
						  &again);
		Assert(memcmp(&again, pr, sizeof(again)) == 0);
	}
#endif

	if (dup != NULL)
		*dup = false;

	if (pr->found)
	{
		if (dup != NULL)
			*dup = true;
		return true;			/* already there: nothing to do */
	}
	if (!lion_sparse_probe_fits(pr))
		return false;

	/*
	 * The varint at the insertion point (if any) and the new pair's replace
	 * it: the bytes behind it slide to where the stream with the pair
	 * inserted has them - first, while nothing has been overwritten yet - and
	 * the bytes in front of it stay where they are.  Nothing is written past
	 * lion_sparse_insert_size().
	 */
	taillen = pr->end - pr->off - pr->len;
	head = pr->newbytes - taillen;
	memmove(stream + head, stream + pr->off + pr->len, taillen);

	p = stream + pr->off;
	if (pr->front)
	{
		Assert(pr->off == 0);
		p = sparse_put_varint(p, pr->code - sparse_base(pr->code));
		s->ckey = lion_code_ckey(pr->code);
	}
	else
		p = sparse_put_varint(p, pr->code - pr->prev);
	if (pr->len > 0)
		p = sparse_put_varint(p, pr->next - pr->code);
	Assert(p == stream + head);
	(void) p;

	s->cardinality = (uint16) (pr->npairs + 1);
	s->flags = (uint8) pr->newbytes;

	Assert(lion_sparse_size(s) <= LION_SPARSE_MAX_SIZE);
	return true;
}

bool
lion_sparse_append(LionContainer *s, uint64 *last, uint32 ckey, uint16 lo)
{
	uint64		code = lion_make_code(ckey, lo);
	uint32		nbytes = lion_sparse_nbytes(s);
	uint64		v;

	Assert(s->type == LION_CT_SPARSE);

	if (s->cardinality == 0)
	{
		nbytes = 0;
		v = code - sparse_base(code);
	}
	else if (code <= *last)
	{
		/* not above the last pair: a repeat, or out of order */
		return lion_sparse_insert(s, ckey, lo, NULL);
	}
	else
		v = code - *last;

	if (nbytes + lion_sparse_varint_size(v) > LION_SPARSE_MAX_BYTES ||
		(uint32) s->cardinality + 1 > LION_SPARSE_MAX_PAIRS)
		return false;

	(void) sparse_put_varint(LION_SPARSE_STREAM(s) + nbytes, v);
	if (s->cardinality == 0)
		s->ckey = ckey;
	s->cardinality++;
	s->flags = (uint8) (nbytes + lion_sparse_varint_size(v));
	*last = code;
	return true;
}

bool
lion_sparse_remove(LionContainer *s, uint32 ckey, uint16 lo)
{
	uint64		codes[LION_SPARSE_MAX_PAIRS];
	uint64		code = lion_make_code(ckey, lo);
	uint32		n;
	uint32		pos;

	Assert(s->type == LION_CT_SPARSE);

	n = sparse_decode(s, codes);
	pos = sparse_lower_bound(codes, n, code);
	if (pos >= n || codes[pos] != code)
		return false;

	memmove(&codes[pos], &codes[pos + 1], (Size) (n - pos - 1) * sizeof(uint64));
	sparse_encode(s, codes, n - 1);
	return true;
}

uint32
lion_sparse_remove_if(LionContainer *s, lion_pair_predicate pred, void *arg)
{
	uint64		codes[LION_SPARSE_MAX_PAIRS];
	uint32		n;
	uint32		nkept = 0;
	uint32		i;

	Assert(s->type == LION_CT_SPARSE);

	/*
	 * VACUUM hands in a page item copied by its line pointer's length, and
	 * the header is not trusted any further than the iterator trusts it.
	 */
	n = sparse_decode(s, codes);

	for (i = 0; i < n; i++)
	{
		if (!pred(lion_code_ckey(codes[i]), lion_code_lo(codes[i]), arg))
			codes[nkept++] = codes[i];
	}

	if (nkept == n)
		return 0;

	sparse_encode(s, codes, nkept);
	return n - nkept;
}

uint32
lion_sparse_extract(LionContainer *s, uint32 ckey, LionContainer *out)
{
	uint64		codes[LION_SPARSE_MAX_PAIRS];
	uint32		n;
	uint32		first;
	uint32		end;
	uint32		i;

	Assert(s->type == LION_CT_SPARSE);

	n = sparse_decode(s, codes);
	first = sparse_lower_bound(codes, n, lion_make_code(ckey, 0));
	for (end = first; end < n && lion_code_ckey(codes[end]) == ckey; end++)
		;

	/*
	 * add(), not the bulk builder's append_sorted(): the los of one ckey are
	 * ascending and unique in a well-formed segment, and then the two build
	 * the same bytes (at most LION_SPARSE_THRESHOLD - 1 of them, so the
	 * builder's saving is nothing), but a damaged segment's are whatever
	 * they are, and append_sorted() is promised order and uniqueness.
	 */
	lion_container_init(out, ckey);
	for (i = first; i < end; i++)
		(void) lion_container_add(out, lion_code_lo(codes[i]));

	if (end > first)
	{
		memmove(&codes[first], &codes[end], (Size) (n - end) * sizeof(uint64));
		sparse_encode(s, codes, n - (end - first));
	}

	return end - first;
}

bool
lion_sparse_split_half(const LionContainer *s, LionContainer *left,
					  LionContainer *right)
{
	uint64		codes[LION_SPARSE_MAX_PAIRS];
	uint32		n;
	uint32		m;

	Assert(s->type == LION_CT_SPARSE);

	n = sparse_decode(s, codes);
	if (n < 2)
		return false;

	/* A ckey must not end up in both halves: walk to the nearest boundary. */
	m = n / 2;
	while (m < n && lion_code_ckey(codes[m]) == lion_code_ckey(codes[m - 1]))
		m++;
	if (m >= n)
	{
		m = n / 2;
		while (m > 0 &&
			   lion_code_ckey(codes[m]) == lion_code_ckey(codes[m - 1]))
			m--;
		if (m == 0)
			return false;		/* every pair has the same ckey */
	}

	sparse_encode(left, codes, m);
	sparse_encode(right, codes + m, n - m);
	return true;
}

void
lion_sparse_split_at(const LionContainer *s, uint32 ckey, LionContainer *left,
					LionContainer *right)
{
	uint64		codes[LION_SPARSE_MAX_PAIRS];
	uint32		n;
	uint32		first;

	Assert(s->type == LION_CT_SPARSE);

	/*
	 * The caller extracted ckey already, so no pair has it - unless the
	 * segment is damaged and held ckey's pairs out of order, in which case
	 * the ones extract() did not find simply go right.
	 */
	n = sparse_decode(s, codes);
	first = sparse_lower_bound(codes, n, lion_make_code(ckey, 0));

	sparse_encode(left, codes, first);
	sparse_encode(right, codes + first, n - first);
}

bool
lion_sparse_merge(const LionContainer *a, const LionContainer *b,
				 LionContainer *out)
{
	uint64		codes[2 * LION_SPARSE_MAX_PAIRS];
	uint32		na;
	uint32		nb;

	Assert(a->type == LION_CT_SPARSE && b->type == LION_CT_SPARSE);

	na = sparse_decode(a, codes);
	nb = sparse_decode(b, codes + na);

	if (na + nb > LION_SPARSE_MAX_PAIRS)
		return false;
	if (na > 0 && nb > 0 &&
		lion_code_ckey(codes[na - 1]) >= lion_code_ckey(codes[na]))
		return false;			/* not adjacent: would interleave */
	if (sparse_encoded_bytes(codes, na + nb) > LION_SPARSE_MAX_BYTES)
		return false;

	sparse_encode(out, codes, na + nb);
	return true;
}

void
lion_sparse_iterate(const LionContainer *s, lion_pair_callback cb, void *arg)
{
	LionSparseIter it;

	Assert(s->type == LION_CT_SPARSE);

	lion_sparse_iter_init(&it, s);
	while (lion_sparse_iter_next(&it))
	{
		if (!cb(lion_code_ckey(it.code), lion_code_lo(it.code), arg))
			return;
	}
}

bool
lion_sparse_check(const LionContainer *s, Size avail_bytes, const char **errmsg)
{
	const uint8 *p;
	const uint8 *end;
	uint64		code;
	uint32		n;
	uint32		i;

	*errmsg = NULL;

	if (avail_bytes < LION_CONTAINER_HDRSZ)
		LION_SPARSE_CHECK_FAIL("sparse segment header does not fit in the available space");

	if (s->type != LION_CT_SPARSE)
		LION_SPARSE_CHECK_FAIL("item is not a sparse segment");

	if (lion_sparse_size(s) > avail_bytes)
		LION_SPARSE_CHECK_FAIL("sparse segment does not fit in the available space");

	/* lion_sparse.h: a segment holds 1 .. LION_SPARSE_MAX_PAIRS pairs */
	n = s->cardinality;
	if (n == 0)
		LION_SPARSE_CHECK_FAIL("sparse segment has no pairs");
	if (n > LION_SPARSE_MAX_PAIRS)
		LION_SPARSE_CHECK_FAIL("sparse segment has more than LION_SPARSE_MAX_PAIRS pairs");
	if (n > lion_sparse_nbytes(s))
		LION_SPARSE_CHECK_FAIL("sparse segment has more pairs than bytes of stream");

	p = LION_SPARSE_STREAM_CONST(s);
	end = p + lion_sparse_nbytes(s);
	code = ((uint64) s->ckey) << LION_CONTAINER_BITS;
	if (code > LION_SPARSE_MAX_CODE)
		LION_SPARSE_CHECK_FAIL("sparse segment pair is past the largest heap TID");

	for (i = 0; i < n; i++)
	{
		uint64		v = 0;
		uint32		len = 0;
		uint8		b;

		do
		{
			if (p >= end)
				LION_SPARSE_CHECK_FAIL("sparse segment stream ends inside its pairs");
			if (len >= LION_SPARSE_VARINT_MAX)
				LION_SPARSE_CHECK_FAIL("sparse segment delta is longer than a heap TID code");
			b = *p++;
			v |= ((uint64) (b & 0x7F)) << (7 * len);
			len++;
		} while (b & 0x80);

		/* one encoding per set of pairs: WAL redo reproduces the bytes */
		if (len > 1 && b == 0)
			LION_SPARSE_CHECK_FAIL("sparse segment delta is not in its shortest form");

		if (i == 0)
		{
			/* the header ckey is the first pair's: its delta is a lo */
			if (v >= LION_CONTAINER_RANGE)
				LION_SPARSE_CHECK_FAIL("sparse segment header key is not its first container key");
		}
		else if (v == 0)
			LION_SPARSE_CHECK_FAIL("sparse segment pairs are not strictly ascending");

		if (v > LION_SPARSE_MAX_CODE - code)
			LION_SPARSE_CHECK_FAIL("sparse segment pair is past the largest heap TID");
		code += v;
	}

	if (p != end)
		LION_SPARSE_CHECK_FAIL("sparse segment stream has bytes after its last pair");

	return true;
}

bool
lion_sparse_check_offsets(const LionContainer *s, uint32 maxoff,
						  const char **errmsg)
{
	LionSparseIter it;

	*errmsg = NULL;
	Assert(maxoff < (1U << LION_OFFSET_BITS));

	lion_sparse_iter_init(&it, s);
	while (lion_sparse_iter_next(&it))
	{
		if (!lion_lo_is_tuple(lion_code_lo(it.code), maxoff))
			LION_SPARSE_CHECK_FAIL("sparse segment member is not a heap tuple offset");
	}

	return true;
}
