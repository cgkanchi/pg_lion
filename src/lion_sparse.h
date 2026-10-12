/*-------------------------------------------------------------------------
 * lion_sparse.h
 *	  Sparse segments for pg_lion.  See DESIGN.md §13.
 *
 *	  A segment is a fourth kind of item (LION_CT_SPARSE) that may appear
 *	  wherever a container may appear: as a page item on a container page, or
 *	  inside an INLINE entry payload.  It holds a sorted list of (ckey, lo)
 *	  pairs - heap TID codes, code = ckey << LION_CONTAINER_BITS | lo - as a
 *	  stream of delta varints behind the 8-byte LionContainer header:
 *
 *		ckey		= the container key of the first pair; the stream's base
 *					  is ckey << LION_CONTAINER_BITS
 *		cardinality	= number of pairs n, 1 .. LION_SPARSE_MAX_PAIRS
 *		type		= LION_CT_SPARSE
 *		flags		= the stream's length in bytes, n .. LION_SPARSE_MAX_BYTES
 *		stream		= n LEB128 varints (seven bits a byte, least significant
 *					  first, the high bit set on every byte but the last):
 *					  the first is the first code minus the base, which is
 *					  its lo and below LION_CONTAINER_RANGE; every other one
 *					  is its code minus the previous code, so at least 1
 *		size		= 8 + flags <= LION_SPARSE_MAX_SIZE (263)
 *
 *	  Pairs are sorted by code and unique, and every varint is in its
 *	  shortest form, so that a set of pairs has exactly one encoding: the
 *	  WAL redo of an in-place insert (LION_OP_SPARSE_INS) has to reproduce
 *	  the bytes, not just the pairs.  A segment covers the ckey range from
 *	  its header ckey to the ckey of its last pair.
 *
 *	  Why a stream: the pairs of a sparse key are spread over the heap, so
 *	  the code deltas are a few hundred to a few hundred thousand, two or
 *	  three bytes as a varint against the six of a (uint32 ckey, uint16 lo)
 *	  pair; and a pair appended above the last one - every insert of a row
 *	  the heap appended - changes the header and adds bytes at the END, so the
 *	  WAL delta of an INLINE entry it lands in is a few dozen bytes instead of
 *	  everything behind a fixed-width ckeys[] array.
 *
 *	  THE CAP is LION_SPARSE_MAX_BYTES = 255 bytes of stream, which is what
 *	  the one-byte flags field holds, and so at most 255 pairs.  It keeps
 *	  every lookup a sequential decode of at most 255 bytes - a lookup by
 *	  ckey, the last ckey of an item (lion_item_last_ckey()), a seek inside a
 *	  segment - cheap enough that no skip table is needed, and every work
 *	  buffer at most LION_SPARSE_MAX_SIZE bytes.  A key of a few hundred
 *	  sparse pairs takes a few segments, eight bytes of header each.
 *
 *	  The point of the format is density: a container costs 12 bytes plus
 *	  MAXALIGN even for a single member, while a pair costs 1-3 bytes in the
 *	  common case and at most LION_SPARSE_VARINT_MAX.  A ckey with fewer than
 *	  LION_SPARSE_THRESHOLD members lives in a segment; from that many on, a
 *	  regular ARRAY container (8 + 2n) gets one.
 *
 *	  THE NON-INTERLEAVING RULE.  Every ckey of a posting set is covered by
 *	  at most one item, and the ckey ranges of the items of a posting set are
 *	  ordered and disjoint: item i's last ckey is strictly below item i+1's
 *	  first ckey, on a page, across a chain and inside an INLINE payload.
 *	  That is what lets every reader keep treating a posting set as one
 *	  ascending run of ckeys, and what the insert path takes such care to
 *	  preserve when it splits a segment around a ckey it has promoted to a
 *	  container.
 *
 *	  Like lion_container.[ch] this module depends only on c.h (plus the
 *	  container library, for building extracted containers), so that it can
 *	  be unit-tested outside the server with -DFRONTEND
 *	  (test/unit/sparse_test.c).  No palloc, no elog.
 *
 *	  Mutators work IN PLACE on a caller-supplied buffer with at least
 *	  LION_SPARSE_MAX_SIZE bytes of capacity (a LION_CONTAINER_MAX_SIZE work
 *	  buffer always qualifies).  Read-only functions accept a pointer directly
 *	  into a page item; the stream is bytes, so a segment inside an INLINE
 *	  payload needs no alignment either, but it is copied out with
 *	  lion_inline_fetch() like every other item, for its header.
 *
 *	  GROWTH IN PLACE: lion_sparse_insert() writes exactly
 *	  lion_sparse_insert_size() bytes and nothing past them, which is what
 *	  the in-place insert (lion_insert_segment_inplace()) and its WAL redo
 *	  (LION_OP_SPARSE_INS) check against the item's allotted length before
 *	  calling it on a page item.  Nothing else may be called on an item in
 *	  place.
 *
 *	  DAMAGED INPUT, as for containers (lion_container.h): every function is
 *	  memory-safe for any bytes behind a LION_CT_SPARSE header, never reads
 *	  more than the header's flags byte of stream - 255 bytes - nor decodes
 *	  more than LION_SPARSE_MAX_PAIRS pairs, never hands a caller a lo at or
 *	  above LION_CONTAINER_RANGE, and never leaves a segment larger than
 *	  LION_SPARSE_MAX_SIZE; what a damaged segment yields is unspecified, and
 *	  lion_sparse_check() is what finds it.
 *-------------------------------------------------------------------------
 */
#ifndef LION_SPARSE_H
#define LION_SPARSE_H

#include "c.h"

#include "lion_container.h"
#include "lion_positions.h"

/* The stream's length lives in the header's one-byte flags field. */
#define LION_SPARSE_MAX_BYTES	255
#define LION_SPARSE_MAX_SIZE	(LION_CONTAINER_HDRSZ + LION_SPARSE_MAX_BYTES)	/* 263 */

/* A pair takes at least one byte of stream. */
#define LION_SPARSE_MAX_PAIRS	((uint32) LION_SPARSE_MAX_BYTES)

/*
 * The longest varint of a code delta: a code has 32 + LION_OFFSET_BITS bits
 * (41 at 8K), seven to a byte.  6 at 8K and 16K, 7 at 32K.
 */
#define LION_SPARSE_VARINT_MAX	((32 + LION_OFFSET_BITS + 6) / 7)

/* The largest code a heap TID makes, and so the largest a pair may have. */
#define LION_SPARSE_MAX_CODE \
	((((uint64) 1) << (32 + LION_OFFSET_BITS)) - 1)

/* A one-pair segment: the header and a varint of its lo, below 2^15. */
#define LION_SPARSE_SINGLETON_MAX	(LION_CONTAINER_HDRSZ + 3)

/*
 * Members of one ckey from which a regular container is used instead of
 * pairs.  An ARRAY container costs 8 + 2n bytes plus its line pointer and
 * MAXALIGN padding on a page; the pairs of one ckey cost one to three bytes
 * each.  Below this many members a ckey lives in a segment.
 */
#define LION_SPARSE_THRESHOLD	4

StaticAssertDecl(LION_SPARSE_MAX_SIZE <= LION_CONTAINER_MAX_SIZE,
				 "pg_lion: a sparse segment must fit a container work buffer");

/* The stream. */
#define LION_SPARSE_STREAM(s) \
	((uint8 *) ((char *) (s) + LION_CONTAINER_HDRSZ))
#define LION_SPARSE_STREAM_CONST(s) \
	((const uint8 *) ((const char *) (s) + LION_CONTAINER_HDRSZ))

/* Bytes of stream the header claims. */
static inline uint32
lion_sparse_nbytes(const LionContainer *s)
{
	return (uint32) s->flags;
}

/* Size of a segment with nbytes bytes of stream. */
static inline Size
lion_sparse_size_for(uint32 nbytes)
{
	return LION_CONTAINER_HDRSZ + (Size) nbytes;
}

/* Never above LION_SPARSE_MAX_SIZE, whatever the header says. */
static inline Size
lion_sparse_size(const LionContainer *s)
{
	Assert(s->type == LION_CT_SPARSE);
	return lion_sparse_size_for(lion_sparse_nbytes(s));
}

static inline bool
lion_item_is_sparse(const LionContainer *item)
{
	return item->type == LION_CT_SPARSE;
}

/* Bytes a varint of v takes. */
static inline uint32
lion_sparse_varint_size(uint64 v)
{
	uint32		n = 1;

	while (v >= 0x80)
	{
		v >>= 7;
		n++;
	}
	return n;
}

/*
 * A forward reader over a segment's pairs:
 *
 *		LionSparseIter it;
 *
 *		lion_sparse_iter_init(&it, s);
 *		while (lion_sparse_iter_next(&it))
 *			... lion_code_ckey(it.code), lion_code_lo(it.code) ...
 *
 * It stops after the header's pair count or at the end of the stream,
 * whichever comes first, and never reads past the stream.
 */
typedef struct LionSparseIter
{
	const uint8 *p;
	const uint8 *end;
	uint64		code;			/* the pair just decoded, or the base */
	uint32		left;			/* pairs the header still promises */
} LionSparseIter;

static inline void
lion_sparse_iter_init(LionSparseIter *it, const LionContainer *s)
{
	it->p = LION_SPARSE_STREAM_CONST(s);
	it->end = it->p + lion_sparse_nbytes(s);
	it->code = ((uint64) s->ckey) << LION_CONTAINER_BITS;
	it->left = Min((uint32) s->cardinality, LION_SPARSE_MAX_PAIRS);
}

static inline bool
lion_sparse_iter_next(LionSparseIter *it)
{
	const uint8 *p = it->p;
	uint64		v;
	uint32		b;

	if (it->left == 0 || p >= it->end)
		return false;

	b = *p++;
	v = b & 0x7F;
	if (unlikely(b & 0x80))
	{
		int			shift = 7;

		do
		{
			if (p >= it->end || shift > 63)
			{
				it->left = 0;	/* truncated or overlong: damaged */
				return false;
			}
			b = *p++;
			v |= ((uint64) (b & 0x7F)) << shift;
			shift += 7;
		} while (b & 0x80);
	}

	it->p = p;
	it->code += v;
	it->left--;
	return true;
}

/* The container key and lo of the pair an iterator stands on. */
static inline uint32
lion_sparse_iter_ckey(const LionSparseIter *it)
{
	return lion_code_ckey(it->code);
}

static inline uint16
lion_sparse_iter_lo(const LionSparseIter *it)
{
	return lion_code_lo(it->code);
}

/*
 * The number of pairs the header claims, clamped to LION_SPARSE_MAX_PAIRS;
 * a reader may get fewer out of a damaged stream.
 */
static inline uint32
lion_sparse_npairs(const LionContainer *s)
{
	return Min((uint32) s->cardinality, LION_SPARSE_MAX_PAIRS);
}

/*
 * The container key of the last pair: a decode of the stream.  An empty
 * segment is never stored, but a damaged page can hold one: it covers its
 * header ckey, like a container.  The readers refuse one anyway
 * (lion_inline_fetch(), lion_page_item_fetch()).
 */
extern uint32 lion_sparse_last_ckey(const LionContainer *s);

/*
 * Size of any item of a posting set, container or segment - or of a position
 * chunk, the item of a position tree, which shares the page code.  Everything
 * that derives an item's length from its header goes through this.
 */
static inline Size
lion_item_size(const LionContainer *item)
{
	if (item->type == LION_CT_SPARSE)
		return lion_sparse_size(item);
	if (item->type == LION_CT_POSITIONS)
		return lion_poschunk_size(item);
	return lion_container_size(item);
}

/* First ckey an item covers (the header field, for every item kind). */
static inline uint32
lion_item_first_ckey(const LionContainer *item)
{
	return item->ckey;
}

/* Last ckey an item covers: a container covers one, a segment a range. */
static inline uint32
lion_item_last_ckey(const LionContainer *item)
{
	if (item->type == LION_CT_SPARSE)
		return lion_sparse_last_ckey(item);
	return item->ckey;
}

/* Does an item cover ckey? */
static inline bool
lion_item_covers(const LionContainer *item, uint32 ckey)
{
	return ckey >= lion_item_first_ckey(item) && ckey <= lion_item_last_ckey(item);
}

/* Number of members an item holds (pairs, for a segment). */
static inline uint32
lion_item_cardinality(const LionContainer *item)
{
	return item->cardinality;
}

extern bool lion_sparse_contains(const LionContainer *s, uint32 ckey, uint16 lo);

/* Is (ckey, lo) a member of this item?  The item must cover ckey. */
static inline bool
lion_item_contains(const LionContainer *item, uint32 ckey, uint16 lo)
{
	Assert(lion_item_covers(item, ckey));
	if (item->type == LION_CT_SPARSE)
		return lion_sparse_contains(item, ckey, lo);
	return lion_container_contains(item, lo);
}

/* Initialise an empty segment in buf.  ckey is a placeholder until the first
 * pair is inserted; an empty segment must never be stored. */
extern void lion_sparse_init(LionContainer *s, uint32 ckey);

/*
 * Range of pairs belonging to ckey: *first receives the index of the first
 * one (or of the insert position when there is none) and *count how many
 * there are.  A decode of the stream up to the end of ckey's pairs.
 */
extern void lion_sparse_find(const LionContainer *s, uint32 ckey,
							uint32 *first, uint32 *count);

/* Number of members of ckey inside the segment. */
extern uint32 lion_sparse_count(const LionContainer *s, uint32 ckey);

/*
 * The size the segment would have with (ckey, lo) inserted: its current
 * size when the pair is already there, and 0 when the pair does not fit
 * (LION_SPARSE_MAX_BYTES).  lion_sparse_insert() writes exactly that many
 * bytes.
 */
extern Size lion_sparse_insert_size(const LionContainer *s, uint32 ckey,
									uint16 lo);

/*
 * Insert one pair in sorted position.  Returns false and changes nothing if
 * it does not fit (lion_sparse_insert_size() is 0); returns true with *dup
 * set (and nothing changed) when the pair is already there.  dup may be
 * NULL.  The segment is rewritten from the insertion point on: a pair above
 * every other one changes the header and the bytes it adds at the end, and
 * nothing else.
 */
extern bool lion_sparse_insert(LionContainer *s, uint32 ckey, uint16 lo,
							  bool *dup);

/*
 * Append (ckey, lo), whose code must be above every pair's, given the code
 * of the last pair in *last (ignored when the segment is empty), which is
 * updated.  The bulk build's way to fill a segment without decoding it.
 * Returns false and changes nothing when the pair does not fit.
 */
extern bool lion_sparse_append(LionContainer *s, uint64 *last, uint32 ckey,
							  uint16 lo);

/* Remove one pair; returns true if it was there. */
extern bool lion_sparse_remove(LionContainer *s, uint32 ckey, uint16 lo);

/*
 * Remove every pair for which pred() returns true (VACUUM).  Returns the
 * number removed; the caller deletes the segment when cardinality reaches 0.
 * A segment can only shrink - a varint of the sum of two deltas is no longer
 * than the two - so this never needs more room.
 */
typedef bool (*lion_pair_predicate) (uint32 ckey, uint16 lo, void *arg);
extern uint32 lion_sparse_remove_if(LionContainer *s, lion_pair_predicate pred,
								   void *arg);

/*
 * Move every member of ckey out of the segment into out, which must have
 * LION_CONTAINER_MAX_SIZE bytes and receives an ARRAY container (not
 * optimized: the caller may still add members to it).  Returns the number of
 * members moved, which is 0 when the segment does not cover ckey at all.
 */
extern uint32 lion_sparse_extract(LionContainer *s, uint32 ckey,
								 LionContainer *out);

/*
 * Split s in half at a ckey boundary, so that no ckey ends up in both parts.
 * Returns false (and leaves left/right undefined) when every pair of s has
 * the same ckey and no such boundary exists.
 */
extern bool lion_sparse_split_half(const LionContainer *s, LionContainer *left,
								  LionContainer *right);

/*
 * Split s around ckey: left receives the pairs below it, right the pairs
 * above it.  s must not contain ckey itself any more (extract it first).
 * Either part may come back empty; the caller drops empty parts.
 */
extern void lion_sparse_split_at(const LionContainer *s, uint32 ckey,
								LionContainer *left, LionContainer *right);

/*
 * Concatenate two adjacent segments (every ckey of a is below every ckey of
 * b) into out.  Returns false if they are not adjacent in that sense or if
 * the result would not fit.
 */
extern bool lion_sparse_merge(const LionContainer *a, const LionContainer *b,
							 LionContainer *out);

/* Iterate the pairs in (ckey, lo) order; the callback stops on false. */
typedef bool (*lion_pair_callback) (uint32 ckey, uint16 lo, void *arg);
extern void lion_sparse_iterate(const LionContainer *s, lion_pair_callback cb,
							   void *arg);

/*
 * Structural validation for lion_index_verify(): type, the stream's length
 * against avail_bytes, the pair count (an empty segment is refused: one is
 * never stored) against what the stream decodes to, every varint in its
 * shortest form and the stream ending exactly after the last one, the first
 * lo in range, the pairs strictly ascending, and every code within what a
 * heap TID can make.  Returns false and sets *errmsg (a static string) on
 * failure.
 */
extern bool lion_sparse_check(const LionContainer *s, Size avail_bytes,
							 const char **errmsg);

/*
 * For lion_index_verify(), of a segment lion_sparse_check() has passed: every
 * pair's lo is a heap tuple (lion_lo_is_tuple(), lion_container.h), maxoff
 * being MaxHeapTuplesPerPage.  Returns false and sets *errmsg (a static
 * string) on failure.
 */
extern bool lion_sparse_check_offsets(const LionContainer *s, uint32 maxoff,
									 const char **errmsg);

#endif							/* LION_SPARSE_H */
