/*-------------------------------------------------------------------------
 * lion_waldelta.h
 *	  How the WAL describes a rewritten item by what changed in it
 *	  (DESIGN.md §25, LION_OP_DELTA and LION_OP_SPLICE).
 *
 *	  An item a writer overwrites - a container on a posting leaf, an entry
 *	  tuple on a directory leaf - is logged against the item the page holds
 *	  now, its OLD image, in one of two forms:
 *
 *	  RANGES	the new item, `newlen` bytes, is the old image cut to newlen
 *				or zero-extended to it, with a list of byte ranges copied
 *				over it: a packed sequence of {uint16 at, uint16 len, len
 *				bytes}, applied in order.
 *
 *	  SPLICE	the same list of ranges, applied to the old image with
 *				`inslen` bytes INSERTED at byte `at` (and then cut to newlen
 *				or zero-extended to it).  The payload is {uint16 at, uint16
 *				inslen, inslen bytes} and then the ranges.  This is the shape
 *				of one member added to an ARRAY - two bytes inserted, two
 *				bytes of cardinality changed - and of a run added to a RUN,
 *				a pair added inside a sparse segment and either of those
 *				inside an INLINE payload: everything above the insertion
 *				point moves up by a few bytes, which RANGES can only say by
 *				repeating every byte that moved.
 *
 *	  The encoder works out both and keeps the smaller; the decoder rebuilds
 *	  the new item from the old image and the payload, byte for byte, which
 *	  is what makes the redo exact.  The old image is the item at its
 *	  ALLOCATED length (ItemIdGetLength), slack included, on both sides.
 *
 *	  Frontend-safe (c.h only), so that test/unit/waldelta_test.c can feed
 *	  it every shape without a server.
 *-------------------------------------------------------------------------
 */
#ifndef LION_WALDELTA_H
#define LION_WALDELTA_H

/*
 * How many equal bytes it takes to close a range.  A range costs four bytes
 * of header, so a handful of equal bytes is cheaper carried along than
 * skipped: a range ends only after this many in a row.
 */
#define LION_DELTA_GAP			5

/* The bytes of one range header and of the SPLICE header. */
#define LION_DELTA_RANGE_HDR	4
#define LION_SPLICE_HDR			4

/*
 * The insertions the encoder looks for: 1 .. this many bytes.  An ARRAY
 * member is 2, a RUN's run 4, a sparse pair a varint of 1 to 3 bytes plus
 * the re-encoded varint after it; 8 covers all of them with room.
 */
#define LION_SPLICE_MAX_SHIFT	8

/* What lion_delta_encode_item() chose. */
#define LION_DELTA_KIND_NONE	0	/* neither is worth it: log the item whole */
#define LION_DELTA_KIND_RANGES	1
#define LION_DELTA_KIND_SPLICE	2

/*
 * Encode the ranges in which cur[0, len) differs from base[0, len) into out.
 * Returns the bytes written, or -1 when that would be more than cap (out is
 * then garbage).  No range ever reaches past len.
 */
extern int	lion_delta_ranges(const char *base, const char *cur, Size len,
							  char *out, Size cap);

/*
 * Copy a list of ranges over target[0, len).  False, with target partly
 * written, when the list is malformed: truncated, or a range past len.
 */
extern bool lion_delta_apply_ranges(char *target, Size len,
									const char *ranges, Size rlen);

/*
 * Is cur[0, curlen) the old image old[0, oldlen) with `shift` bytes inserted,
 * from some byte on?  Both are read zero-extended past their ends.  On true,
 * *at is the smallest offset with cur[at + shift, curlen) equal to the old
 * image from `at` on - everything from there up moved by `shift` - and at is
 * within the old image (at <= oldlen) and the inserted bytes within cur.  The
 * bytes below at may differ in any way; ranges say how.
 */
extern bool lion_splice_find(const char *old, Size oldlen,
							 const char *cur, Size curlen, Size shift,
							 Size *at);

/*
 * The base a SPLICE's ranges apply to: old[0, at), ins[0, inslen), old[at,
 * oldlen), cut to newlen or zero-extended to it, written to base[0, newlen).
 * at <= oldlen.
 */
extern void lion_splice_base(const char *old, Size oldlen, Size at,
							 const char *ins, Size inslen,
							 char *base, Size newlen);

/*
 * Encode cur[0, len) against the old image old[0, oldlen) as RANGES or SPLICE,
 * whichever is smaller, into out (at most cap bytes; cap is the budget, and
 * an encoding that needs more is not taken).  scratch is len bytes of work
 * space.  Returns the payload length and sets *kind; *kind is
 * LION_DELTA_KIND_NONE (and the result 0) when neither fits cap.
 *
 * RANGES wins ties, so an item whose change is not an insertion is logged
 * exactly as before SPLICE existed.
 */
extern Size lion_delta_encode_item(const char *old, Size oldlen,
								   const char *cur, Size len,
								   char *out, Size cap, char *scratch,
								   int *kind);

/*
 * Rebuild the new item, newlen bytes, into out from the old image and a
 * payload of the given kind.  False when the payload is malformed for that
 * old image (a SPLICE point past it, an insertion that does not fit, a range
 * past newlen, trailing bytes): the record was not written against it.
 * out must not overlap old.
 */
extern bool lion_delta_decode_item(const char *old, Size oldlen, int kind,
								   const char *payload, Size plen,
								   char *out, Size newlen);

#endif							/* LION_WALDELTA_H */
