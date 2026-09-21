/*-------------------------------------------------------------------------
 * lion_container.c
 *	  Roaring-style containers for pg_lion.  See DESIGN.md §3.
 *
 *	  Three representations of a set of 15-bit "lo" values:
 *
 *		ARRAY	uint16 lo[cardinality], strictly ascending, cardinality <= 2048
 *		BITSET	uint64 words[512], fixed 4096 bytes
 *		RUN		uint16 nruns, then nruns ascending, non-overlapping and
 *				non-adjacent (start, len_minus_1) pairs, nruns <= 1023
 *
 *	  Representation policy (DESIGN.md §3), enforced by the mutators:
 *		- adding to a full ARRAY (2048 members)			=> BITSET
 *		- adding to a RUN that would need a 1024th run	=> BITSET
 *		- removing from a RUN so that a split would need a 1024th run
 *														=> BITSET
 *		- removing from a BITSET leaving <= 2048 members	=> ARRAY
 *
 *	  lion_container_optimize() is the only function that searches for the
 *	  globally smallest representation.
 *
 *	  This file depends only on c.h, port/pg_bitutils.h and the project
 *	  headers, so that it also builds standalone with -DFRONTEND for
 *	  test/unit/container_test.c.  No palloc, no elog: invariants are
 *	  Assert()ed and structural damage is reported by lion_container_check().
 *-------------------------------------------------------------------------
 */
/*
 * lion_container.h pulls in lion_tid.h, which needs the server's ItemPointer
 * declarations in a backend build; in a -DFRONTEND build (the unit tests)
 * lion_tid.h reduces itself to the code/ckey/lo arithmetic and c.h is enough.
 */
#ifndef FRONTEND
#include "postgres.h"
#else
#include "c.h"
#endif

#include <string.h>

#include "port/pg_bitutils.h"

#include "lion_container.h"

/* All-ones 64-bit word. */
#define LION_ALL_ONES		UINT64CONST(0xFFFFFFFFFFFFFFFF)
/* Largest legal lo value. */
#define LION_LO_MAX			((uint32) (LION_CONTAINER_RANGE - 1))
/* A size that always loses the "smallest representation" comparison. */
#define LION_SIZE_INFEASIBLE	((Size) (LION_CONTAINER_MAX_SIZE + 1))

/*
 * A work buffer large enough for any container.  The union forces 8-byte
 * alignment so that the bitset payload (offset 8) is aligned as well.
 */
typedef union LionContainerBuf
{
	LionContainer hdr;
	uint64		force_align;
	char		data[LION_CONTAINER_MAX_SIZE];
} LionContainerBuf;


/* ----------------------------------------------------------------
 *						payload accessors
 * ----------------------------------------------------------------
 */

static inline uint16 *
array_mdata(LionContainer *c)
{
	return (uint16 *) ((char *) c + LION_CONTAINER_HDRSZ);
}

static inline const uint16 *
array_cdata(const LionContainer *c)
{
	return (const uint16 *) ((const char *) c + LION_CONTAINER_HDRSZ);
}

static inline uint64 *
bitset_mdata(LionContainer *c)
{
	return (uint64 *) ((char *) c + LION_CONTAINER_HDRSZ);
}

static inline const uint64 *
bitset_cdata(const LionContainer *c)
{
	return (const uint64 *) ((const char *) c + LION_CONTAINER_HDRSZ);
}

static inline uint32
run_nruns(const LionContainer *c)
{
	return (uint32) *(const uint16 *) ((const char *) c + LION_CONTAINER_HDRSZ);
}

static inline void
run_set_nruns(LionContainer *c, uint32 nruns)
{
	Assert(nruns <= LION_RUN_MAX_NRUNS);
	*(uint16 *) ((char *) c + LION_CONTAINER_HDRSZ) = (uint16) nruns;
}

static inline LionRun *
run_mdata(LionContainer *c)
{
	return (LionRun *) ((char *) c + LION_CONTAINER_HDRSZ + sizeof(uint16));
}

static inline const LionRun *
run_cdata(const LionContainer *c)
{
	return (const LionRun *) ((const char *) c + LION_CONTAINER_HDRSZ + sizeof(uint16));
}

/* Last lo value covered by a run. */
static inline int32
run_last(const LionRun *r)
{
	return (int32) r->start + (int32) r->len_minus_1;
}


/* ----------------------------------------------------------------
 *						bitset primitives
 *
 * These all operate on a bare array of LION_BITSET_WORDS uint64s.
 * ----------------------------------------------------------------
 */

/* Word with bits lo .. hi (0-based, inclusive, within one word) set. */
static inline uint64
word_mask(uint32 lo, uint32 hi)
{
	Assert(lo <= hi && hi < 64);
	return (LION_ALL_ONES >> (63 - (hi - lo))) << lo;
}

static inline bool
bits_test(const uint64 *w, uint32 lo)
{
	Assert(lo <= LION_LO_MAX);
	return (w[lo >> 6] & (UINT64CONST(1) << (lo & 63))) != 0;
}

static inline void
bits_set(uint64 *w, uint32 lo)
{
	Assert(lo <= LION_LO_MAX);
	w[lo >> 6] |= UINT64CONST(1) << (lo & 63);
}

static inline void
bits_clear(uint64 *w, uint32 lo)
{
	Assert(lo <= LION_LO_MAX);
	w[lo >> 6] &= ~(UINT64CONST(1) << (lo & 63));
}

static void
bits_set_range(uint64 *w, uint32 lo, uint32 hi)
{
	uint32		wlo = lo >> 6;
	uint32		whi = hi >> 6;

	Assert(lo <= hi && hi <= LION_LO_MAX);
	if (wlo == whi)
	{
		w[wlo] |= word_mask(lo & 63, hi & 63);
		return;
	}
	w[wlo] |= word_mask(lo & 63, 63);
	if (whi > wlo + 1)
		memset(&w[wlo + 1], 0xFF, (size_t) (whi - wlo - 1) * sizeof(uint64));
	w[whi] |= word_mask(0, hi & 63);
}

static void
bits_clear_range(uint64 *w, uint32 lo, uint32 hi)
{
	uint32		wlo = lo >> 6;
	uint32		whi = hi >> 6;

	Assert(lo <= hi && hi <= LION_LO_MAX);
	if (wlo == whi)
	{
		w[wlo] &= ~word_mask(lo & 63, hi & 63);
		return;
	}
	w[wlo] &= ~word_mask(lo & 63, 63);
	if (whi > wlo + 1)
		memset(&w[wlo + 1], 0x00, (size_t) (whi - wlo - 1) * sizeof(uint64));
	w[whi] &= ~word_mask(0, hi & 63);
}

static uint32
bits_cardinality(const uint64 *w)
{
	uint32		n = 0;
	uint32		i;

	for (i = 0; i < LION_BITSET_WORDS; i++)
		n += (uint32) pg_popcount64(w[i]);
	return n;
}

static uint32
bits_range_cardinality(const uint64 *w, uint32 lo, uint32 hi)
{
	uint32		wlo = lo >> 6;
	uint32		whi = hi >> 6;
	uint32		n;
	uint32		i;

	Assert(lo <= hi && hi <= LION_LO_MAX);
	if (wlo == whi)
		return (uint32) pg_popcount64(w[wlo] & word_mask(lo & 63, hi & 63));

	n = (uint32) pg_popcount64(w[wlo] & word_mask(lo & 63, 63));
	for (i = wlo + 1; i < whi; i++)
		n += (uint32) pg_popcount64(w[i]);
	n += (uint32) pg_popcount64(w[whi] & word_mask(0, hi & 63));
	return n;
}

/*
 * Number of maximal runs of consecutive set bits.  A bit starts a run if it
 * is set and its predecessor is not, so count those in a word-parallel way.
 */
static uint32
bits_count_runs(const uint64 *w)
{
	uint32		nruns = 0;
	uint64		prev = 0;
	uint32		i;

	for (i = 0; i < LION_BITSET_WORDS; i++)
	{
		uint64		cur = w[i];

		nruns += (uint32) pg_popcount64(cur & ~((cur << 1) | prev));
		prev = cur >> 63;
	}
	return nruns;
}

/* Materialise the set bits in ascending order.  Returns the count. */
static uint32
bits_extract_array(const uint64 *w, uint16 *out)
{
	uint32		n = 0;
	uint32		i;

	for (i = 0; i < LION_BITSET_WORDS; i++)
	{
		uint64		cur = w[i];
		uint32		base = i << 6;

		while (cur != 0)
		{
			out[n++] = (uint16) (base + (uint32) pg_rightmost_one_pos64(cur));
			cur &= cur - 1;
		}
	}
	return n;
}

/*
 * Materialise the runs of set bits.  At most maxruns are written to out, but
 * the total number of runs is always returned, so the caller can detect that
 * the buffer was too small.
 */
static uint32
bits_extract_runs(const uint64 *w, LionRun *out, uint32 maxruns)
{
	uint32		n = 0;
	int32		rstart = -1;
	int32		rprev = -2;
	uint32		i;

	for (i = 0; i < LION_BITSET_WORDS; i++)
	{
		uint64		cur = w[i];
		int32		base = (int32) (i << 6);

		while (cur != 0)
		{
			int32		v = base + pg_rightmost_one_pos64(cur);

			cur &= cur - 1;
			if (v == rprev + 1)
			{
				rprev = v;
				continue;
			}
			if (rstart >= 0)
			{
				if (n < maxruns)
				{
					out[n].start = (uint16) rstart;
					out[n].len_minus_1 = (uint16) (rprev - rstart);
				}
				n++;
			}
			rstart = v;
			rprev = v;
		}
	}
	if (rstart >= 0)
	{
		if (n < maxruns)
		{
			out[n].start = (uint16) rstart;
			out[n].len_minus_1 = (uint16) (rprev - rstart);
		}
		n++;
	}
	return n;
}


/* ----------------------------------------------------------------
 *						array primitives
 * ----------------------------------------------------------------
 */

/* First index i with arr[i] >= key (i.e. n if there is none). */
static uint32
array_lower_bound(const uint16 *arr, uint32 n, uint32 key)
{
	uint32		lo = 0;
	uint32		hi = n;

	while (lo < hi)
	{
		uint32		mid = lo + (hi - lo) / 2;

		if (arr[mid] < key)
			lo = mid + 1;
		else
			hi = mid;
	}
	return lo;
}

/* First index i with arr[i] > key. */
static uint32
array_upper_bound(const uint16 *arr, uint32 n, uint32 key)
{
	uint32		lo = 0;
	uint32		hi = n;

	while (lo < hi)
	{
		uint32		mid = lo + (hi - lo) / 2;

		if (arr[mid] <= key)
			lo = mid + 1;
		else
			hi = mid;
	}
	return lo;
}

/*
 * Exponential ("galloping") search: first index i >= from with arr[i] >= key.
 * Used when intersecting arrays of very different sizes.
 */
static uint32
array_gallop(const uint16 *arr, uint32 n, uint32 from, uint32 key)
{
	uint32		lo;
	uint32		hi;
	uint32		step = 1;

	if (from >= n)
		return n;
	if (arr[from] >= key)
		return from;

	lo = from;
	while (from + step < n && arr[from + step] < key)
	{
		lo = from + step;
		step *= 2;
	}
	hi = (from + step < n) ? from + step : n;
	return lo + 1 + array_lower_bound(arr + lo + 1, hi - lo - 1, key);
}


/* ----------------------------------------------------------------
 *						run primitives
 * ----------------------------------------------------------------
 */

/* Index of the rightmost run whose start is <= lo, or -1. */
static int32
run_locate(const LionRun *runs, uint32 nruns, uint32 lo)
{
	int32		lo_i = 0;
	int32		hi_i = (int32) nruns - 1;
	int32		res = -1;

	while (lo_i <= hi_i)
	{
		int32		mid = lo_i + (hi_i - lo_i) / 2;

		if ((uint32) runs[mid].start <= lo)
		{
			res = mid;
			lo_i = mid + 1;
		}
		else
			hi_i = mid - 1;
	}
	return res;
}


/* ----------------------------------------------------------------
 *				container -> bitset image helpers
 *
 * "w" is always a caller-supplied array of LION_BITSET_WORDS uint64s that does
 * not overlap the container's own payload.
 * ----------------------------------------------------------------
 */

/* w |= c */
static void
container_or_bitset(const LionContainer *c, uint64 *w)
{
	uint32		i;

	switch (c->type)
	{
		case LION_CT_ARRAY:
			{
				const uint16 *arr = array_cdata(c);

				for (i = 0; i < c->cardinality; i++)
					bits_set(w, arr[i]);
				break;
			}
		case LION_CT_BITSET:
			{
				const uint64 *src = bitset_cdata(c);

				for (i = 0; i < LION_BITSET_WORDS; i++)
					w[i] |= src[i];
				break;
			}
		case LION_CT_RUN:
			{
				const LionRun *runs = run_cdata(c);
				uint32		nruns = run_nruns(c);

				for (i = 0; i < nruns; i++)
					bits_set_range(w, runs[i].start, (uint32) run_last(&runs[i]));
				break;
			}
		default:
			Assert(false);
			break;
	}
}

/* w &= ~c */
static void
container_andnot_bitset(const LionContainer *c, uint64 *w)
{
	uint32		i;

	switch (c->type)
	{
		case LION_CT_ARRAY:
			{
				const uint16 *arr = array_cdata(c);

				for (i = 0; i < c->cardinality; i++)
					bits_clear(w, arr[i]);
				break;
			}
		case LION_CT_BITSET:
			{
				const uint64 *src = bitset_cdata(c);

				for (i = 0; i < LION_BITSET_WORDS; i++)
					w[i] &= ~src[i];
				break;
			}
		case LION_CT_RUN:
			{
				const LionRun *runs = run_cdata(c);
				uint32		nruns = run_nruns(c);

				for (i = 0; i < nruns; i++)
					bits_clear_range(w, runs[i].start, (uint32) run_last(&runs[i]));
				break;
			}
		default:
			Assert(false);
			break;
	}
}

/*
 * w &= c.  Only BITSET and RUN operands get here: the public entry points
 * intersect ARRAY operands by probing, without materialising a bitset.
 */
static void
container_and_bitset(const LionContainer *c, uint64 *w)
{
	uint32		i;

	switch (c->type)
	{
		case LION_CT_BITSET:
			{
				const uint64 *src = bitset_cdata(c);

				for (i = 0; i < LION_BITSET_WORDS; i++)
					w[i] &= src[i];
				break;
			}
		case LION_CT_RUN:
			{
				const LionRun *runs = run_cdata(c);
				uint32		nruns = run_nruns(c);
				uint32		prev = 0;

				/* clear the gaps between (and around) the runs */
				for (i = 0; i < nruns; i++)
				{
					if ((uint32) runs[i].start > prev)
						bits_clear_range(w, prev, (uint32) runs[i].start - 1);
					prev = (uint32) run_last(&runs[i]) + 1;
				}
				if (prev <= LION_LO_MAX)
					bits_clear_range(w, prev, LION_LO_MAX);
				break;
			}
		default:
			Assert(false);
			break;
	}
}

/* w = c */
static void
container_fill_bitset(const LionContainer *c, uint64 *w)
{
	if (c->type == LION_CT_BITSET)
		memcpy(w, bitset_cdata(c), LION_BITSET_BYTES);
	else
	{
		memset(w, 0, LION_BITSET_BYTES);
		container_or_bitset(c, w);
	}
}

/* Number of maximal runs of consecutive members, whatever the type. */
static uint32
container_count_runs(const LionContainer *c)
{
	switch (c->type)
	{
		case LION_CT_ARRAY:
			{
				const uint16 *arr = array_cdata(c);
				uint32		n = c->cardinality;
				uint32		nruns = 0;
				uint32		i;

				for (i = 0; i < n; i++)
					if (i == 0 || (uint32) arr[i] != (uint32) arr[i - 1] + 1)
						nruns++;
				return nruns;
			}
		case LION_CT_BITSET:
			return bits_count_runs(bitset_cdata(c));
		case LION_CT_RUN:
			return run_nruns(c);
		default:
			Assert(false);
			return 0;
	}
}


/* ----------------------------------------------------------------
 *						representation changes
 * ----------------------------------------------------------------
 */

static void
container_make_bitset(LionContainer *c)
{
	uint64		w[LION_BITSET_WORDS];

	if (c->type == LION_CT_BITSET)
		return;
	container_fill_bitset(c, w);
	c->type = LION_CT_BITSET;
	memcpy(bitset_mdata(c), w, LION_BITSET_BYTES);
}

static void
container_make_array(LionContainer *c)
{
	uint16		tmp[LION_ARRAY_MAX_CARD];
	uint32		n;

	if (c->type == LION_CT_ARRAY)
		return;
	Assert(c->cardinality <= LION_ARRAY_MAX_CARD);
	n = lion_container_to_array(c, tmp);
	Assert(n == c->cardinality);
	c->type = LION_CT_ARRAY;
	memcpy(array_mdata(c), tmp, (size_t) n * sizeof(uint16));
}

static void
container_make_run(LionContainer *c)
{
	LionRun		tmp[LION_RUN_MAX_NRUNS];
	uint32		n;

	if (c->type == LION_CT_RUN)
		return;
	if (c->type == LION_CT_BITSET)
		n = bits_extract_runs(bitset_cdata(c), tmp, LION_RUN_MAX_NRUNS);
	else
	{
		const uint16 *arr = array_cdata(c);
		uint32		card = c->cardinality;
		uint32		i;

		n = 0;
		for (i = 0; i < card; i++)
		{
			if (i > 0 && (uint32) arr[i] == (uint32) arr[i - 1] + 1)
			{
				if (n <= LION_RUN_MAX_NRUNS)
					tmp[n - 1].len_minus_1++;
				continue;
			}
			if (n < LION_RUN_MAX_NRUNS)
			{
				tmp[n].start = arr[i];
				tmp[n].len_minus_1 = 0;
			}
			n++;
		}
	}
	Assert(n <= LION_RUN_MAX_NRUNS);
	c->type = LION_CT_RUN;
	run_set_nruns(c, n);
	memcpy(run_mdata(c), tmp, (size_t) n * sizeof(LionRun));
}

/*
 * Rewrite a RUN container's payload from a bitset image holding "card"
 * members; "w" must not overlap c.  The RUN encoding is kept when the runs
 * still fit, otherwise the smaller of ARRAY (when it is legal) and BITSET is
 * used.  This is the tail of the RUN removal paths, which per DESIGN.md §3
 * must never leave a BITSET holding <= LION_ARRAY_MAX_CARD members.
 */
static void
container_rebuild(LionContainer *c, const uint64 *w, uint32 card)
{
	uint32		nruns;

	Assert(card <= LION_CONTAINER_RANGE);
	c->cardinality = (uint16) card;

	nruns = bits_count_runs(w);
	if (nruns <= LION_RUN_MAX_NRUNS)
	{
		LionRun		runs[LION_RUN_MAX_NRUNS];
		uint32		n = bits_extract_runs(w, runs, LION_RUN_MAX_NRUNS);

		Assert(n == nruns);
		c->type = LION_CT_RUN;
		run_set_nruns(c, n);
		memcpy(run_mdata(c), runs, (size_t) n * sizeof(LionRun));
		return;
	}
	if (card <= LION_ARRAY_MAX_CARD)
	{
		uint16		vals[LION_ARRAY_MAX_CARD];
		uint32		n = bits_extract_array(w, vals);

		Assert(n == card);
		c->type = LION_CT_ARRAY;
		memcpy(array_mdata(c), vals, (size_t) n * sizeof(uint16));
		return;
	}
	c->type = LION_CT_BITSET;
	memcpy(bitset_mdata(c), w, LION_BITSET_BYTES);
}

/* DESIGN.md §3: a BITSET that has shrunk to <= 2048 members becomes an ARRAY. */
static inline void
container_shrink_bitset(LionContainer *c)
{
	if (c->type == LION_CT_BITSET && c->cardinality <= LION_ARRAY_MAX_CARD)
		container_make_array(c);
}


/* ----------------------------------------------------------------
 *						size, construction, lookup
 * ----------------------------------------------------------------
 */

Size
lion_container_size_for(LionContainerType type, uint32 cardinality, uint32 nruns)
{
	switch (type)
	{
		case LION_CT_ARRAY:
			return LION_CONTAINER_HDRSZ + (Size) cardinality * sizeof(uint16);
		case LION_CT_BITSET:
			return LION_CONTAINER_HDRSZ + LION_BITSET_BYTES;
		case LION_CT_RUN:
			return LION_CONTAINER_HDRSZ + sizeof(uint16) +
				(Size) nruns * sizeof(LionRun);
		case LION_CT_SPARSE:
			/* a segment is not a container: lion_sparse_size() sizes those */
			break;
	}
	Assert(false);
	return LION_CONTAINER_HDRSZ;
}

Size
lion_container_size(const LionContainer *c)
{
	if (c->type == LION_CT_RUN)
		return lion_container_size_for(LION_CT_RUN, c->cardinality, run_nruns(c));
	return lion_container_size_for((LionContainerType) c->type, c->cardinality, 0);
}

void
lion_container_init(LionContainer *c, uint32 ckey)
{
	c->ckey = ckey;
	c->cardinality = 0;
	c->type = LION_CT_ARRAY;
	c->flags = 0;
}

uint32
lion_container_cardinality(const LionContainer *c)
{
	return c->cardinality;
}

bool
lion_container_contains(const LionContainer *c, uint16 lo)
{
	Assert((uint32) lo <= LION_LO_MAX);

	switch (c->type)
	{
		case LION_CT_ARRAY:
			{
				const uint16 *arr = array_cdata(c);
				uint32		n = c->cardinality;
				uint32		pos = array_lower_bound(arr, n, lo);

				return pos < n && arr[pos] == lo;
			}
		case LION_CT_BITSET:
			return bits_test(bitset_cdata(c), lo);
		case LION_CT_RUN:
			{
				const LionRun *runs = run_cdata(c);
				int32		idx = run_locate(runs, run_nruns(c), lo);

				return idx >= 0 && (int32) lo <= run_last(&runs[idx]);
			}
		default:
			Assert(false);
			return false;
	}
}

void
lion_container_to_bitset(LionContainer *c)
{
	container_make_bitset(c);
}

void
lion_container_optimize(LionContainer *c)
{
	uint32		card = c->cardinality;
	uint32		nruns = container_count_runs(c);
	Size		asz;
	Size		rsz;
	Size		bsz = lion_container_size_for(LION_CT_BITSET, card, 0);

	asz = (card <= LION_ARRAY_MAX_CARD)
		? lion_container_size_for(LION_CT_ARRAY, card, 0)
		: LION_SIZE_INFEASIBLE;
	rsz = (nruns <= LION_RUN_MAX_NRUNS)
		? lion_container_size_for(LION_CT_RUN, card, nruns)
		: LION_SIZE_INFEASIBLE;

	/* ties prefer ARRAY, then RUN, then BITSET */
	if (asz <= rsz && asz <= bsz)
		container_make_array(c);
	else if (rsz <= bsz)
		container_make_run(c);
	else
		container_make_bitset(c);

	Assert(lion_container_size(c) <= LION_CONTAINER_MAX_SIZE);
}


/* ----------------------------------------------------------------
 *							mutators
 * ----------------------------------------------------------------
 */

static bool
run_add(LionContainer *c, uint32 lo)
{
	LionRun	   *runs = run_mdata(c);
	int32		nruns = (int32) run_nruns(c);
	int32		idx = run_locate(runs, (uint32) nruns, lo);
	int32		v = (int32) lo;

	if (idx >= 0)
	{
		int32		last = run_last(&runs[idx]);

		if (v <= last)
			return false;		/* already a member */
		if (v == last + 1)
		{
			/* extend run idx to the right */
			runs[idx].len_minus_1++;
			/* and merge with the next run if that closed the gap */
			if (idx + 1 < nruns && (int32) runs[idx + 1].start == v + 1)
			{
				runs[idx].len_minus_1 = (uint16)
					(run_last(&runs[idx + 1]) - (int32) runs[idx].start);
				memmove(&runs[idx + 1], &runs[idx + 2],
						(size_t) (nruns - idx - 2) * sizeof(LionRun));
				run_set_nruns(c, (uint32) (nruns - 1));
			}
			c->cardinality++;
			return true;
		}
	}

	/* extend the following run to the left? */
	if (idx + 1 < nruns && (int32) runs[idx + 1].start == v + 1)
	{
		runs[idx + 1].start = (uint16) v;
		runs[idx + 1].len_minus_1++;
		c->cardinality++;
		return true;
	}

	/* a brand new one-element run is needed */
	if (nruns >= (int32) LION_RUN_MAX_NRUNS)
	{
		container_make_bitset(c);
		bits_set(bitset_mdata(c), lo);
		c->cardinality++;
		return true;
	}
	memmove(&runs[idx + 2], &runs[idx + 1],
			(size_t) (nruns - idx - 1) * sizeof(LionRun));
	runs[idx + 1].start = (uint16) v;
	runs[idx + 1].len_minus_1 = 0;
	run_set_nruns(c, (uint32) (nruns + 1));
	c->cardinality++;
	return true;
}

static bool
run_remove(LionContainer *c, uint32 lo)
{
	LionRun	   *runs = run_mdata(c);
	int32		nruns = (int32) run_nruns(c);
	int32		idx = run_locate(runs, (uint32) nruns, lo);
	int32		v = (int32) lo;
	int32		start;
	int32		last;

	if (idx < 0)
		return false;
	start = (int32) runs[idx].start;
	last = run_last(&runs[idx]);
	if (v > last)
		return false;

	if (start == last)
	{
		/* the whole run disappears */
		memmove(&runs[idx], &runs[idx + 1],
				(size_t) (nruns - idx - 1) * sizeof(LionRun));
		run_set_nruns(c, (uint32) (nruns - 1));
	}
	else if (v == start)
	{
		runs[idx].start = (uint16) (start + 1);
		runs[idx].len_minus_1--;
	}
	else if (v == last)
	{
		runs[idx].len_minus_1--;
	}
	else
	{
		/* the run splits in two */
		if (nruns >= (int32) LION_RUN_MAX_NRUNS)
		{
			container_make_bitset(c);
			bits_clear(bitset_mdata(c), lo);
			c->cardinality--;
			container_shrink_bitset(c);
			return true;
		}
		memmove(&runs[idx + 2], &runs[idx + 1],
				(size_t) (nruns - idx - 1) * sizeof(LionRun));
		runs[idx].len_minus_1 = (uint16) (v - 1 - start);
		runs[idx + 1].start = (uint16) (v + 1);
		runs[idx + 1].len_minus_1 = (uint16) (last - v - 1);
		run_set_nruns(c, (uint32) (nruns + 1));
	}
	c->cardinality--;
	return true;
}

bool
lion_container_add(LionContainer *c, uint16 lo)
{
	Assert((uint32) lo <= LION_LO_MAX);

	switch (c->type)
	{
		case LION_CT_ARRAY:
			{
				uint16	   *arr = array_mdata(c);
				uint32		n = c->cardinality;
				uint32		pos = array_lower_bound(arr, n, lo);

				if (pos < n && arr[pos] == lo)
					return false;
				if (n >= LION_ARRAY_MAX_CARD)
				{
					container_make_bitset(c);
					bits_set(bitset_mdata(c), lo);
					c->cardinality++;
					return true;
				}
				memmove(&arr[pos + 1], &arr[pos],
						(size_t) (n - pos) * sizeof(uint16));
				arr[pos] = lo;
				c->cardinality++;
				return true;
			}
		case LION_CT_BITSET:
			{
				uint64	   *w = bitset_mdata(c);

				if (bits_test(w, lo))
					return false;
				bits_set(w, lo);
				c->cardinality++;
				return true;
			}
		case LION_CT_RUN:
			return run_add(c, lo);
		default:
			Assert(false);
			return false;
	}
}

bool
lion_container_remove(LionContainer *c, uint16 lo)
{
	Assert((uint32) lo <= LION_LO_MAX);

	switch (c->type)
	{
		case LION_CT_ARRAY:
			{
				uint16	   *arr = array_mdata(c);
				uint32		n = c->cardinality;
				uint32		pos = array_lower_bound(arr, n, lo);

				if (pos >= n || arr[pos] != lo)
					return false;
				memmove(&arr[pos], &arr[pos + 1],
						(size_t) (n - pos - 1) * sizeof(uint16));
				c->cardinality--;
				return true;
			}
		case LION_CT_BITSET:
			{
				uint64	   *w = bitset_mdata(c);

				if (!bits_test(w, lo))
					return false;
				bits_clear(w, lo);
				c->cardinality--;
				container_shrink_bitset(c);
				return true;
			}
		case LION_CT_RUN:
			return run_remove(c, lo);
		default:
			Assert(false);
			return false;
	}
}

void
lion_container_append_sorted(LionContainer *c, uint16 lo)
{
	Assert((uint32) lo <= LION_LO_MAX);

	if (c->type == LION_CT_ARRAY)
	{
		uint16	   *arr = array_mdata(c);
		uint32		n = c->cardinality;

		Assert(n == 0 || arr[n - 1] < lo);
		if (n < LION_ARRAY_MAX_CARD)
		{
			arr[n] = lo;
			c->cardinality = (uint16) (n + 1);
			return;
		}
		container_make_bitset(c);
	}

	if (c->type == LION_CT_BITSET)
	{
		uint64	   *w = bitset_mdata(c);

		Assert(!bits_test(w, lo));
		bits_set(w, lo);
		c->cardinality++;
		return;
	}

	/* A builder never produces a RUN, but stay total if one shows up. */
	Assert(c->type == LION_CT_RUN);
	(void) lion_container_add(c, lo);
}


/* ----------------------------------------------------------------
 *							iteration
 * ----------------------------------------------------------------
 */

void
lion_container_iterate(const LionContainer *c, lion_lo_callback cb, void *arg)
{
	uint32		i;

	switch (c->type)
	{
		case LION_CT_ARRAY:
			{
				const uint16 *arr = array_cdata(c);
				uint32		n = c->cardinality;

				for (i = 0; i < n; i++)
					if (!cb(arr[i], arg))
						return;
				break;
			}
		case LION_CT_BITSET:
			{
				const uint64 *w = bitset_cdata(c);

				for (i = 0; i < LION_BITSET_WORDS; i++)
				{
					uint64		cur = w[i];
					uint32		base = i << 6;

					while (cur != 0)
					{
						uint32		lo = base + (uint32) pg_rightmost_one_pos64(cur);

						cur &= cur - 1;
						if (!cb((uint16) lo, arg))
							return;
					}
				}
				break;
			}
		case LION_CT_RUN:
			{
				const LionRun *runs = run_cdata(c);
				uint32		nruns = run_nruns(c);

				for (i = 0; i < nruns; i++)
				{
					int32		v = (int32) runs[i].start;
					int32		last = run_last(&runs[i]);

					for (; v <= last; v++)
						if (!cb((uint16) v, arg))
							return;
				}
				break;
			}
		default:
			Assert(false);
			break;
	}
}

uint32
lion_container_to_array(const LionContainer *c, uint16 *out)
{
	uint32		n = 0;
	uint32		i;

	switch (c->type)
	{
		case LION_CT_ARRAY:
			n = c->cardinality;
			memcpy(out, array_cdata(c), (size_t) n * sizeof(uint16));
			break;
		case LION_CT_BITSET:
			n = bits_extract_array(bitset_cdata(c), out);
			break;
		case LION_CT_RUN:
			{
				const LionRun *runs = run_cdata(c);
				uint32		nruns = run_nruns(c);

				for (i = 0; i < nruns; i++)
				{
					int32		v = (int32) runs[i].start;
					int32		last = run_last(&runs[i]);

					for (; v <= last; v++)
						out[n++] = (uint16) v;
				}
				break;
			}
		default:
			Assert(false);
			break;
	}
	Assert(n == c->cardinality);
	return n;
}


/* ----------------------------------------------------------------
 *					bulk removal and range helpers
 * ----------------------------------------------------------------
 */

uint32
lion_container_remove_if(LionContainer *c, lion_lo_predicate pred, void *arg)
{
	uint32		removed = 0;
	uint32		i;

	switch (c->type)
	{
		case LION_CT_ARRAY:
			{
				uint16	   *arr = array_mdata(c);
				uint32		n = c->cardinality;
				uint32		keep = 0;

				for (i = 0; i < n; i++)
				{
					if (pred(arr[i], arg))
						removed++;
					else
						arr[keep++] = arr[i];
				}
				c->cardinality = (uint16) keep;
				break;
			}
		case LION_CT_BITSET:
			{
				uint64	   *w = bitset_mdata(c);

				for (i = 0; i < LION_BITSET_WORDS; i++)
				{
					uint64		cur = w[i];
					uint64		keep = cur;
					uint32		base = i << 6;

					while (cur != 0)
					{
						int			b = pg_rightmost_one_pos64(cur);

						cur &= cur - 1;
						if (pred((uint16) (base + (uint32) b), arg))
						{
							keep &= ~(UINT64CONST(1) << b);
							removed++;
						}
					}
					w[i] = keep;
				}
				Assert(removed <= c->cardinality);
				c->cardinality -= (uint16) removed;
				container_shrink_bitset(c);
				break;
			}
		case LION_CT_RUN:
			{
				uint64		w[LION_BITSET_WORDS];
				const LionRun *runs = run_cdata(c);
				uint32		nruns = run_nruns(c);
				uint32		card = c->cardinality;

				memset(w, 0, LION_BITSET_BYTES);
				for (i = 0; i < nruns; i++)
				{
					int32		v = (int32) runs[i].start;
					int32		last = run_last(&runs[i]);

					for (; v <= last; v++)
					{
						if (pred((uint16) v, arg))
							removed++;
						else
							bits_set(w, (uint32) v);
					}
				}
				Assert(removed <= card);
				container_rebuild(c, w, card - removed);
				break;
			}
		default:
			Assert(false);
			break;
	}
	Assert(lion_container_size(c) <= LION_CONTAINER_MAX_SIZE);
	return removed;
}

uint32
lion_container_range_cardinality(const LionContainer *c, uint16 lo_start, uint16 lo_end)
{
	Assert((uint32) lo_start <= LION_LO_MAX && (uint32) lo_end <= LION_LO_MAX);
	if (lo_start > lo_end || c->cardinality == 0)
		return 0;

	switch (c->type)
	{
		case LION_CT_ARRAY:
			{
				const uint16 *arr = array_cdata(c);
				uint32		n = c->cardinality;

				return array_upper_bound(arr, n, lo_end) -
					array_lower_bound(arr, n, lo_start);
			}
		case LION_CT_BITSET:
			return bits_range_cardinality(bitset_cdata(c), lo_start, lo_end);
		case LION_CT_RUN:
			{
				const LionRun *runs = run_cdata(c);
				int32		nruns = (int32) run_nruns(c);
				int32		i = run_locate(runs, (uint32) nruns, lo_start);
				uint32		n = 0;

				if (i < 0 || run_last(&runs[i]) < (int32) lo_start)
					i++;
				for (; i < nruns && (int32) runs[i].start <= (int32) lo_end; i++)
				{
					int32		s = Max((int32) runs[i].start, (int32) lo_start);
					int32		e = Min(run_last(&runs[i]), (int32) lo_end);

					if (s <= e)
						n += (uint32) (e - s + 1);
				}
				return n;
			}
		default:
			Assert(false);
			return 0;
	}
}

uint32
lion_container_remove_range(LionContainer *c, uint16 lo_start, uint16 lo_end)
{
	uint32		removed;

	Assert((uint32) lo_start <= LION_LO_MAX && (uint32) lo_end <= LION_LO_MAX);
	if (lo_start > lo_end || c->cardinality == 0)
		return 0;

	switch (c->type)
	{
		case LION_CT_ARRAY:
			{
				uint16	   *arr = array_mdata(c);
				uint32		n = c->cardinality;
				uint32		from = array_lower_bound(arr, n, lo_start);
				uint32		to = array_upper_bound(arr, n, lo_end);

				removed = to - from;
				if (removed > 0)
				{
					memmove(&arr[from], &arr[to],
							(size_t) (n - to) * sizeof(uint16));
					c->cardinality = (uint16) (n - removed);
				}
				break;
			}
		case LION_CT_BITSET:
			{
				uint64	   *w = bitset_mdata(c);

				removed = bits_range_cardinality(w, lo_start, lo_end);
				if (removed > 0)
				{
					bits_clear_range(w, lo_start, lo_end);
					c->cardinality -= (uint16) removed;
					container_shrink_bitset(c);
				}
				break;
			}
		case LION_CT_RUN:
			{
				LionRun	   *runs = run_mdata(c);
				int32		nruns = (int32) run_nruns(c);
				int32		s = (int32) lo_start;
				int32		e = (int32) lo_end;
				LionRun		repl[2];
				int32		nrepl = 0;
				int32		newn;
				int32		i;
				int32		j;
				int32		k;

				/* first run reaching into the range */
				i = run_locate(runs, (uint32) nruns, lo_start);
				if (i < 0 || run_last(&runs[i]) < s)
					i++;
				if (i >= nruns || (int32) runs[i].start > e)
					return 0;
				/* last run starting at or before the end of the range */
				j = run_locate(runs, (uint32) nruns, lo_end);
				Assert(j >= i);

				removed = 0;
				for (k = i; k <= j; k++)
				{
					int32		rs = Max((int32) runs[k].start, s);
					int32		re = Min(run_last(&runs[k]), e);

					if (rs <= re)
						removed += (uint32) (re - rs + 1);
				}

				if ((int32) runs[i].start < s)
				{
					repl[nrepl].start = runs[i].start;
					repl[nrepl].len_minus_1 = (uint16) (s - 1 - (int32) runs[i].start);
					nrepl++;
				}
				if (run_last(&runs[j]) > e)
				{
					repl[nrepl].start = (uint16) (e + 1);
					repl[nrepl].len_minus_1 = (uint16) (run_last(&runs[j]) - (e + 1));
					nrepl++;
				}

				newn = nruns - (j - i + 1) + nrepl;
				if (newn > (int32) LION_RUN_MAX_NRUNS)
				{
					container_make_bitset(c);
					bits_clear_range(bitset_mdata(c), lo_start, lo_end);
					c->cardinality -= (uint16) removed;
					container_shrink_bitset(c);
					break;
				}
				memmove(&runs[i + nrepl], &runs[j + 1],
						(size_t) (nruns - j - 1) * sizeof(LionRun));
				for (k = 0; k < nrepl; k++)
					runs[i + k] = repl[k];
				run_set_nruns(c, (uint32) newn);
				c->cardinality -= (uint16) removed;
				break;
			}
		default:
			Assert(false);
			removed = 0;
			break;
	}
	Assert(lion_container_size(c) <= LION_CONTAINER_MAX_SIZE);
	return removed;
}


/* ----------------------------------------------------------------
 *							set algebra
 * ----------------------------------------------------------------
 */

/* Intersection of two sorted arrays; out gets at most min(na, nb) values. */
static uint32
array_intersect(const uint16 *a, uint32 na, const uint16 *b, uint32 nb,
				uint16 *out)
{
	uint32		n = 0;
	uint32		i = 0;
	uint32		j = 0;

	/* very different sizes: gallop through the larger array */
	if (na > nb * 8 || nb > na * 8)
	{
		const uint16 *small = (na <= nb) ? a : b;
		const uint16 *large = (na <= nb) ? b : a;
		uint32		nsmall = (na <= nb) ? na : nb;
		uint32		nlarge = (na <= nb) ? nb : na;
		uint32		li = 0;

		for (i = 0; i < nsmall; i++)
		{
			li = array_gallop(large, nlarge, li, small[i]);
			if (li >= nlarge)
				break;
			if (large[li] == small[i])
				out[n++] = small[i];
		}
		return n;
	}

	while (i < na && j < nb)
	{
		if (a[i] < b[j])
			i++;
		else if (a[i] > b[j])
			j++;
		else
		{
			out[n++] = a[i];
			i++;
			j++;
		}
	}
	return n;
}

/* Union of two sorted arrays. */
static uint32
array_union(const uint16 *a, uint32 na, const uint16 *b, uint32 nb, uint16 *out)
{
	uint32		n = 0;
	uint32		i = 0;
	uint32		j = 0;

	while (i < na && j < nb)
	{
		if (a[i] < b[j])
			out[n++] = a[i++];
		else if (a[i] > b[j])
			out[n++] = b[j++];
		else
		{
			out[n++] = a[i++];
			j++;
		}
	}
	while (i < na)
		out[n++] = a[i++];
	while (j < nb)
		out[n++] = b[j++];
	return n;
}

/* a \ b for two sorted arrays. */
static uint32
array_difference(const uint16 *a, uint32 na, const uint16 *b, uint32 nb,
				 uint16 *out)
{
	uint32		n = 0;
	uint32		i = 0;
	uint32		j = 0;

	while (i < na && j < nb)
	{
		if (a[i] < b[j])
			out[n++] = a[i++];
		else if (a[i] > b[j])
			j++;
		else
		{
			i++;
			j++;
		}
	}
	while (i < na)
		out[n++] = a[i++];
	return n;
}

/* Members of arr that are (not) in the run container rc. */
static uint32
array_and_run(const uint16 *arr, uint32 na, const LionContainer *rc, uint16 *out)
{
	const LionRun *runs = run_cdata(rc);
	uint32		nruns = run_nruns(rc);
	uint32		i = 0;
	uint32		j = 0;
	uint32		n = 0;

	while (i < na && j < nruns)
	{
		if ((int32) arr[i] < (int32) runs[j].start)
			i++;
		else if ((int32) arr[i] > run_last(&runs[j]))
			j++;
		else
			out[n++] = arr[i++];
	}
	return n;
}

static uint32
array_andnot_run(const uint16 *arr, uint32 na, const LionContainer *rc,
				 uint16 *out)
{
	const LionRun *runs = run_cdata(rc);
	uint32		nruns = run_nruns(rc);
	uint32		i = 0;
	uint32		j = 0;
	uint32		n = 0;

	while (i < na && j < nruns)
	{
		if ((int32) arr[i] < (int32) runs[j].start)
			out[n++] = arr[i++];
		else if ((int32) arr[i] > run_last(&runs[j]))
			j++;
		else
			i++;
	}
	while (i < na)
		out[n++] = arr[i++];
	return n;
}

static uint32
array_and_bitset(const uint16 *arr, uint32 na, const uint64 *w, uint16 *out)
{
	uint32		n = 0;
	uint32		i;

	for (i = 0; i < na; i++)
		if (bits_test(w, arr[i]))
			out[n++] = arr[i];
	return n;
}

static uint32
array_andnot_bitset(const uint16 *arr, uint32 na, const uint64 *w, uint16 *out)
{
	uint32		n = 0;
	uint32		i;

	for (i = 0; i < na; i++)
		if (!bits_test(w, arr[i]))
			out[n++] = arr[i];
	return n;
}

/*
 * Intersect two RUN containers straight into o's run payload.  Returns false
 * (leaving o's payload undefined) if the result needs more than
 * LION_RUN_MAX_NRUNS runs; the caller then falls back to the bitset path.
 */
static bool
run_and_run(const LionContainer *a, const LionContainer *b, LionContainer *o)
{
	const LionRun *ra = run_cdata(a);
	const LionRun *rb = run_cdata(b);
	uint32		na = run_nruns(a);
	uint32		nb = run_nruns(b);
	LionRun	   *out = run_mdata(o);
	uint32		i = 0;
	uint32		j = 0;
	uint32		n = 0;
	uint32		card = 0;

	while (i < na && j < nb)
	{
		int32		ea = run_last(&ra[i]);
		int32		eb = run_last(&rb[j]);
		int32		s = Max((int32) ra[i].start, (int32) rb[j].start);
		int32		e = Min(ea, eb);

		if (s <= e)
		{
			if (n >= LION_RUN_MAX_NRUNS)
				return false;
			out[n].start = (uint16) s;
			out[n].len_minus_1 = (uint16) (e - s);
			n++;
			card += (uint32) (e - s + 1);
		}
		if (ea < eb)
			i++;
		else
			j++;
	}
	o->type = LION_CT_RUN;
	run_set_nruns(o, n);
	o->cardinality = (uint16) card;
	return true;
}

/* Number of members two RUN containers have in common. */
static uint32
run_and_run_cardinality(const LionContainer *a, const LionContainer *b)
{
	const LionRun *ra = run_cdata(a);
	const LionRun *rb = run_cdata(b);
	uint32		na = run_nruns(a);
	uint32		nb = run_nruns(b);
	uint32		i = 0;
	uint32		j = 0;
	uint32		card = 0;

	while (i < na && j < nb)
	{
		int32		ea = run_last(&ra[i]);
		int32		eb = run_last(&rb[j]);
		int32		s = Max((int32) ra[i].start, (int32) rb[j].start);
		int32		e = Min(ea, eb);

		if (s <= e)
			card += (uint32) (e - s + 1);
		if (ea < eb)
			i++;
		else
			j++;
	}
	return card;
}

/* Finish a freshly computed result: optimize and copy into dest. */
static uint32
container_emit_result(LionContainer *o, LionContainer *dest)
{
	lion_container_optimize(o);
	Assert(lion_container_size(o) <= LION_CONTAINER_MAX_SIZE);
	memcpy(dest, o, lion_container_size(o));
	return o->cardinality;
}

uint32
lion_container_and(const LionContainer *a, const LionContainer *b,
				  LionContainer *dest)
{
	LionContainerBuf buf;
	LionContainer *o = &buf.hdr;

	Assert(a->ckey == b->ckey);
	lion_container_init(o, a->ckey);

	if (a->cardinality == 0 || b->cardinality == 0)
	{
		/* empty result */
	}
	else if (a->type == LION_CT_ARRAY || b->type == LION_CT_ARRAY)
	{
		const LionContainer *arr = (a->type == LION_CT_ARRAY) ? a : b;
		const LionContainer *oth = (a->type == LION_CT_ARRAY) ? b : a;
		uint16	   *out = array_mdata(o);
		uint32		n;

		if (oth->type == LION_CT_ARRAY)
			n = array_intersect(array_cdata(a), a->cardinality,
								array_cdata(b), b->cardinality, out);
		else if (oth->type == LION_CT_BITSET)
			n = array_and_bitset(array_cdata(arr), arr->cardinality,
								 bitset_cdata(oth), out);
		else
			n = array_and_run(array_cdata(arr), arr->cardinality, oth, out);
		o->cardinality = (uint16) n;
	}
	else if (a->type == LION_CT_RUN && b->type == LION_CT_RUN &&
			 run_and_run(a, b, o))
	{
		/* done: o already holds the intersected runs */
	}
	else
	{
		uint64	   *w = bitset_mdata(o);

		container_fill_bitset(a, w);
		container_and_bitset(b, w);
		o->type = LION_CT_BITSET;
		o->cardinality = (uint16) bits_cardinality(w);
	}
	return container_emit_result(o, dest);
}

uint32
lion_container_or(const LionContainer *a, const LionContainer *b,
				 LionContainer *dest)
{
	LionContainerBuf buf;
	LionContainer *o = &buf.hdr;

	Assert(a->ckey == b->ckey);
	lion_container_init(o, a->ckey);

	if (a->type == LION_CT_ARRAY && b->type == LION_CT_ARRAY &&
		(uint32) a->cardinality + (uint32) b->cardinality <= LION_ARRAY_MAX_CARD)
	{
		uint32		n = array_union(array_cdata(a), a->cardinality,
									array_cdata(b), b->cardinality,
									array_mdata(o));

		o->cardinality = (uint16) n;
	}
	else if (a->cardinality == 0 || b->cardinality == 0)
	{
		const LionContainer *src = (a->cardinality == 0) ? b : a;

		memcpy(o, src, lion_container_size(src));
		o->ckey = a->ckey;
		o->flags = 0;
	}
	else
	{
		uint64	   *w = bitset_mdata(o);

		container_fill_bitset(a, w);
		container_or_bitset(b, w);
		o->type = LION_CT_BITSET;
		o->cardinality = (uint16) bits_cardinality(w);
	}
	return container_emit_result(o, dest);
}

uint32
lion_container_andnot(const LionContainer *a, const LionContainer *b,
					 LionContainer *dest)
{
	LionContainerBuf buf;
	LionContainer *o = &buf.hdr;

	Assert(a->ckey == b->ckey);
	lion_container_init(o, a->ckey);

	if (a->cardinality == 0)
	{
		/* empty result */
	}
	else if (b->cardinality == 0)
	{
		memcpy(o, a, lion_container_size(a));
		o->flags = 0;
	}
	else if (a->type == LION_CT_ARRAY)
	{
		uint16	   *out = array_mdata(o);
		uint32		n;

		if (b->type == LION_CT_ARRAY)
			n = array_difference(array_cdata(a), a->cardinality,
								 array_cdata(b), b->cardinality, out);
		else if (b->type == LION_CT_BITSET)
			n = array_andnot_bitset(array_cdata(a), a->cardinality,
									bitset_cdata(b), out);
		else
			n = array_andnot_run(array_cdata(a), a->cardinality, b, out);
		o->cardinality = (uint16) n;
	}
	else
	{
		uint64	   *w = bitset_mdata(o);

		container_fill_bitset(a, w);
		container_andnot_bitset(b, w);
		o->type = LION_CT_BITSET;
		o->cardinality = (uint16) bits_cardinality(w);
	}
	return container_emit_result(o, dest);
}

uint32
lion_container_and_cardinality(const LionContainer *a, const LionContainer *b)
{
	Assert(a->ckey == b->ckey);

	if (a->cardinality == 0 || b->cardinality == 0)
		return 0;

	/* BITSET x BITSET: popcount of the ANDed words, nothing materialised */
	if (a->type == LION_CT_BITSET && b->type == LION_CT_BITSET)
	{
		const uint64 *wa = bitset_cdata(a);
		const uint64 *wb = bitset_cdata(b);
		uint32		card = 0;
		uint32		i;

		for (i = 0; i < LION_BITSET_WORDS; i++)
			card += (uint32) pg_popcount64(wa[i] & wb[i]);
		return card;
	}

	if (a->type == LION_CT_ARRAY || b->type == LION_CT_ARRAY)
	{
		const LionContainer *arr = (a->type == LION_CT_ARRAY) ? a : b;
		const LionContainer *oth = (a->type == LION_CT_ARRAY) ? b : a;
		const uint16 *data = array_cdata(arr);
		uint32		n = arr->cardinality;
		uint32		card = 0;
		uint32		i;

		if (oth->type == LION_CT_ARRAY)
		{
			const uint16 *ba = array_cdata(a);
			const uint16 *bb = array_cdata(b);
			uint32		na = a->cardinality;
			uint32		nb = b->cardinality;
			uint32		x = 0;
			uint32		y = 0;

			while (x < na && y < nb)
			{
				if (ba[x] < bb[y])
					x++;
				else if (ba[x] > bb[y])
					y++;
				else
				{
					card++;
					x++;
					y++;
				}
			}
			return card;
		}
		if (oth->type == LION_CT_BITSET)
		{
			const uint64 *w = bitset_cdata(oth);

			for (i = 0; i < n; i++)
				if (bits_test(w, data[i]))
					card++;
			return card;
		}
		/* ARRAY x RUN */
		{
			const LionRun *runs = run_cdata(oth);
			uint32		nruns = run_nruns(oth);
			uint32		j = 0;

			i = 0;
			while (i < n && j < nruns)
			{
				if ((int32) data[i] < (int32) runs[j].start)
					i++;
				else if ((int32) data[i] > run_last(&runs[j]))
					j++;
				else
				{
					card++;
					i++;
				}
			}
			return card;
		}
	}

	if (a->type == LION_CT_RUN && b->type == LION_CT_RUN)
		return run_and_run_cardinality(a, b);

	/* BITSET x RUN */
	{
		const LionContainer *bs = (a->type == LION_CT_BITSET) ? a : b;
		const LionContainer *rc = (a->type == LION_CT_BITSET) ? b : a;
		const uint64 *w = bitset_cdata(bs);
		const LionRun *runs = run_cdata(rc);
		uint32		nruns = run_nruns(rc);
		uint32		card = 0;
		uint32		i;

		for (i = 0; i < nruns; i++)
			card += bits_range_cardinality(w, runs[i].start,
										   (uint32) run_last(&runs[i]));
		return card;
	}
}


/* ----------------------------------------------------------------
 *						structural validation
 * ----------------------------------------------------------------
 */

#define LION_CHECK_FAIL(msg) \
	do { *errmsg = (msg); return false; } while (0)

bool
lion_container_check(const LionContainer *c, Size avail_bytes, const char **errmsg)
{
	uint32		card;

	*errmsg = NULL;

	if (avail_bytes < LION_CONTAINER_HDRSZ)
		LION_CHECK_FAIL("container header does not fit in the available space");

	/*
	 * A sparse segment (DESIGN.md §13) shares this header but is not a
	 * container: it is checked by lion_sparse_check(), and reaching this
	 * function with one means the caller failed to dispatch on the type.
	 */
	if (c->type == LION_CT_SPARSE)
		LION_CHECK_FAIL("item is a sparse segment, not a container");

	if (c->type != LION_CT_ARRAY && c->type != LION_CT_BITSET &&
		c->type != LION_CT_RUN)
		LION_CHECK_FAIL("invalid container type");

	if (c->flags != 0)
		LION_CHECK_FAIL("container flags are not zero");

	card = c->cardinality;
	if (card > LION_CONTAINER_RANGE)
		LION_CHECK_FAIL("container cardinality is out of range");

	switch (c->type)
	{
		case LION_CT_ARRAY:
			{
				const uint16 *arr;
				uint32		i;

				if (card > LION_ARRAY_MAX_CARD)
					LION_CHECK_FAIL("array container has more than LION_ARRAY_MAX_CARD members");
				if (lion_container_size_for(LION_CT_ARRAY, card, 0) > avail_bytes)
					LION_CHECK_FAIL("array container does not fit in the available space");
				arr = array_cdata(c);
				for (i = 0; i < card; i++)
				{
					if ((uint32) arr[i] > LION_LO_MAX)
						LION_CHECK_FAIL("array container member is out of range");
					if (i > 0 && arr[i] <= arr[i - 1])
						LION_CHECK_FAIL("array container members are not strictly ascending");
				}
				break;
			}
		case LION_CT_BITSET:
			{
				if (lion_container_size_for(LION_CT_BITSET, card, 0) > avail_bytes)
					LION_CHECK_FAIL("bitset container does not fit in the available space");
				if (bits_cardinality(bitset_cdata(c)) != card)
					LION_CHECK_FAIL("bitset container cardinality does not match its payload");
				break;
			}
		case LION_CT_RUN:
			{
				const LionRun *runs;
				uint32		nruns;
				uint32		total = 0;
				int32		prev_last = -2;
				uint32		i;

				if (avail_bytes < LION_CONTAINER_HDRSZ + sizeof(uint16))
					LION_CHECK_FAIL("run container header does not fit in the available space");
				nruns = run_nruns(c);
				if (nruns > LION_RUN_MAX_NRUNS)
					LION_CHECK_FAIL("run container has more than LION_RUN_MAX_NRUNS runs");
				if (lion_container_size_for(LION_CT_RUN, card, nruns) > avail_bytes)
					LION_CHECK_FAIL("run container does not fit in the available space");
				if (nruns == 0 && card != 0)
					LION_CHECK_FAIL("run container cardinality does not match its runs");
				runs = run_cdata(c);
				for (i = 0; i < nruns; i++)
				{
					int32		start = (int32) runs[i].start;
					int32		last = run_last(&runs[i]);

					if (last > (int32) LION_LO_MAX)
						LION_CHECK_FAIL("run container run extends past the container range");
					if (start <= prev_last + 1)
						LION_CHECK_FAIL("run container runs are not ascending, disjoint and merged");
					prev_last = last;
					total += (uint32) (last - start + 1);
				}
				if (total != card)
					LION_CHECK_FAIL("run container cardinality does not match its runs");
				break;
			}
	}

	Assert(lion_container_size(c) <= avail_bytes);
	return true;
}
