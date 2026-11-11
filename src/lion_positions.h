/*-------------------------------------------------------------------------
 * lion_positions.h
 *	  Position chunks for pg_lion: the per-row word positions of a lexeme,
 *	  stored beside its posting set in an index whose build stored them
 *	  (store_positions = auto, the default, or on).
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
 *	  and the payload is
 *
 *		uint16	used: the member bytes, which follow the rest of the header
 *		uint32	last: the last member's code less lion_pos_block_base(block),
 *				or LION_POS_LAST_UNKNOWN for a chunk without members or one
 *				whose last member is that far from its block or further
 *		8 bytes	impacts: what the members' rows can score (below)
 *
 *	  then the members, ascending by heap TID code and unique:
 *
 *		varint	code delta: from lion_pos_block_base(block) for the first
 *				member, and from the previous member's code for the others
 *				(so >= 1)
 *		varint	npos, 0 .. LION_POS_MAX_NPOS; 0 is a member whose tsvector
 *				was stripped of positions
 *		npos varints	(pos delta << 2) | weight, the positions ascending
 *				strictly and below LION_POS_LIMIT, the first one's delta from 0
 *
 *	  IMPACTS (DESIGN.md §17, "Block bounds"): up to LION_POS_NIMPACTS pairs
 *	  of bytes (tf - 1, length code), the unused ones 0xFF 0xFF, such that
 *	  every member is dominated by a pair: its tf - npos, at least 1 - at
 *	  most the pair's, and its row's length (the empty key's position,
 *	  lion_bm25.c) at least the length the pair's code stands for
 *	  (lion_pos_len_value()).  A BM25 term's score rises with tf and falls
 *	  with the length, so the best the chunk's rows can score is the best of
 *	  its pairs, for any k1 and b.  The pairs are kept as a Pareto front -
 *	  tf descending, length code descending - and two neighbours are merged
 *	  into the pair that dominates both when a fifth would not fit.  A member
 *	  whose row's length the writer does not know adds the pair that
 *	  dominates everything (tf 256, length 0).  A member removed leaves the
 *	  pairs as they were, and a split gives both halves the whole chunk's:
 *	  bounds that are no longer tight, never wrong.
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

/* The payload's own header: the member bytes, the last code, the impacts. */
#define LION_POS_USED_OFF		LION_CONTAINER_HDRSZ	/* 8 */
#define LION_POS_LAST_OFF		(LION_POS_USED_OFF + sizeof(uint16))	/* 10 */
#define LION_POS_IMPACTS_OFF	(LION_POS_LAST_OFF + sizeof(uint32))	/* 14 */
#define LION_POS_NIMPACTS		4
#define LION_POS_HDRSZ			(LION_POS_IMPACTS_OFF + 2 * LION_POS_NIMPACTS)	/* 22 */
#define LION_POS_MAX_BYTES		((uint32) (LION_CONTAINER_MAX_SIZE - LION_POS_HDRSZ))	/* 4082 */

#define LION_POS_LAST_UNKNOWN	PG_UINT32_MAX

/*
 * The member bytes a position tree's chunk is filled to by the build and
 * split at by an insert: small enough that its impacts bound a few dozen
 * rows of a common lexeme (DESIGN.md §17, "Block bounds").  A chunk with
 * one member may hold more, up to LION_POS_MAX_BYTES; an INLINE entry's
 * one chunk is as big as the entry lets it be.
 */
#define LION_POS_TREE_BYTES		512

/* The largest length code: the code of 65535, the longest length kept. */
#define LION_POS_LEN_CODE_MAX	207

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

	/*
	 * The row's length, for the chunk's impacts when the member is appended
	 * or inserted: 0 when the writer does not know it (a row's length is at
	 * least 1).  Never stored with the member, so a decoded one has 0.
	 */
	uint16		dl;
	uint16		pos[LION_POS_MAX_NPOS];	/* weight << 14 | pos, ascending by pos */
} LionPosMember;

/* One impact pair, decoded: tf 1 .. LION_POS_MAX_NPOS and a row length. */
typedef struct LionPosImpact
{
	uint16		tf;
	uint16		len;
} LionPosImpact;

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

	memcpy(&used, (const char *) c + LION_POS_USED_OFF, sizeof(uint16));
	return Min((uint32) used, LION_POS_MAX_BYTES);
}

/* The last member's code, when the header records it: false when not. */
static inline bool
lion_poschunk_last(const LionContainer *c, uint64 *code)
{
	uint32		last;

	memcpy(&last, (const char *) c + LION_POS_LAST_OFF, sizeof(uint32));
	if (last == LION_POS_LAST_UNKNOWN || c->cardinality == 0)
		return false;
	*code = lion_pos_block_base(c->ckey) + last;
	return true;
}

/*
 * The length a length code stands for: 0 .. 15 as they are, then sixteen
 * steps an octave, (16 + m) << e for code (e + 1) << 4 | m - so a length
 * coded is at most a sixteenth more than the length the code gives back.
 */
static inline uint32
lion_pos_len_value(uint32 code)
{
	if (code < 16)
		return code;
	return (16 + (code & 15)) << ((code >> 4) - 1);
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
/* ... the member's code and npos only, its positions stepped over */
extern bool lion_poschunk_iter_next_npos(LionPosIter *it, LionPosMember *m);
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

extern uint32 lion_pos_len_code(uint32 len);
extern int	lion_poschunk_impacts(const LionContainer *c, LionPosImpact *imp);

#endif							/* LION_POSITIONS_H */
