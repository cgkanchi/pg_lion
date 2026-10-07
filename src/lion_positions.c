/*-------------------------------------------------------------------------
 * lion_positions.c
 *	  Position chunks for pg_lion.  See lion_positions.h for the format.
 *
 *	  Every reader goes through decode_member(), which is the one place that
 *	  turns bytes into a member and the one place that decides a chunk is
 *	  damaged: the iterator stops there, and lion_poschunk_check() reports
 *	  why.  The mutators that rewrite a chunk (insert, remove, split) work
 *	  from a copy of it, so they can re-encode into the caller's buffer in
 *	  place; a chunk is at most LION_CONTAINER_MAX_SIZE bytes, so the copy is
 *	  a stack buffer.
 *-------------------------------------------------------------------------
 */
#ifndef FRONTEND
#include "postgres.h"
#else
#include "c.h"
#endif

#include <stdio.h>
#include <string.h>

#include "lion_positions.h"

typedef enum PosDecode
{
	POS_OK,
	POS_END,					/* no bytes left */
	POS_TRUNCATED,				/* a varint or a member runs past the end */
	POS_VARINT,					/* a varint longer than ten bytes */
	POS_ORDER,					/* a code that does not ascend */
	POS_NPOS,					/* npos above LION_POS_MAX_NPOS */
	POS_POSORDER,				/* positions that do not ascend */
	POS_POSLIMIT,				/* a position at or above LION_POS_LIMIT */
	POS_CODE					/* a code above LION_POS_MAX_CODE */
} PosDecode;

static const char *const pos_decode_msg[] = {
	"ok",
	"member bytes end early",
	"member truncated",
	"varint longer than ten bytes",
	"member codes do not ascend",
	"too many positions in a member",
	"positions do not ascend",
	"position out of range",
	"member code beyond the largest heap TID"
};

static inline uint16
pos_used_raw(const LionContainer *c)
{
	uint16		used;

	memcpy(&used, (const char *) c + LION_CONTAINER_HDRSZ, sizeof(uint16));
	return used;
}

static inline void
pos_set_used(LionContainer *c, uint32 used)
{
	uint16		u = (uint16) used;

	Assert(used <= LION_POS_MAX_BYTES);
	memcpy((char *) c + LION_CONTAINER_HDRSZ, &u, sizeof(uint16));
}

static inline uint8 *
pos_members(LionContainer *c)
{
	return (uint8 *) c + LION_POS_HDRSZ;
}

static inline const uint8 *
pos_members_const(const LionContainer *c)
{
	return (const uint8 *) c + LION_POS_HDRSZ;
}

static inline uint64
pos_base(const LionContainer *c)
{
	return lion_pos_block_base(c->ckey);
}

static PosDecode
read_varint(const uint8 **pp, const uint8 *end, uint64 *v)
{
	const uint8 *p = *pp;
	uint64		r = 0;
	int			i;

	for (i = 0; i < LION_POS_VARINT_MAX; i++)
	{
		uint8		b;

		if (p >= end)
			return POS_TRUNCATED;
		b = *p++;
		if (i < 9)
			r |= ((uint64) (b & 0x7F)) << (7 * i);
		else
			r |= ((uint64) (b & 0x01)) << 63;
		if ((b & 0x80) == 0)
		{
			*pp = p;
			*v = r;
			return POS_OK;
		}
	}
	return POS_VARINT;
}

static inline uint8 *
write_varint(uint8 *p, uint64 v)
{
	while (v >= 0x80)
	{
		*p++ = (uint8) (v | 0x80);
		v >>= 7;
	}
	*p++ = (uint8) v;
	return p;
}

/*
 * Decode one member at *pp, whose code is relative to prev (the chunk's base
 * when first).  On POS_OK, *pp is past the member and m holds it.
 */
static PosDecode
decode_member(const uint8 **pp, const uint8 *end, uint64 prev, bool first,
			  LionPosMember *m)
{
	const uint8 *p = *pp;
	uint64		delta;
	uint64		npos;
	uint32		pos = 0;
	PosDecode	r;
	uint32		i;

	if (p >= end)
		return POS_END;
	if ((r = read_varint(&p, end, &delta)) != POS_OK)
		return r;
	if ((!first && delta == 0) || prev + delta < prev)
		return POS_ORDER;
	if (prev + delta > LION_POS_MAX_CODE)
		return POS_CODE;
	m->code = prev + delta;
	if ((r = read_varint(&p, end, &npos)) != POS_OK)
		return r;
	if (npos > LION_POS_MAX_NPOS)
		return POS_NPOS;
	m->npos = (uint16) npos;
	for (i = 0; i < npos; i++)
	{
		uint64		v;
		uint64		d;

		if ((r = read_varint(&p, end, &v)) != POS_OK)
			return r;
		d = v >> 2;
		if (i > 0 && d == 0)
			return POS_POSORDER;
		if (d >= LION_POS_LIMIT || pos + d >= LION_POS_LIMIT)
			return POS_POSLIMIT;
		pos += (uint32) d;
		m->pos[i] = LION_POS_MAKE(pos, (uint16) (v & 3));
	}
	*pp = p;
	return POS_OK;
}

/*
 * Step over one member at *pp without decoding its positions, for the walks
 * that only want codes: its code goes to *code and *pp past it.  Positions
 * are counted by their varints' last bytes, so a damaged member can be
 * stepped over at a boundary decode_member() would refuse; nothing a caller
 * hands out comes from here without going through decode_member() too.
 */
static PosDecode
skip_member(const uint8 **pp, const uint8 *end, uint64 prev, bool first,
			uint64 *code)
{
	const uint8 *p = *pp;
	uint64		delta;
	uint64		npos;
	PosDecode	r;

	if (p >= end)
		return POS_END;
	if ((r = read_varint(&p, end, &delta)) != POS_OK)
		return r;
	if ((!first && delta == 0) || prev + delta < prev)
		return POS_ORDER;
	if (prev + delta > LION_POS_MAX_CODE)
		return POS_CODE;
	if ((r = read_varint(&p, end, &npos)) != POS_OK)
		return r;
	if (npos > LION_POS_MAX_NPOS)
		return POS_NPOS;
	while (npos > 0)
	{
		if (p >= end)
			return POS_TRUNCATED;
		if ((*p++ & 0x80) == 0)
			npos--;
	}
	*code = prev + delta;
	*pp = p;
	return POS_OK;
}

/* Is m a member this module may write: positions ascending and in range? */
static bool
member_valid(const LionPosMember *m)
{
	uint32		i;

	if (m->npos > LION_POS_MAX_NPOS || m->code > LION_POS_MAX_CODE)
		return false;
	for (i = 0; i < m->npos; i++)
	{
		if (i > 0 && LION_POS_POS(m->pos[i]) <= LION_POS_POS(m->pos[i - 1]))
			return false;
	}
	return true;
}

/* Encoded size of m after a member (or base) of code prev; prev <= m->code. */
Size
lion_posmember_size(uint64 prev_code, const LionPosMember *m)
{
	Size		sz;
	uint32		pos = 0;
	uint32		i;

	Assert(m->code >= prev_code);
	sz = lion_pos_varint_size(m->code - prev_code) + lion_pos_varint_size(m->npos);
	for (i = 0; i < m->npos; i++)
	{
		uint32		p = LION_POS_POS(m->pos[i]);

		sz += lion_pos_varint_size(((uint64) (p - pos) << 2) | LION_POS_WEIGHT(m->pos[i]));
		pos = p;
	}
	return sz;
}

static uint8 *
encode_member(uint8 *dst, uint64 prev_code, const LionPosMember *m)
{
	uint32		pos = 0;
	uint32		i;

	dst = write_varint(dst, m->code - prev_code);
	dst = write_varint(dst, m->npos);
	for (i = 0; i < m->npos; i++)
	{
		uint32		p = LION_POS_POS(m->pos[i]);

		dst = write_varint(dst, ((uint64) (p - pos) << 2) | LION_POS_WEIGHT(m->pos[i]));
		pos = p;
	}
	return dst;
}

void
lion_poschunk_init(LionContainer *c, uint32 block)
{
	c->ckey = block;
	c->cardinality = 0;
	c->type = LION_CT_POSITIONS;
	c->flags = 0;
	pos_set_used(c, 0);
}

void
lion_poschunk_iter_init(LionPosIter *it, const LionContainer *c)
{
	it->p = pos_members_const(c);
	it->end = it->p + lion_poschunk_used(c);
	it->left = c->cardinality;
	it->code = pos_base(c);
	it->first = true;
}

/*
 * The next member, or false at the end - or at the first damage, after
 * which the iterator hands out nothing more.
 */
bool
lion_poschunk_iter_next(LionPosIter *it, LionPosMember *m)
{
	if (it->left == 0)
		return false;
	if (decode_member(&it->p, it->end, it->code, it->first, m) != POS_OK)
	{
		it->left = 0;
		return false;
	}
	it->code = m->code;
	it->first = false;
	it->left--;
	return true;
}

bool
lion_poschunk_iter_next_npos(LionPosIter *it, LionPosMember *m)
{
	const uint8 *p = it->p;
	uint64		npos;

	if (it->left == 0)
		return false;
	/* skip_member() checks the member as decode_member() does, bar positions */
	if (skip_member(&it->p, it->end, it->code, it->first, &m->code) != POS_OK)
	{
		it->left = 0;
		return false;
	}
	/* npos is the second varint, already found sound by skip_member() */
	{
		uint64		delta;

		(void) read_varint(&p, it->end, &delta);
		(void) read_varint(&p, it->end, &npos);
	}
	m->npos = (uint16) npos;
	it->code = m->code;
	it->first = false;
	it->left--;
	return true;
}

bool
lion_poschunk_find(const LionContainer *c, uint64 code, LionPosMember *m)
{
	LionPosIter it;

	lion_poschunk_iter_init(&it, c);
	while (it.left > 0)
	{
		const uint8 *at = it.p;
		uint64		mcode;

		if (skip_member(&it.p, it.end, it.code, it.first, &mcode) != POS_OK ||
			mcode > code)
			break;
		if (mcode == code)
		{
			/* the one member handed out is decoded, and checked, in full */
			it.p = at;
			return lion_poschunk_iter_next(&it, m) && m->code == code;
		}
		it.code = mcode;
		it.first = false;
		it.left--;
	}
	return false;
}

/*
 * The last member's code.  The last member is found by walking the chunk,
 * which the append path does once per call; chunks are small.
 */
static bool
pos_last(const LionContainer *c, uint64 *code, const uint8 **endp, uint32 *nread)
{
	LionPosIter it;
	LionPosMember m;
	uint32		n = 0;

	lion_poschunk_iter_init(&it, c);
	while (lion_poschunk_iter_next(&it, &m))
	{
		*code = m.code;
		n++;
	}
	if (endp)
		*endp = it.p;
	if (nread)
		*nread = n;
	return n > 0;
}

bool
lion_poschunk_last_code(const LionContainer *c, uint64 *code)
{
	LionPosIter it;
	bool		any = false;

	/* codes only: stepped over, not decoded (skip_member()) */
	lion_poschunk_iter_init(&it, c);
	while (it.left > 0)
	{
		uint64		mcode;

		if (skip_member(&it.p, it.end, it.code, it.first, &mcode) != POS_OK)
			break;
		*code = it.code = mcode;
		it.first = false;
		it.left--;
		any = true;
	}
	return any;
}

/*
 * Bytes appending m to c needs, or 0 when m cannot be appended: its code is
 * not above the last member's (or below the chunk's base), or the member is
 * malformed.
 */
Size
lion_poschunk_append_need(const LionContainer *c, const LionPosMember *m)
{
	uint64		last;

	if (!member_valid(m))
		return 0;
	if (pos_last(c, &last, NULL, NULL))
	{
		if (m->code <= last)
			return 0;
		return lion_posmember_size(last, m);
	}
	if (m->code < pos_base(c))
		return 0;
	return lion_posmember_size(pos_base(c), m);
}

/*
 * Append m after the last member, in a buffer of cap bytes.  This is the
 * insert hot path (the next row's TID is above every TID before it), and
 * writes nothing but the new member's bytes and the header.
 */
bool
lion_poschunk_append(LionContainer *c, Size cap, const LionPosMember *m)
{
	LionPosTail tail;

	tail.valid = false;
	return lion_poschunk_append_tail(c, cap, m, &tail);
}

/*
 * The same, for a caller that appends to one chunk many times - the build
 * fills chunk after chunk this way - and keeps where its last member ends in
 * *tail, so that the chunk is not decoded again at every call: tail->valid
 * false makes the first call find it.  A failed append leaves *tail as it
 * was.
 */
bool
lion_poschunk_append_tail(LionContainer *c, Size cap, const LionPosMember *m,
						  LionPosTail *tail)
{
	uint64		prev;
	Size		need;

	if (!member_valid(m))
		return false;
	if (!tail->valid)
	{
		const uint8 *end;
		uint32		n;
		uint64		last;

		tail->any = pos_last(c, &last, &end, &n);
		tail->last = tail->any ? last : 0;
		/*
		 * Write after the last member the iterator could read: on a damaged
		 * chunk that drops the bytes it could not, rather than burying the
		 * new member behind them.
		 */
		tail->used = (uint32) (end - pos_members_const(c));
		tail->n = n;
		tail->valid = true;
	}
	if (tail->any)
	{
		if (m->code <= tail->last)
			return false;
		prev = tail->last;
	}
	else
	{
		if (m->code < pos_base(c))
			return false;
		prev = pos_base(c);
	}

	need = lion_posmember_size(prev, m);
	if (LION_POS_HDRSZ + tail->used + need > cap ||
		tail->used + need > LION_POS_MAX_BYTES)
		return false;
	encode_member(pos_members(c) + tail->used, prev, m);
	tail->used += (uint32) need;
	tail->n++;
	tail->last = m->code;
	tail->any = true;
	pos_set_used(c, tail->used);
	c->cardinality = (uint16) tail->n;
	return true;
}

/*
 * Re-encoding.  A PosOut accumulates members in ascending order, either
 * counting their bytes (dst NULL) or writing them.
 */
typedef struct PosOut
{
	uint8	   *dst;
	uint64		prev;
	Size		bytes;
	uint32		n;
} PosOut;

static inline void
posout_init(PosOut *o, uint8 *dst, uint32 block)
{
	o->dst = dst;
	o->prev = lion_pos_block_base(block);
	o->bytes = 0;
	o->n = 0;
}

static inline void
posout_add(PosOut *o, const LionPosMember *m)
{
	Assert(o->n == 0 ? m->code >= o->prev : m->code > o->prev);
	if (o->dst)
	{
		uint8	   *e = encode_member(o->dst + o->bytes, o->prev, m);

		o->bytes = (Size) (e - o->dst);
	}
	else
		o->bytes += lion_posmember_size(o->prev, m);
	o->prev = m->code;
	o->n++;
}

typedef union PosCopy
{
	LionContainer hdr;
	char		data[LION_CONTAINER_MAX_SIZE];
} PosCopy;

static void
pos_copy(PosCopy *copy, const LionContainer *c)
{
	memcpy(copy->data, c, lion_poschunk_size(c));
}

/* Emit the members of src merged with m (replacing a member of m's code). */
static void
pos_merge(PosOut *o, const LionContainer *src, const LionPosMember *m, bool *replaced)
{
	LionPosIter it;
	LionPosMember cur;
	bool		done = false;

	if (replaced)
		*replaced = false;
	lion_poschunk_iter_init(&it, src);
	while (lion_poschunk_iter_next(&it, &cur))
	{
		if (!done && cur.code >= m->code)
		{
			posout_add(o, m);
			done = true;
			if (cur.code == m->code)
			{
				if (replaced)
					*replaced = true;
				continue;
			}
		}
		posout_add(o, &cur);
	}
	if (!done)
		posout_add(o, m);
}

/*
 * Insert m where its code belongs, replacing a member of the same code (a
 * row's positions are a function of its TID while the TID lives: a
 * same-TID member can only be an orphan of a dead tuple, DESIGN.md §17).
 * The header block drops to m's when m comes below it, and never rises, not
 * even for an empty chunk: a position tree's separators were copied from
 * headers (lion_positions_put()).  Fails, leaving c as it was, when the result
 * needs more than cap bytes.
 */
bool
lion_poschunk_insert(LionContainer *c, Size cap, const LionPosMember *m,
					 bool *replaced)
{
	PosCopy		copy;
	PosOut		o;
	uint32		block;

	if (!member_valid(m))
		return false;
	block = lion_pos_block(m->code);
	if (c->ckey < block)
		block = c->ckey;

	posout_init(&o, NULL, block);
	pos_merge(&o, c, m, NULL);
	if (o.bytes > LION_POS_MAX_BYTES || LION_POS_HDRSZ + o.bytes > cap)
		return false;

	pos_copy(&copy, c);
	posout_init(&o, pos_members(c), block);
	pos_merge(&o, &copy.hdr, m, replaced);
	c->ckey = block;
	c->cardinality = (uint16) o.n;
	pos_set_used(c, (uint32) o.bytes);
	/* a replaced member can be shorter: keep the bytes it freed zero */
	if (LION_POS_HDRSZ + o.bytes < lion_poschunk_size(&copy.hdr))
		memset((char *) c + LION_POS_HDRSZ + o.bytes, 0,
			   lion_poschunk_size(&copy.hdr) - (LION_POS_HDRSZ + o.bytes));
	return true;
}

/*
 * Remove every member cb says to, in place, and return how many it removed.
 * The chunk never grows (a kept member's code delta is the sum of the
 * deltas it replaces, and a varint of a sum is no longer than the varints of
 * its terms), the bytes it frees are zeroed so that slack stays zero, and
 * the header block is left as it was: still a lower bound.
 */
uint32
lion_poschunk_remove(LionContainer *c, LionPosRemoveCallback cb, void *arg)
{
	PosCopy		copy;
	PosOut		o;
	LionPosIter it;
	LionPosMember cur;
	uint32		removed = 0;
	Size		old = lion_poschunk_size(c);

	pos_copy(&copy, c);
	posout_init(&o, pos_members(c), c->ckey);
	lion_poschunk_iter_init(&it, &copy.hdr);
	while (lion_poschunk_iter_next(&it, &cur))
	{
		if (cb(cur.code, arg))
			removed++;
		else
			posout_add(&o, &cur);
	}
	Assert(LION_POS_HDRSZ + o.bytes <= old);
	c->cardinality = (uint16) o.n;
	pos_set_used(c, (uint32) o.bytes);
	if (LION_POS_HDRSZ + o.bytes < old)
		memset((char *) c + LION_POS_HDRSZ + o.bytes, 0,
			   old - (LION_POS_HDRSZ + o.bytes));
	return removed;
}

/*
 * Split c's members between left and right (each a buffer of
 * LION_CONTAINER_MAX_SIZE bytes, neither c) at the member boundary nearest
 * half its bytes, keeping at least one member on each side.  left keeps c's
 * block; right takes its first member's.  False when c has fewer than two
 * members.
 */
bool
lion_poschunk_split(const LionContainer *c, LionContainer *left, LionContainer *right)
{
	LionPosIter it;
	LionPosMember cur;
	PosOut		lo;
	PosOut		ro;
	uint32		half = lion_poschunk_used(c) / 2;
	uint32		n = 0;
	bool		inright = false;

	Assert(left != c && right != c);
	posout_init(&ro, NULL, 0);
	{
		uint64		last;
		uint32		nread;

		if (!pos_last(c, &last, NULL, &nread) || nread < 2)
			return false;
	}

	lion_poschunk_init(left, c->ckey);
	posout_init(&lo, pos_members(left), c->ckey);
	lion_poschunk_iter_init(&it, c);
	while (lion_poschunk_iter_next(&it, &cur))
	{
		if (!inright && n > 0 && lo.bytes + lion_posmember_size(lo.prev, &cur) / 2 > half)
		{
			inright = true;
			lion_poschunk_init(right, lion_pos_block(cur.code));
			posout_init(&ro, pos_members(right), right->ckey);
		}
		if (inright)
			posout_add(&ro, &cur);
		else
			posout_add(&lo, &cur);
		n++;
	}
	if (!inright)
	{
		/* everything landed left: move the last member over */
		LionPosIter it2;
		LionPosMember prev = {0};
		uint32		i = 0;

		lion_poschunk_init(left, c->ckey);
		posout_init(&lo, pos_members(left), c->ckey);
		lion_poschunk_iter_init(&it2, c);
		while (lion_poschunk_iter_next(&it2, &prev))
		{
			if (++i == n)
				break;
			posout_add(&lo, &prev);
		}
		lion_poschunk_init(right, lion_pos_block(prev.code));
		posout_init(&ro, pos_members(right), right->ckey);
		posout_add(&ro, &prev);
	}
	left->cardinality = (uint16) lo.n;
	pos_set_used(left, (uint32) lo.bytes);
	right->cardinality = (uint16) ro.n;
	pos_set_used(right, (uint32) ro.bytes);
	return true;
}

/*
 * Is c, an item of itemsz bytes, a well-formed chunk?  On failure, errbuf
 * says why.  Everything the readers tolerate is reported here: a byte count
 * past the largest chunk, members that the header does not count or that
 * do not fill the byte count exactly, damaged members, a first member below
 * the header block, and slack (the item's bytes past the chunk) that is not
 * zero.
 */
bool
lion_poschunk_check(const LionContainer *c, Size itemsz, char *errbuf, Size errlen)
{
	const uint8 *p;
	const uint8 *end;
	LionPosMember m;
	uint64		prev;
	uint32		n = 0;
	Size		size;
	Size		i;

#define POSFAIL(...) \
	do { if (errbuf && errlen > 0) snprintf(errbuf, errlen, __VA_ARGS__); return false; } while (0)

	if (itemsz < LION_POS_HDRSZ)
		POSFAIL("position chunk item of %zu bytes is shorter than its header", (size_t) itemsz);
	if (c->type != LION_CT_POSITIONS)
		POSFAIL("item type %u is not a position chunk", (unsigned) c->type);
	if (c->flags != 0)
		POSFAIL("position chunk flags %u are not zero", (unsigned) c->flags);
	if (pos_used_raw(c) > LION_POS_MAX_BYTES)
		POSFAIL("position chunk claims %u member bytes, more than %u",
				(unsigned) pos_used_raw(c), (unsigned) LION_POS_MAX_BYTES);
	size = lion_poschunk_size(c);
	if (itemsz > LION_CONTAINER_MAX_SIZE)
		POSFAIL("position chunk item of %zu bytes is larger than %zu",
				(size_t) itemsz, (size_t) LION_CONTAINER_MAX_SIZE);
	if (size > itemsz)
		POSFAIL("position chunk of %zu bytes overruns its %zu-byte item",
				(size_t) size, (size_t) itemsz);

	p = pos_members_const(c);
	end = p + lion_poschunk_used(c);
	prev = pos_base(c);
	while (p < end)
	{
		PosDecode	r = decode_member(&p, end, prev, n == 0, &m);

		if (r != POS_OK)
			POSFAIL("position chunk member %u: %s", n, pos_decode_msg[r]);
		prev = m.code;
		n++;
	}
	if (n != c->cardinality)
		POSFAIL("position chunk holds %u members but its header says %u",
				n, (unsigned) c->cardinality);
	for (i = size; i < itemsz; i++)
	{
		if (((const uint8 *) c)[i] != 0)
			POSFAIL("position chunk slack byte %zu is not zero", (size_t) i);
	}
	return true;
#undef POSFAIL
}
