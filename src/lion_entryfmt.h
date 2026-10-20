/*-------------------------------------------------------------------------
 * lion_entryfmt.h
 *	  The layout of a directory entry tuple (DESIGN.md §21, "Entry tuples",
 *	  format version 10).
 *
 *	  Every item on a directory page - an entry on a leaf, a high key, a
 *	  downlink - is a LionEntryTuple: a header, the key, and for an entry
 *	  that stores them a positions extension and then an INLINE payload.
 *	  The header comes in two forms, chosen by the entry's flags:
 *
 *	  SHORT (12 bytes): every INLINE entry and every pivot.
 *
 *		flags			uint16	LION_ENTRY_*
 *		keylen			uint16	bytes of key
 *		attno			uint16	the key column (§24); 0 only on the
 *								minus-infinity downlink
 *		sh_ncontainers	uint16	INLINE: items in the payload; pivot: 0
 *		sh_word			uint32	INLINE: TIDs in the payload; pivot: the
 *								child block of a downlink, InvalidBlockNumber
 *								on a high key
 *
 *	  CHAIN (28 bytes): an entry flagged LION_ENTRY_CHAIN, whose posting set
 *	  is a posting tree of its own (§22).
 *
 *		flags, keylen, attno	as above
 *		unused			uint16	zero
 *		head			BlockNumber		the posting tree's root
 *		tail			BlockNumber		its last leaf (the append hint)
 *		ntids			uint64	TIDs in the set
 *		ncontainers		uint32	items in the set
 *
 *	  An INLINE payload is at most LION_MAX_ENTRY_SIZE bytes and every item
 *	  in it is at least LION_CONTAINER_HDRSZ bytes and holds at most
 *	  LION_CONTAINER_RANGE TIDs, so its counts always fit the short form's
 *	  fields (the static assertion in lion.h); the setters below say false
 *	  when asked to store more, and the backend makes that an ERROR.
 *
 *	  THE KEY follows the header: at the header's end, or - for a key type
 *	  passed by reference whose alignment is 'd', flagged
 *	  LION_ENTRY_ALIGNKEY - at the next DOUBLEALIGN boundary, because the
 *	  opclass functions are handed a pointer into the page and read the
 *	  key's fields in place.  Every other key needs at most four-byte
 *	  alignment, which both header sizes have: a by-value key is copied out
 *	  (lion_key_store_byval()), and is stored at its type's width.
 *
 *	  THE POSITIONS EXTENSION (LION_ENTRY_POSITIONS, §17) is at the next
 *	  INTALIGN boundary after the key, and THE PAYLOAD right after it, or
 *	  right after the key: a payload is packed and read through
 *	  lion_inline_fetch(), so it needs no alignment at all.
 *
 *	  The entry has no hash: the directory order's hash term (§21) is the
 *	  key's hash, recomputed from the stored key where a comparison needs
 *	  it, and the owner stamp of a posting tree's pages (§18) is a hash of
 *	  the stored key's bytes (lion_entry_owner()).
 *
 *	  Only c.h here, so that test/unit/entry_test.c can build and read
 *	  entries without a server.
 *-------------------------------------------------------------------------
 */
#ifndef LION_ENTRYFMT_H
#define LION_ENTRYFMT_H

#include "c.h"
#include "storage/block.h"

/* ---------- entry flags ---------- */

#define LION_ENTRY_INLINE	0x0001
#define LION_ENTRY_CHAIN		0x0002
#define LION_ENTRY_NULLKEY	0x0004	/* the reserved NULL-key entry, DESIGN.md §14 */
#define LION_ENTRY_EMPTYKEY	0x0008	/* the reserved no-key entry, DESIGN.md §17 */

/*
 * Pivot tuples (DESIGN.md §21).  A directory page's first item is its HIGH
 * KEY when the page is not the rightmost of its level, and every item of an
 * internal page is a DOWNLINK whose child block is in the short header's
 * sh_word (lion_entry_head()).  Both are LionEntryTuple-shaped so that the
 * key helpers apply unchanged; they carry neither INLINE nor CHAIN, so they
 * have the short header, and no counts.  A downlink flagged MINUSINF is the
 * leftmost child of its page and compares below every key.
 */
#define LION_ENTRY_HIGHKEY	0x0010
#define LION_ENTRY_DOWNLINK	0x0020
#define LION_ENTRY_MINUSINF	0x0040

/*
 * SUMMARY entries (DESIGN.md §32, format version 7).  A summary entry's posting
 * set is the union of the posting sets of a run of its column's VALUE entries
 * - a BUCKET of keys - and its key is the largest key the bucket may hold: the
 * buckets of a column are (previous summary's key, this summary's key], in key
 * order, and sort after every VALUE entry of the column.  The last bucket of a
 * column is open above; its entry carries LION_ENTRY_SUMLAST as well, sorts
 * after every other summary of the column WHATEVER ITS KEY, and its key is the
 * largest key it holds so far, which an insert of a larger key raises.  So its
 * key is data, not position, and the order compares no key of it at all.
 */
#define LION_ENTRY_SUMMARY	0x0080
#define LION_ENTRY_SUMLAST	0x0100

/*
 * An entry that stores word positions (DESIGN.md §17, "Stored positions"):
 * a LionEntryPosExt follows its key, before its payload.  Never on a pivot,
 * which is made of the header and key alone (lion_pivot_kindflags() does not
 * keep it).
 */
#define LION_ENTRY_POSITIONS	0x0200

/*
 * The key is at the DOUBLEALIGN boundary after the header rather than at
 * its end: a by-reference key type whose alignment is 'd' (see above).  A
 * pivot copies it from the item it is made of, as it copies the key.
 */
#define LION_ENTRY_ALIGNKEY	0x0400

/*
 * The third reserved entry of a multi-key column (DESIGN.md §17): the rows
 * whose value the opclass extracted keys from, every one of them NULL - an
 * array of NULLs, `{NULL}` or `{{NULL},{NULL}}` - where the EMPTY entry holds
 * the values it extracted nothing from at all, an array with no elements.
 */
#define LION_ENTRY_NULLELEMS	0x0800

#define LION_ENTRY_RESERVED	(LION_ENTRY_NULLKEY | LION_ENTRY_EMPTYKEY | \
							 LION_ENTRY_NULLELEMS)
#define LION_ENTRY_SUMKINDS	(LION_ENTRY_SUMMARY | LION_ENTRY_SUMLAST)
/*
 * The flags that make an entry's KIND, which a pivot copied from it keeps -
 * all but SUMLAST (lion_pivot_kindflags()).
 */
#define LION_ENTRY_KINDFLAGS	(LION_ENTRY_RESERVED | LION_ENTRY_SUMKINDS)
#define LION_ENTRY_PIVOT		(LION_ENTRY_HIGHKEY | LION_ENTRY_DOWNLINK)
#define LION_ENTRY_ALLFLAGS \
	(LION_ENTRY_INLINE | LION_ENTRY_CHAIN | LION_ENTRY_RESERVED | \
	 LION_ENTRY_PIVOT | LION_ENTRY_MINUSINF | LION_ENTRY_SUMKINDS | \
	 LION_ENTRY_POSITIONS | LION_ENTRY_ALIGNKEY)

/* ---------- the two header forms ---------- */

typedef struct LionEntryTuple
{
	uint16		flags;			/* LION_ENTRY_* */
	uint16		keylen;			/* bytes of key data (0 for a reserved entry) */
	uint16		attno;			/* 1-based KEY COLUMN (DESIGN.md §24); 0 only
								 * on a minus-infinity downlink */
	uint16		sh_ncontainers; /* SHORT form only: see above */
	uint32		sh_word;		/* SHORT form only: see above */
	/* key, positions extension, payload: see above */
} LionEntryTuple;

typedef struct LionEntryChainHdr
{
	uint16		flags;
	uint16		keylen;
	uint16		attno;
	uint16		unused;
	BlockNumber head;			/* the posting tree's root */
	BlockNumber tail;			/* its last leaf */
	uint64		ntids;
	uint32		ncontainers;
	/* the struct's own size pads this out; the header ends here */
} LionEntryChainHdr;

#define LION_ENTRY_SHORT_HDRSZ		12
#define LION_ENTRY_CHAIN_HDRSZ		28
/* The largest offset a key can start at: a CHAIN header, double-aligned. */
#define LION_ENTRY_MAX_KEYOFF		DOUBLEALIGN(LION_ENTRY_CHAIN_HDRSZ)

StaticAssertDecl(sizeof(LionEntryTuple) == LION_ENTRY_SHORT_HDRSZ,
				 "the short lion entry header is 12 bytes");
StaticAssertDecl(offsetof(LionEntryChainHdr, ncontainers) + sizeof(uint32) ==
				 LION_ENTRY_CHAIN_HDRSZ,
				 "the CHAIN lion entry header is 28 bytes");
StaticAssertDecl(offsetof(LionEntryChainHdr, attno) ==
				 offsetof(LionEntryTuple, attno) &&
				 offsetof(LionEntryChainHdr, keylen) ==
				 offsetof(LionEntryTuple, keylen) &&
				 offsetof(LionEntryChainHdr, flags) ==
				 offsetof(LionEntryTuple, flags),
				 "both lion entry headers start with flags, keylen and attno");
StaticAssertDecl(offsetof(LionEntryChainHdr, ntids) % 8 == 0,
				 "a CHAIN entry's ntids is aligned in a MAXALIGNed item");

/*
 * The positions extension of an entry flagged LION_ENTRY_POSITIONS, right
 * after its key at an INTALIGN offset (DESIGN.md §17, "Stored positions"):
 *
 *	- on a CHAIN entry pos_root is the root of the key's position tree
 *	  (lion_postree.c), or InvalidBlockNumber while it has none, and pos_len
 *	  is 0;
 *	- on an INLINE entry pos_root is InvalidBlockNumber and pos_len is the
 *	  size of the positions section: the last pos_len bytes of the item, a
 *	  run of position chunks after the container items and their slack.
 *
 * Only entries that store positions carry it: every other entry is laid out
 * without it.
 */
typedef struct LionEntryPosExt
{
	BlockNumber pos_root;
	uint32		pos_len;
} LionEntryPosExt;

#define LION_ENTRY_POSEXT_SIZE	sizeof(LionEntryPosExt)	/* 8 */

/* ---------- where things are ---------- */

/* The header size of an entry with these flags. */
static inline Size
lion_entry_hdrsz_for(uint16 flags)
{
	return (flags & LION_ENTRY_CHAIN) != 0 ?
		LION_ENTRY_CHAIN_HDRSZ : LION_ENTRY_SHORT_HDRSZ;
}

/* Where the key of an entry with these flags starts. */
static inline Size
lion_entry_keyoff_for(uint16 flags)
{
	Size		hdr = lion_entry_hdrsz_for(flags);

	return (flags & LION_ENTRY_ALIGNKEY) != 0 ? DOUBLEALIGN(hdr) : hdr;
}

/* Where the positions extension of such an entry with this key starts. */
static inline Size
lion_entry_extoff_for(uint16 flags, Size keylen)
{
	return INTALIGN(lion_entry_keyoff_for(flags) + keylen);
}

/* Where the payload of such an entry starts: everything before it. */
static inline Size
lion_entry_payoff_for(uint16 flags, Size keylen)
{
	if ((flags & LION_ENTRY_POSITIONS) != 0)
		return lion_entry_extoff_for(flags, keylen) + LION_ENTRY_POSEXT_SIZE;
	return lion_entry_keyoff_for(flags) + keylen;
}

#define LionEntryHdrSize(e)			lion_entry_hdrsz_for((e)->flags)
#define LionEntryKeyOffset(e)		lion_entry_keyoff_for((e)->flags)
#define LionEntryGetKey(e)			((char *) (e) + LionEntryKeyOffset(e))
#define LionEntryExtOffset(e)		lion_entry_extoff_for((e)->flags, (e)->keylen)
#define LionEntryPayloadOffset(e)	lion_entry_payoff_for((e)->flags, (e)->keylen)
#define LionEntryGetPayload(e)		((char *) (e) + LionEntryPayloadOffset(e))
/* payload length must be computed from the item size: LION_ENTRY_PAYLOAD_LEN() */

static inline bool
lion_entry_is_chain(const LionEntryTuple *e)
{
	return (e->flags & LION_ENTRY_CHAIN) != 0;
}

#define LionEntryChainHeader(e)		((LionEntryChainHdr *) (e))

/* ---------- the counts and links ---------- */

/* Members of the posting set; 0 on a pivot. */
static inline uint64
lion_entry_ntids(const LionEntryTuple *e)
{
	if (lion_entry_is_chain(e))
		return ((const LionEntryChainHdr *) e)->ntids;
	if ((e->flags & LION_ENTRY_PIVOT) != 0)
		return 0;
	return e->sh_word;
}

/* Items of the posting set; 0 on a pivot. */
static inline uint32
lion_entry_ncontainers(const LionEntryTuple *e)
{
	if (lion_entry_is_chain(e))
		return ((const LionEntryChainHdr *) e)->ncontainers;
	return e->sh_ncontainers;
}

/*
 * A CHAIN entry's posting tree root, a downlink's child block; on anything
 * else InvalidBlockNumber - which a high key stores, and an INLINE entry
 * has no field for.
 */
static inline BlockNumber
lion_entry_head(const LionEntryTuple *e)
{
	if (lion_entry_is_chain(e))
		return ((const LionEntryChainHdr *) e)->head;
	if ((e->flags & LION_ENTRY_PIVOT) != 0)
		return (BlockNumber) e->sh_word;
	return InvalidBlockNumber;
}

/* A CHAIN entry's last posting leaf; InvalidBlockNumber on anything else. */
static inline BlockNumber
lion_entry_tail(const LionEntryTuple *e)
{
	if (lion_entry_is_chain(e))
		return ((const LionEntryChainHdr *) e)->tail;
	return InvalidBlockNumber;
}

/*
 * The setters.  The short form's counts are narrower than a CHAIN's: these
 * say false, and store nothing, when n does not fit the entry's form.
 */
static inline bool
lion_entry_try_set_ntids(LionEntryTuple *e, uint64 n)
{
	if (lion_entry_is_chain(e))
	{
		((LionEntryChainHdr *) e)->ntids = n;
		return true;
	}
	if ((e->flags & LION_ENTRY_PIVOT) != 0 || n > PG_UINT32_MAX)
		return n == 0;
	e->sh_word = (uint32) n;
	return true;
}

static inline bool
lion_entry_try_set_ncontainers(LionEntryTuple *e, uint32 n)
{
	if (lion_entry_is_chain(e))
	{
		((LionEntryChainHdr *) e)->ncontainers = n;
		return true;
	}
	if (n > PG_UINT16_MAX)
		return false;
	e->sh_ncontainers = (uint16) n;
	return true;
}

/* Only a CHAIN entry has a head and a tail, and only a pivot a child. */
static inline void
lion_entry_set_head(LionEntryTuple *e, BlockNumber blk)
{
	if (lion_entry_is_chain(e))
		((LionEntryChainHdr *) e)->head = blk;
	else
	{
		Assert((e->flags & LION_ENTRY_PIVOT) != 0);
		e->sh_word = (uint32) blk;
	}
}

static inline void
lion_entry_set_tail(LionEntryTuple *e, BlockNumber blk)
{
	Assert(lion_entry_is_chain(e));
	((LionEntryChainHdr *) e)->tail = blk;
}

/* ---------- making one ---------- */

/*
 * Lay out the header of a new entry at e, which has at least
 * lion_entry_payoff_for(flags, keylen) bytes and is MAXALIGNed: flags,
 * key length and column, and the empty values of everything else - no
 * items, no TIDs, no posting tree or child, and a positions extension, when
 * the flags ask for one, with no tree and no inline positions.  Bytes the
 * layout skips are zeroed; the key is the caller's to copy to
 * LionEntryGetKey(e), and the payload to LionEntryGetPayload(e).
 */
static inline void
lion_entry_init(LionEntryTuple *e, uint16 flags, uint16 attno, uint16 keylen)
{
	memset(e, 0, lion_entry_payoff_for(flags, keylen));
	e->flags = flags;
	e->keylen = keylen;
	e->attno = attno;
	if ((flags & LION_ENTRY_CHAIN) != 0)
	{
		LionEntryChainHdr *c = (LionEntryChainHdr *) e;

		c->head = InvalidBlockNumber;
		c->tail = InvalidBlockNumber;
	}
	else if ((flags & LION_ENTRY_PIVOT) != 0)
		e->sh_word = (uint32) InvalidBlockNumber;
	if ((flags & LION_ENTRY_POSITIONS) != 0)
	{
		LionEntryPosExt *x = (LionEntryPosExt *)
			((char *) e + lion_entry_extoff_for(flags, keylen));

		x->pos_root = InvalidBlockNumber;
		x->pos_len = 0;
	}
}

/*
 * Copy src's column and key, and its positions extension when both it and
 * newflags have one, into dst laid out for newflags - another header form,
 * when INLINE and CHAIN trade places - and return the payload offset.  dst
 * has lion_entry_payoff_for(newflags, src->keylen) bytes; its counts and
 * links are the empty ones of lion_entry_init(), the caller's to fill in,
 * and so is the extension when only newflags has one.  newflags must say
 * LION_ENTRY_ALIGNKEY exactly as src does: it is where the key type needs
 * its key.
 */
static inline Size
lion_entry_copy_reshaped(LionEntryTuple *dst, const LionEntryTuple *src,
						 uint16 newflags)
{
	Assert(((newflags ^ src->flags) & LION_ENTRY_ALIGNKEY) == 0);
	lion_entry_init(dst, newflags, src->attno, src->keylen);
	if (src->keylen > 0)
		memcpy(LionEntryGetKey(dst), LionEntryGetKey(src), src->keylen);
	if ((newflags & src->flags & LION_ENTRY_POSITIONS) != 0)
		memcpy((char *) dst + LionEntryExtOffset(dst),
			   (const char *) src + LionEntryExtOffset(src),
			   LION_ENTRY_POSEXT_SIZE);
	return LionEntryPayloadOffset(dst);
}

/* ---------- by-value keys at their width ---------- */

/*
 * A key of a type passed by value is stored as its typlen bytes - 1, 2, 4
 * or 8 - not as a whole Datum: the low typlen bytes of the Datum, which is
 * what store_att_byval() writes, copied rather than stored through a cast
 * because the key need not be aligned.  The value is the Datum as a uint64
 * here, so that this file needs no more than c.h; it reads back extended
 * as fetch_att() gives it: the Datum of a char, an int16 or an int32.
 */
static inline void
lion_key_store_byval(char *dest, uint64 value, int typlen)
{
	switch (typlen)
	{
		case 1:
			{
				char		v = (char) value;

				memcpy(dest, &v, 1);
				break;
			}
		case 2:
			{
				int16		v = (int16) value;

				memcpy(dest, &v, 2);
				break;
			}
		case 4:
			{
				int32		v = (int32) value;

				memcpy(dest, &v, 4);
				break;
			}
		default:
			Assert(typlen == 8);
			memcpy(dest, &value, 8);
			break;
	}
}

static inline uint64
lion_key_fetch_byval(const char *src, int typlen)
{
	switch (typlen)
	{
		case 1:
			{
				char		v;

				memcpy(&v, src, 1);
				return (uint64) v;	/* as CharGetDatum() converts it */
			}
		case 2:
			{
				int16		v;

				memcpy(&v, src, 2);
				return (uint64) (int64) v;
			}
		case 4:
			{
				int32		v;

				memcpy(&v, src, 4);
				return (uint64) (int64) v;
			}
		default:
			{
				uint64		v;

				Assert(typlen == 8);
				memcpy(&v, src, 8);
				return v;
			}
	}
}

#endif							/* LION_ENTRYFMT_H */
