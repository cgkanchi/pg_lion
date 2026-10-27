/*-------------------------------------------------------------------------
 *
 * lion_slice_enc.h
 *	  The encoding of a BIT-SLICED key column (DESIGN.md §51): which types a
 *	  column may be sliced for, how a value becomes the unsigned integer whose
 *	  bits the slices hold, and how the slices' entries are named.
 *
 *	  A sliced column keeps, beside its VALUE entries, one SLICE entry per
 *	  bit position of the encoded value - slice i holds the rows whose
 *	  encoded value has bit i set - and one EXISTENCE slice that holds every
 *	  row whose value is not NULL.  The encoding is the value as a signed
 *	  integer of its type's width, nbits = 16, 32 or 64, with the sign bit
 *	  flipped: an order-preserving map onto [0, 2^nbits), so that the
 *	  smallest value of the type is 0 and the largest all ones, and the order
 *	  of the encodings is the type's btree order.
 *
 *	  Only c.h and the generated catalog/pg_type_d.h here, so that the next
 *	  piece of work - the planner's answers from slices, and the build's and
 *	  the readers' use of the bit-plane kernel (DESIGN.md §50,
 *	  lion_planes_scatter_strided() takes these encodings) - can unit-test
 *	  the arithmetic without a server, as lion_entryfmt.h and lion_vec_math.h
 *	  are.
 *
 *-------------------------------------------------------------------------
 */
#ifndef LION_SLICE_ENC_H
#define LION_SLICE_ENC_H

#include "c.h"
#include "catalog/pg_type_d.h"

/* The widest encoding: an int8, a time or a timestamp. */
#define LION_SLICE_MAX_BITS		64

/*
 * The one-byte key a SLICE entry is stored with (DESIGN.md §51, "The entry
 * and its order"): the bit position 0 .. nbits - 1, or LION_SLICE_EXISTS for
 * the existence slice, which therefore sorts after every bit slice of its
 * column.
 */
#define LION_SLICE_EXISTS		64
#define LION_SLICE_KEYLEN		1

/* The slices a column of nbits has: its bits and its existence slice. */
#define LION_SLICE_NPOS(nbits)	((nbits) + 1)
/* ... and an array indexed by position, the existence slice last */
#define LION_SLICE_MAX_POS		(LION_SLICE_MAX_BITS + 1)

/* The index into such an array of a key byte, and back. */
static inline int
lion_slice_pos_index(int pos, int nbits)
{
	return (pos == LION_SLICE_EXISTS) ? nbits : pos;
}

static inline int
lion_slice_index_pos(int idx, int nbits)
{
	return (idx == nbits) ? LION_SLICE_EXISTS : idx;
}

/*
 * The width a key type is sliced at, or 0 for a type that is not sliced.
 * The allowlist: pass-by-value types whose btree order is the signed-integer
 * order of the Datum's bits - int2, int4, int8, date (int32 days), time
 * (int64 microseconds), timestamp and timestamptz (int64 microseconds, their
 * infinities the int64 extremes, which sort where the types put them).  Not
 * oid (its order is unsigned), not float4 or float8 (their bits are not in
 * order for negative values, and NaN), not numeric or interval (not one
 * integer); a later version may take them with encodings of their own.
 */
static inline int
lion_slice_type_nbits(Oid typid)
{
	switch (typid)
	{
		case INT2OID:
			return 16;
		case INT4OID:
		case DATEOID:
			return 32;
		case INT8OID:
		case TIMEOID:
		case TIMESTAMPOID:
		case TIMESTAMPTZOID:
			return 64;
		default:
			return 0;
	}
}

/* The highest bit of an nbits encoding: the sign bit, flipped. */
static inline uint64
lion_slice_signbit(int nbits)
{
	Assert(nbits >= 1 && nbits <= LION_SLICE_MAX_BITS);
	return UINT64CONST(1) << (nbits - 1);
}

/* The bits an encoding of nbits may have set. */
static inline uint64
lion_slice_mask(int nbits)
{
	Assert(nbits >= 1 && nbits <= LION_SLICE_MAX_BITS);
	return (nbits == 64) ? PG_UINT64_MAX : ((UINT64CONST(1) << nbits) - 1);
}

/*
 * The encoding of v, a value of a type nbits wide (so -2^(nbits-1) <= v <
 * 2^(nbits-1)): its two's complement bits with the sign bit flipped.
 * Order-preserving: v < w exactly when encode(v) < encode(w).
 */
static inline uint64
lion_slice_encode(int64 v, int nbits)
{
	return (((uint64) v) & lion_slice_mask(nbits)) ^ lion_slice_signbit(nbits);
}

/* ... and back: the value whose encoding is u, sign-extended to 64 bits. */
static inline int64
lion_slice_decode(uint64 u, int nbits)
{
	uint64		x = (u & lion_slice_mask(nbits)) ^ lion_slice_signbit(nbits);

	/* sign-extend from bit nbits - 1 */
	if (nbits < 64 && (x & lion_slice_signbit(nbits)) != 0)
		x |= ~lion_slice_mask(nbits);
	return (int64) x;
}

/*
 * The sum of the values of n rows whose encodings sum to encsum: each
 * encoding is its value plus 2^(nbits-1).  Exact in 128 bits, which is where
 * a sum over the slices is kept (DESIGN.md §51, "The SQL function").
 */
#ifdef HAVE_INT128
static inline int128
lion_slice_value_sum(int128 encsum, int64 n, int nbits)
{
	return encsum - (int128) n * (int128) lion_slice_signbit(nbits);
}
#endif

#endif							/* LION_SLICE_ENC_H */
