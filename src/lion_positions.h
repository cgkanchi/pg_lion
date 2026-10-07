/*-------------------------------------------------------------------------
 * lion_positions.h
 *	  Position chunks for pg_lion: the per-row word positions of a lexeme,
 *	  stored beside its posting set in an index built WITH
 *	  (store_positions = true).
 *
 *	  A chunk is a sixth kind of item (LION_CT_POSITIONS).  It shares the
 *	  8-byte LionContainer header, and it never appears among the items of a
 *	  posting set: chunks live in a key's position tree, or in the positions
 *	  section at the end of an INLINE entry's payload.  The header is
 *
 *		ckey		= the chunk's BLOCK, not a container key: a lower bound on
 *					  the heap block of every member, the first member's block
 *					  when the chunk was written, which removing members may
 *					  leave below the new first one
 *		cardinality	= number of members, 0 .. LION_POS_MAX_MEMBERS
 *		type		= LION_CT_POSITIONS
 *		flags		= reserved, 0
 *
 *	  and the payload is a uint16 count of the member bytes that follow, then
 *	  the members, ascending by heap TID code and unique:
 *
 *		varint	code delta: from lion_pos_block_base(block) for the first
 *				member, and from the previous member's code for the others
 *				(so >= 1)
 *		varint	npos, 0 .. LION_POS_MAX_NPOS; 0 is a member whose tsvector
 *				was stripped of positions
 *		npos varints	(pos delta << 2) | weight, the positions ascending
 *				strictly and below LION_POS_LIMIT, the first one's delta from 0
 *
 *	  Varints are LEB128: seven bits a byte, least significant first, the
 *	  high bit set on every byte but the last.  A member decodes to the
 *	  tsvector's own WordEntryPos form (weight << 14 | pos), which is what
 *	  TS_execute()'s callback hands to ExecPhraseData.
 *
 *	  A chunk may cover members of several heap blocks, and the members of
 *	  one block may continue in the next chunk of the same tree: separators
 *	  of a position tree are, like the posting tree's (§22), non-decreasing,
 *	  and they are blocks too.  A block rather than a container key because a
 *	  run of equal separators is walked, not searched (lion_postree.c): one
 *	  heap page's rows bound it, where a container key's are 64 pages' worth.
 *
 *	  Like lion_container.[ch] and lion_sparse.[ch] this module depends only
 *	  on c.h, so test/unit/positions_test.c runs it outside the server.  No
 *	  palloc, no elog.  Mutators work on a caller-supplied buffer of the
 *	  capacity they are given, and fail (returning false, the buffer
 *	  unchanged) when the result would not fit it.
 *
 *	  DAMAGED INPUT, as for containers: every function is memory-safe for
 *	  any bytes behind a LION_CT_POSITIONS header, never reads past
 *	  lion_poschunk_size() - capped at LION_CONTAINER_MAX_SIZE - and never
 *	  hands a caller more than LION_POS_MAX_NPOS positions, a position at or
 *	  above LION_POS_LIMIT, or codes that do not ascend.  What a damaged
 *	  chunk yields beyond that is unspecified, and lion_poschunk_check() is
 *	  what finds it.
 *-------------------------------------------------------------------------
 */
#ifndef LION_POSITIONS_H
#define LION_POSITIONS_H

#include "c.h"

#include "lion_container.h"

/* A chunk's block: the heap block of a code, and the first code of a block. */
static inline uint32
lion_pos_block(uint64 code)
{
	return (uint32) (code >> LION_OFFSET_BITS);
}

static inline uint64
lion_pos_block_base(uint32 block)
{
	return ((uint64) block) << LION_OFFSET_BITS;
}

/* The largest code a heap TID makes, and so the largest a member may have. */
#define LION_POS_MAX_CODE \
	((((uint64) 1) << (32 + LION_OFFSET_BITS)) - 1)

/* tsvector's limits (tsearch/ts_type.h): MAXNUMPOS and MAXENTRYPOS. */
#define LION_POS_MAX_NPOS		256
#define LION_POS_LIMIT			(1 << 14)
#define LION_POS_POS(wep)		((uint16) ((wep) & (LION_POS_LIMIT - 1)))
#define LION_POS_WEIGHT(wep)	((uint16) ((wep) >> 14))
#define LION_POS_MAKE(pos, weight)	((uint16) (((weight) << 14) | (pos)))

/* The uint16 member-byte count that opens the payload. */
#define LION_POS_HDRSZ			(LION_CONTAINER_HDRSZ + sizeof(uint16))	/* 10 */
#define LION_POS_MAX_BYTES		((uint32) (LION_CONTAINER_MAX_SIZE - LION_POS_HDRSZ))	/* 4094 */

/* A member is at least two bytes: a code delta and npos. */
#define LION_POS_MAX_MEMBERS	(LION_POS_MAX_BYTES / 2)

/* Longest varint of a 64-bit value, and the longest member. */
#define LION_POS_VARINT_MAX		10
#define LION_POS_MEMBER_MAX \
	(LION_POS_VARINT_MAX + 2 + LION_POS_MAX_NPOS * 3)	/* 780 */

/* One decoded member. */
typedef struct LionPosMember
{
	uint64		code;			/* lion_tid_to_code() of the heap TID */
	uint16		npos;
	uint16		pos[LION_POS_MAX_NPOS];	/* weight << 14 | pos, ascending by pos */
} LionPosMember;

/* A forward reader over a chunk's members. */
typedef struct LionPosIter
{
	const uint8 *p;
	const uint8 *end;
	uint32		left;			/* members the header still promises */
	uint64		code;			/* the last code handed out, or the base */
	bool		first;
} LionPosIter;

static inline bool
lion_item_is_positions(const LionContainer *item)
{
	return item->type == LION_CT_POSITIONS;
}

/* The member bytes the header claims, capped at what a chunk can hold. */
static inline uint32
lion_poschunk_used(const LionContainer *c)
{
	uint16		used;

	memcpy(&used, (const char *) c + LION_CONTAINER_HDRSZ, sizeof(uint16));
	return Min((uint32) used, LION_POS_MAX_BYTES);
}

/* Size of the chunk (header, byte count and members), never above 4104. */
static inline Size
lion_poschunk_size(const LionContainer *c)
{
	return LION_POS_HDRSZ + lion_poschunk_used(c);
}

/* Bytes a varint of v takes. */
static inline uint32
lion_pos_varint_size(uint64 v)
{
	uint32		n = 1;

	while (v >= 0x80)
	{
		v >>= 7;
		n++;
	}
	return n;
}

extern void lion_poschunk_init(LionContainer *c, uint32 block);
extern void lion_poschunk_iter_init(LionPosIter *it, const LionContainer *c);
extern bool lion_poschunk_iter_next(LionPosIter *it, LionPosMember *m);
extern bool lion_poschunk_find(const LionContainer *c, uint64 code, LionPosMember *m);
extern bool lion_poschunk_last_code(const LionContainer *c, uint64 *code);

extern Size lion_posmember_size(uint64 prev_code, const LionPosMember *m);
extern Size lion_poschunk_append_need(const LionContainer *c, const LionPosMember *m);
extern bool lion_poschunk_append(LionContainer *c, Size cap, const LionPosMember *m);

/* Where a chunk's last member ends, kept by a caller that appends many. */
typedef struct LionPosTail
{
	bool		valid;			/* false: found again at the next append */
	bool		any;			/* the chunk has a member */
	uint64		last;			/* its last member's code */
	uint32		used;			/* member bytes up to its end */
	uint32		n;				/* members */
} LionPosTail;

extern bool lion_poschunk_append_tail(LionContainer *c, Size cap,
									  const LionPosMember *m, LionPosTail *tail);
extern bool lion_poschunk_insert(LionContainer *c, Size cap, const LionPosMember *m,
								 bool *replaced);

typedef bool (*LionPosRemoveCallback) (uint64 code, void *arg);
extern uint32 lion_poschunk_remove(LionContainer *c, LionPosRemoveCallback cb, void *arg);
extern bool lion_poschunk_split(const LionContainer *c, LionContainer *left,
								LionContainer *right);

extern bool lion_poschunk_check(const LionContainer *c, Size itemsz,
								char *errbuf, Size errlen);

#endif							/* LION_POSITIONS_H */
