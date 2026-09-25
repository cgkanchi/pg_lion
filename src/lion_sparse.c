/*-------------------------------------------------------------------------
 * lion_sparse.c
 *	  Sparse segments for pg_lion.  See DESIGN.md §13 and the header
 *	  comment of lion_sparse.h, which states the format and the
 *	  non-interleaving rule this file maintains.
 *
 *	  The payload is two parallel arrays, uint32 ckeys[n] then uint16 los[n],
 *	  so los[] starts at a position that depends on n and has to be moved
 *	  whenever a pair is inserted or removed.  Every such move is done with
 *	  memmove() in an order that never overwrites data it still has to read;
 *	  each one says which way it slides.
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
 * (lion_container.c, "untrusted containers").  Every function here works
 * with lion_sparse_npairs() pairs and finds los[] where that many ckeys end,
 * so that a header claiming more than LION_SPARSE_MAX_PAIRS never walks it
 * past the largest legal segment, 4100 bytes; and a mutator first rewrites
 * such a claim to that count (sparse_clamp()), so that what it leaves behind
 * fits a LION_CONTAINER_MAX_SIZE buffer.  The pairs' values are only ever
 * compared, never used as an index, and a lo handed to the container library
 * is masked into range.  For a well-formed segment all of this is a no-op.
 * ----------------------------------------------------------------
 */

/* los[] of a segment, for reading, where npairs ckeys end. */
static inline const uint16 *
sparse_los_const(const LionContainer *s, uint32 npairs)
{
	return (const uint16 *) ((const char *) s + LION_CONTAINER_HDRSZ +
							 (Size) npairs * sizeof(uint32));
}

/* See lion_container.c's container_clamp(). */
static inline void
sparse_clamp(LionContainer *s)
{
	if (s->cardinality > LION_SPARSE_MAX_PAIRS)
		s->cardinality = LION_SPARSE_MAX_PAIRS;
}

/*
 * Index of the first pair whose (ckey, lo) is not below (ckey, lo) -- the
 * position at which such a pair would be inserted.
 */
static uint32
sparse_lower_bound(const LionContainer *s, uint32 ckey, uint32 lo)
{
	uint32		n = lion_sparse_npairs(s);
	const uint32 *ckeys = LION_SPARSE_CKEYS_CONST(s);
	const uint16 *los = sparse_los_const(s, n);
	uint32		low = 0;
	uint32		high = n;

	while (low < high)
	{
		uint32		mid = low + (high - low) / 2;

		if (ckeys[mid] < ckey || (ckeys[mid] == ckey && (uint32) los[mid] < lo))
			low = mid + 1;
		else
			high = mid;
	}

	return low;
}

/*
 * Copy the pairs [from, to) of src into dest, which is re-initialised as a
 * segment.  dest must have LION_CONTAINER_MAX_SIZE bytes and must not alias
 * src.  A range of zero pairs produces an empty segment.
 */
static void
sparse_copy_range(const LionContainer *src, uint32 from, uint32 to,
				  LionContainer *dest)
{
	uint32		n = to - from;

	Assert(from <= to && to <= lion_sparse_npairs(src));
	Assert(n <= LION_SPARSE_MAX_PAIRS);

	dest->type = LION_CT_SPARSE;
	dest->flags = 0;
	dest->cardinality = (uint16) n;
	dest->ckey = n > 0 ? LION_SPARSE_CKEYS_CONST(src)[from] : 0;

	if (n > 0)
	{
		memcpy(LION_SPARSE_CKEYS(dest), LION_SPARSE_CKEYS_CONST(src) + from,
			   (Size) n * sizeof(uint32));
		memcpy(LION_SPARSE_LOS(dest),
			   sparse_los_const(src, lion_sparse_npairs(src)) + from,
			   (Size) n * sizeof(uint16));
	}
}

/*
 * Drop the pairs [from, from + count) from s, sliding everything after them
 * down.  The los array also moves down by count * sizeof(uint32) bytes
 * because ckeys[] shrinks.
 */
static void
sparse_delete_range(LionContainer *s, uint32 from, uint32 count)
{
	uint32		n = s->cardinality;
	uint32		newn = n - count;
	uint32	   *ckeys = LION_SPARSE_CKEYS(s);
	uint16	   *los = LION_SPARSE_LOS_AT(s, n);
	uint16	   *newlos = LION_SPARSE_LOS_AT(s, newn);

	Assert(count <= n && from + count <= n);

	if (count == 0)
		return;

	/* ckeys: slide the tail down; this never reaches into los[] */
	memmove(ckeys + from, ckeys + from + count,
			(Size) (n - from - count) * sizeof(uint32));

	/*
	 * los: the whole array slides down, so move the head first (its
	 * destination is below its source and below the tail's source) and the
	 * tail second.
	 */
	memmove(newlos, los, (Size) from * sizeof(uint16));
	memmove(newlos + from, los + from + count,
			(Size) (n - from - count) * sizeof(uint16));

	s->cardinality = (uint16) newn;
	s->ckey = newn > 0 ? ckeys[0] : 0;
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
lion_sparse_find(const LionContainer *s, uint32 ckey, uint32 *first,
				uint32 *count)
{
	const uint32 *ckeys = LION_SPARSE_CKEYS_CONST(s);
	uint32		n = lion_sparse_npairs(s);
	uint32		pos = sparse_lower_bound(s, ckey, 0);
	uint32		end = pos;

	Assert(s->type == LION_CT_SPARSE);

	while (end < n && ckeys[end] == ckey)
		end++;

	*first = pos;
	*count = end - pos;
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
	uint32		n = lion_sparse_npairs(s);
	uint32		pos = sparse_lower_bound(s, ckey, lo);

	Assert(s->type == LION_CT_SPARSE);

	return pos < n &&
		LION_SPARSE_CKEYS_CONST(s)[pos] == ckey &&
		sparse_los_const(s, n)[pos] == lo;
}

bool
lion_sparse_insert(LionContainer *s, uint32 ckey, uint16 lo, bool *dup)
{
	uint32		n;
	uint32		pos;
	uint32	   *ckeys;
	uint16	   *los;
	uint16	   *newlos;

	Assert(s->type == LION_CT_SPARSE);
	Assert((uint32) lo < LION_CONTAINER_RANGE);

	sparse_clamp(s);
	n = s->cardinality;

	if (dup != NULL)
		*dup = false;

	pos = sparse_lower_bound(s, ckey, lo);
	if (pos < n && LION_SPARSE_CKEYS_CONST(s)[pos] == ckey &&
		LION_SPARSE_LOS_CONST(s)[pos] == lo)
	{
		if (dup != NULL)
			*dup = true;
		return true;			/* already there: nothing to do */
	}

	if (n >= LION_SPARSE_MAX_PAIRS)
		return false;

	ckeys = LION_SPARSE_CKEYS(s);
	los = LION_SPARSE_LOS_AT(s, n);
	newlos = LION_SPARSE_LOS_AT(s, n + 1);

	/*
	 * Everything slides up, so the moves go from the far end backwards: the
	 * los tail (up by 6 bytes), then the los head (up by 4), then the ckeys
	 * tail (up by 4, into the 4 bytes the los array has just vacated).
	 */
	memmove(newlos + pos + 1, los + pos, (Size) (n - pos) * sizeof(uint16));
	memmove(newlos, los, (Size) pos * sizeof(uint16));
	memmove(ckeys + pos + 1, ckeys + pos, (Size) (n - pos) * sizeof(uint32));

	ckeys[pos] = ckey;
	newlos[pos] = lo;
	s->cardinality = (uint16) (n + 1);
	s->ckey = ckeys[0];

	Assert(lion_sparse_size(s) <= LION_CONTAINER_MAX_SIZE);
	return true;
}

bool
lion_sparse_remove(LionContainer *s, uint32 ckey, uint16 lo)
{
	uint32		pos;

	Assert(s->type == LION_CT_SPARSE);

	sparse_clamp(s);
	pos = sparse_lower_bound(s, ckey, lo);

	if (pos >= s->cardinality || LION_SPARSE_CKEYS_CONST(s)[pos] != ckey ||
		LION_SPARSE_LOS_CONST(s)[pos] != lo)
		return false;

	sparse_delete_range(s, pos, 1);
	return true;
}

uint32
lion_sparse_remove_if(LionContainer *s, lion_pair_predicate pred, void *arg)
{
	bool		keep[LION_SPARSE_MAX_PAIRS];
	uint32	   *ckeys = LION_SPARSE_CKEYS(s);
	const uint16 *los;
	uint32		n;
	uint32		nkept = 0;
	uint32		i;
	uint16	   *newlos;

	Assert(s->type == LION_CT_SPARSE);

	/*
	 * keep[] is sized by the largest legal segment, and VACUUM hands in a
	 * page item copied by its line pointer's length, not by its header: a
	 * header claiming 1000 pairs would index keep[] 318 entries past its end.
	 */
	sparse_clamp(s);
	n = s->cardinality;
	los = LION_SPARSE_LOS_CONST(s);

	for (i = 0; i < n; i++)
	{
		keep[i] = !pred(ckeys[i], los[i], arg);
		if (keep[i])
			nkept++;
	}

	if (nkept == n)
		return 0;

	/*
	 * Compact ckeys[] forwards in place first: its new tail overlaps the place
	 * the los values are about to move to, so the los values have to be read
	 * out of the old array afterwards, not before.  Nothing of ckeys[] is
	 * needed once the predicates have been evaluated above.
	 */
	newlos = LION_SPARSE_LOS_AT(s, nkept);
	{
		uint32		k = 0;

		for (i = 0; i < n; i++)
		{
			if (keep[i])
				ckeys[k++] = ckeys[i];
		}
		Assert(k == nkept);

		/*
		 * The los values move down (4 bytes per dropped pair), and slot k is
		 * always strictly below slot i it is read from, so a forward loop
		 * never overwrites a value it has still to read.
		 */
		k = 0;
		for (i = 0; i < n; i++)
		{
			if (keep[i])
				newlos[k++] = los[i];
		}
		Assert(k == nkept);
	}

	s->cardinality = (uint16) nkept;
	s->ckey = nkept > 0 ? ckeys[0] : 0;

	return n - nkept;
}

uint32
lion_sparse_extract(LionContainer *s, uint32 ckey, LionContainer *out)
{
	const uint16 *los;
	uint32		first;
	uint32		count;
	uint32		i;

	Assert(s->type == LION_CT_SPARSE);

	sparse_clamp(s);
	los = LION_SPARSE_LOS_CONST(s);
	lion_sparse_find(s, ckey, &first, &count);

	/*
	 * add(), not the bulk builder's append_sorted(): the los of one ckey are
	 * ascending and unique in a well-formed segment, and then the two build
	 * the same bytes (at most LION_SPARSE_THRESHOLD - 1 of them, so the
	 * builder's saving is nothing), but a damaged segment's are whatever
	 * they are, and append_sorted() is promised order and uniqueness.
	 */
	lion_container_init(out, ckey);
	for (i = 0; i < count; i++)
		(void) lion_container_add(out, (uint16) (los[first + i] & LION_LO_MASK));

	if (count > 0)
		sparse_delete_range(s, first, count);

	return count;
}

bool
lion_sparse_split_half(const LionContainer *s, LionContainer *left,
					  LionContainer *right)
{
	const uint32 *ckeys = LION_SPARSE_CKEYS_CONST(s);
	uint32		n = lion_sparse_npairs(s);
	uint32		m;

	Assert(s->type == LION_CT_SPARSE);

	if (n < 2)
		return false;

	/* A ckey must not end up in both halves: walk to the nearest boundary. */
	m = n / 2;
	while (m < n && ckeys[m] == ckeys[m - 1])
		m++;
	if (m >= n)
	{
		m = n / 2;
		while (m > 0 && ckeys[m] == ckeys[m - 1])
			m--;
		if (m == 0)
			return false;		/* every pair has the same ckey */
	}

	sparse_copy_range(s, 0, m, left);
	sparse_copy_range(s, m, n, right);
	return true;
}

void
lion_sparse_split_at(const LionContainer *s, uint32 ckey, LionContainer *left,
					LionContainer *right)
{
	uint32		first;
	uint32		count;

	Assert(s->type == LION_CT_SPARSE);

	/*
	 * The caller extracted ckey already, so count is 0 - unless the segment
	 * is damaged and held ckey's pairs out of order, in which case the ones
	 * extract() did not find simply go right.
	 */
	lion_sparse_find(s, ckey, &first, &count);

	sparse_copy_range(s, 0, first, left);
	sparse_copy_range(s, first, lion_sparse_npairs(s), right);
}

bool
lion_sparse_merge(const LionContainer *a, const LionContainer *b,
				 LionContainer *out)
{
	uint32		na = lion_sparse_npairs(a);
	uint32		nb = lion_sparse_npairs(b);

	Assert(a->type == LION_CT_SPARSE && b->type == LION_CT_SPARSE);

	if (na + nb > LION_SPARSE_MAX_PAIRS)
		return false;
	if (na > 0 && nb > 0 &&
		LION_SPARSE_CKEYS_CONST(a)[na - 1] >= LION_SPARSE_CKEYS_CONST(b)[0])
		return false;			/* not adjacent: would interleave */

	out->type = LION_CT_SPARSE;
	out->flags = 0;
	out->cardinality = (uint16) (na + nb);
	out->ckey = na > 0 ? a->ckey : (nb > 0 ? b->ckey : 0);

	memcpy(LION_SPARSE_CKEYS(out), LION_SPARSE_CKEYS_CONST(a),
		   (Size) na * sizeof(uint32));
	memcpy(LION_SPARSE_CKEYS(out) + na, LION_SPARSE_CKEYS_CONST(b),
		   (Size) nb * sizeof(uint32));
	memcpy(LION_SPARSE_LOS(out), sparse_los_const(a, na),
		   (Size) na * sizeof(uint16));
	memcpy(LION_SPARSE_LOS(out) + na, sparse_los_const(b, nb),
		   (Size) nb * sizeof(uint16));

	return true;
}

void
lion_sparse_iterate(const LionContainer *s, lion_pair_callback cb, void *arg)
{
	uint32		n = lion_sparse_npairs(s);
	const uint32 *ckeys = LION_SPARSE_CKEYS_CONST(s);
	const uint16 *los = sparse_los_const(s, n);
	uint32		i;

	Assert(s->type == LION_CT_SPARSE);

	for (i = 0; i < n; i++)
	{
		if (!cb(ckeys[i], los[i], arg))
			return;
	}
}

bool
lion_sparse_check(const LionContainer *s, Size avail_bytes, const char **errmsg)
{
	const uint32 *ckeys;
	const uint16 *los;
	uint32		n;
	uint32		i;

	*errmsg = NULL;

	if (avail_bytes < LION_CONTAINER_HDRSZ)
		LION_SPARSE_CHECK_FAIL("sparse segment header does not fit in the available space");

	if (s->type != LION_CT_SPARSE)
		LION_SPARSE_CHECK_FAIL("item is not a sparse segment");

	if (s->flags != 0)
		LION_SPARSE_CHECK_FAIL("sparse segment flags are not zero");

	n = s->cardinality;
	if (n > LION_SPARSE_MAX_PAIRS)
		LION_SPARSE_CHECK_FAIL("sparse segment has more than LION_SPARSE_MAX_PAIRS pairs");

	if (lion_sparse_size_for(n) > avail_bytes)
		LION_SPARSE_CHECK_FAIL("sparse segment does not fit in the available space");

	if (n == 0)
		return true;

	ckeys = LION_SPARSE_CKEYS_CONST(s);
	los = LION_SPARSE_LOS_CONST(s);

	if (s->ckey != ckeys[0])
		LION_SPARSE_CHECK_FAIL("sparse segment header key is not its first container key");

	for (i = 0; i < n; i++)
	{
		if ((uint32) los[i] >= LION_CONTAINER_RANGE)
			LION_SPARSE_CHECK_FAIL("sparse segment member is out of range");
		if (i > 0 &&
			(ckeys[i] < ckeys[i - 1] ||
			 (ckeys[i] == ckeys[i - 1] && los[i] <= los[i - 1])))
			LION_SPARSE_CHECK_FAIL("sparse segment pairs are not strictly ascending");
	}

	return true;
}
