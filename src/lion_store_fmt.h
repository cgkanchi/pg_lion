/*-------------------------------------------------------------------------
 *
 * lion_store_fmt.h
 *	  The bytes of a window store page (DESIGN.md §40): its header, its
 *	  dictionary and its per-heap-page sub-arrays, and the arithmetic that
 *	  reads and writes them.
 *
 * Everything here is plain C over byte arrays, with no server dependency,
 * so that test/unit/store_test.c can exercise the packing on its own; the
 * pages themselves, the map and every caller are in lion_store.h and
 * lion_store.c.
 *
 * A store page holds, as items in this order:
 *
 *	1	LionStoreHeader
 *	2	the dictionary: LionStoreDict, then the values.  A fixed-width column
 *		packs them at typlen bytes each; a varlena column writes their bytes
 *		back to back (no varlena header) and then ndict + 1 uint16 offsets,
 *		off[0] = 0 and off[i + 1] the end of entry i.  The item is there in
 *		RAW mode too, empty, so that item 3 + k is always heap page lo + k.
 *	3 + k	the sub-array of heap page lo + k: LionStoreSub, then
 *		  DICT:	nslots codes of `width` bits, packed least significant
 *				bit first (16-bit codes low byte first); code 0 is NULL,
 *				code c names dictionary entry c - 1
 *		  RAW:	nslots slots of `width` bytes, then a null bitmap of nslots
 *				bits (bit set = NULL).  A fixed-width slot is the value's
 *				typlen bytes; a capped varlena slot is a uint16 length and
 *				the bytes, zero-padded to the width.
 *		  ABSENT: nothing at all (nslots is 0)
 *
 * Slot i of a sub-array is heap offset i + 1.  Nothing on a store page is
 * aligned beyond the item itself, so every multi-byte read is a memcpy.
 *
 * KEY-ORDERED WINDOWS (DESIGN.md §41) use the same page with two meanings
 * added, both said by the header's flags.  A VIRTUAL page is a data page of
 * an ordered index: its "heap pages" are the window's virtual pages, 0 ..
 * LION_STORE_MAX_VPAGES - 1, each one BUCKET of rows whose order values fall
 * in one range (the window header's bucket table says which).  A PERM page
 * holds the window's permutation: a two-byte RAW column in heap coordinates
 * whose slots are the virtual lo of each row's bucket slot; the dictionary
 * item of the chain's head is the window header (LionStoreWinHdr, its
 * buckets, its directory and its fence values) instead of an empty
 * dictionary.
 *
 *-------------------------------------------------------------------------
 */
#ifndef LION_STORE_FMT_H
#define LION_STORE_FMT_H

#include "c.h"

#include "lion_tid.h"

/* LionStoreHeader.mode */
#define LION_STORE_DICT			1
#define LION_STORE_RAW			2

/*
 * Item 1 of a store page.  ckey and ord repeat what the special area says
 * (owner_head and owner_hash), so that a page is self-describing to verify()
 * and to a reader holding only its block; lo and hi are the heap pages of the
 * window it covers, inclusive, 0 .. LION_BLOCKS_PER_CONTAINER - 1.
 */
typedef struct LionStoreHeader
{
	uint32		ckey;			/* the window */
	uint16		ord;			/* stored column ordinal */
	uint8		lo;				/* first heap page of the window covered */
	uint8		hi;				/* last heap page covered */
	uint8		mode;			/* LION_STORE_DICT or LION_STORE_RAW */
	uint8		flags;			/* LION_STORE_F_*; zero in heap order */
	uint16		width;			/* DICT: bits per code; RAW: bytes per slot */
	uint16		ndict;			/* DICT: dictionary entries; RAW: 0 */
	int16		typlen;			/* the column's typlen, -1 for varlena */
} LionStoreHeader;

StaticAssertDecl(sizeof(LionStoreHeader) == 16,
				 "the lion store page header must not change size");

/* LionStoreHeader.flags (DESIGN.md §41) */
#define LION_STORE_F_VIRTUAL	0x01	/* a data page over virtual pages */
#define LION_STORE_F_PERM		0x02	/* a page of the permutation */
#define LION_STORE_F_KINDS		0x03

/*
 * The virtual pages of a window of an ordered index: each a bucket, or
 * unused.  A virtual lo is vpage << LION_OFFSET_BITS | offset, as a
 * container's lo is for a heap page, and so fits 16 bits.
 */
#define LION_STORE_MAX_VPAGES	(2 * LION_BLOCKS_PER_CONTAINER)

StaticAssertDecl(LION_CONTAINER_BITS + 1 <= 16,
				 "a virtual lo must fit 16 bits");

static inline uint16
lion_store_vlo(int vpage, int off)
{
	return (uint16) ((vpage << LION_OFFSET_BITS) | off);
}

static inline int
lion_store_vlo_page(uint16 vlo)
{
	return vlo >> LION_OFFSET_BITS;
}

static inline int
lion_store_vlo_off(uint16 vlo)
{
	return vlo & ((1 << LION_OFFSET_BITS) - 1);
}

/*
 * The window header (DESIGN.md §41): the dictionary item of a permutation
 * chain's head, LionStoreDict (ndict 0, nbytes the rest) and then this,
 * nbucket LionStoreBucket in the order of their ranges, ndir directory
 * entries ascending by (ord, lo), and fencebytes of fence values.
 *
 * The generation advances whenever a slot that a permutation entry named
 * may be written for another row: a reader that read an entry under one
 * generation and finds another after its data walk reads the window again.
 */
typedef struct LionStoreWinHdr
{
	uint32		gen;			/* the window's generation */
	uint16		nbucket;		/* buckets that follow, 1 .. MAX_VPAGES */
	uint16		ndir;			/* directory entries after them */
	uint16		fencebytes;		/* bytes of fence values after those */
	uint16		flags;			/* LION_STORE_WH_THIN, or zero */
} LionStoreWinHdr;

/* The directory was thinned to fit: it names some of the pages, not all. */
#define LION_STORE_WH_THIN		0x0001

/*
 * A bucket: a virtual page holding the rows whose (order value, heap lo) is
 * at or above its fence and below the next bucket's.  Slots 1 .. used have
 * been handed out since the bucket was made; the rest are free.  The first
 * bucket has no fence (LOW, below everything); a bucket whose fence value
 * was too long to keep has the fence of the bucket before it (SAME).
 */
typedef struct LionStoreBucket
{
	uint8		vpage;			/* its virtual page */
	uint8		flags;			/* LION_STORE_BK_* */
	uint16		used;			/* slots handed out */
	uint16		lo;				/* the fence's heap lo */
	uint16		fenceoff;		/* its value's bytes in the fence area */
	uint16		fencelen;
} LionStoreBucket;

#define LION_STORE_BK_LOW		0x01	/* no fence: the first bucket */
#define LION_STORE_BK_NULL		0x02	/* the fence value is NULL */
#define LION_STORE_BK_SAME		0x04	/* the fence of the bucket before */
#define LION_STORE_BK_FLAGS		0x07

typedef struct LionStoreDirEnt
{
	uint16		ord;			/* stored column ordinal */
	uint16		lo;				/* first virtual page of the page's range */
	uint32		blk;			/* its block */
} LionStoreDirEnt;

StaticAssertDecl(sizeof(LionStoreWinHdr) == 12 && sizeof(LionStoreBucket) == 10 &&
				 sizeof(LionStoreDirEnt) == 8,
				 "the lion window header must not change size");

/* The bytes of a window header of nbucket buckets, ndir entries, fencebytes. */
static inline Size
lion_store_winhdr_len(uint32 nbucket, uint32 ndir, uint32 fencebytes)
{
	return sizeof(LionStoreWinHdr) + (Size) nbucket * sizeof(LionStoreBucket) +
		(Size) ndir * sizeof(LionStoreDirEnt) + fencebytes;
}

/*
 * Is the window header of len bytes at item well formed for buckets of at
 * most maxslots slots?  NULL if so, else what is wrong (a static string).
 * Distinct virtual pages, the first bucket LOW and no other, the others' fence
 * bytes inside the fence area, the directory ascending by (ord, lo).
 */
static inline const char *
lion_store_winhdr_check(const char *item, Size len, uint32 maxslots)
{
	LionStoreWinHdr wh;
	uint8		seen[LION_STORE_MAX_VPAGES / 8];
	const char *bk;
	const char *dir;
	uint32		i;

	if (len < sizeof(LionStoreWinHdr))
		return "is shorter than its header";
	memcpy(&wh, item, sizeof(LionStoreWinHdr));
	if (wh.nbucket < 1 || wh.nbucket > LION_STORE_MAX_VPAGES ||
		(wh.flags & ~LION_STORE_WH_THIN) != 0 ||
		len != lion_store_winhdr_len(wh.nbucket, wh.ndir, wh.fencebytes))
		return "has counts that do not add up to its length";
	memset(seen, 0, sizeof(seen));
	bk = item + sizeof(LionStoreWinHdr);
	for (i = 0; i < wh.nbucket; i++)
	{
		LionStoreBucket b;

		memcpy(&b, bk + (Size) i * sizeof(LionStoreBucket), sizeof(LionStoreBucket));
		if (b.vpage >= LION_STORE_MAX_VPAGES || (b.flags & ~LION_STORE_BK_FLAGS) != 0 ||
			b.used > maxslots)
			return "has a bad bucket";
		if ((seen[b.vpage / 8] & (1 << (b.vpage % 8))) != 0)
			return "names a virtual page for two buckets";
		seen[b.vpage / 8] |= (uint8) (1 << (b.vpage % 8));
		if (((b.flags & LION_STORE_BK_LOW) != 0) != (i == 0))
			return "has a bucket below the first";
		if ((b.flags & (LION_STORE_BK_LOW | LION_STORE_BK_NULL | LION_STORE_BK_SAME)) != 0)
		{
			if (b.fencelen != 0)
				return "has fence bytes for a bucket without a fence value";
		}
		else if ((uint32) b.fenceoff + b.fencelen > wh.fencebytes)
			return "has a fence past its fence area";
	}
	dir = bk + (Size) wh.nbucket * sizeof(LionStoreBucket);
	for (i = 1; i < wh.ndir; i++)
	{
		LionStoreDirEnt a;
		LionStoreDirEnt b;

		memcpy(&a, dir + (Size) (i - 1) * sizeof(LionStoreDirEnt), sizeof(LionStoreDirEnt));
		memcpy(&b, dir + (Size) i * sizeof(LionStoreDirEnt), sizeof(LionStoreDirEnt));
		if (b.ord < a.ord || (b.ord == a.ord && b.lo <= a.lo))
			return "has a directory out of order";
	}
	return NULL;
}

/* Item 2: the dictionary's own header, so that the item is never empty. */
typedef struct LionStoreDict
{
	uint16		ndict;			/* entries; LionStoreHeader.ndict again */
	uint16		nbytes;			/* bytes of the values that follow */
} LionStoreDict;

/* Items 3 ..: a heap page's sub-array header. */
typedef struct LionStoreSub
{
	uint16		nslots;			/* the highest heap offset written */
	uint16		flags;			/* LION_STORE_ABSENT */
} LionStoreSub;

#define LION_STORE_ABSENT		0x0001	/* the page could not hold its values */

#define LION_STORE_HDR_OFF		((OffsetNumber) 1)
#define LION_STORE_DICT_OFF		((OffsetNumber) 2)
#define LION_STORE_SUB_FIRST	((OffsetNumber) 3)

/* The most dictionary entries a DICT page can name (16-bit codes, 0 = NULL). */
#define LION_STORE_MAX_DICT		65535

/* The slots a sub-array can have: one per heap offset an index TID can carry. */
#define LION_STORE_MAX_SLOTS	((1 << LION_OFFSET_BITS) - 1)

/*
 * The code width a dictionary of ndict entries needs: the smallest of 1, 2,
 * 4, 8 and 16 bits whose largest code is at least ndict.  Codes never straddle
 * a byte below 16 bits, which is what keeps them a shift and a mask.  0 when
 * no width can hold it.
 */
static inline int
lion_store_dict_width(uint32 ndict)
{
	if (ndict <= 1)
		return 1;
	if (ndict <= 3)
		return 2;
	if (ndict <= 15)
		return 4;
	if (ndict <= 255)
		return 8;
	if (ndict <= LION_STORE_MAX_DICT)
		return 16;
	return 0;
}

static inline bool
lion_store_width_valid(int width)
{
	return width == 1 || width == 2 || width == 4 || width == 8 || width == 16;
}

static inline uint32
lion_store_width_max(int width)
{
	return (((uint32) 1) << width) - 1;
}

/* Bytes of nslots packed codes of `width` bits. */
static inline Size
lion_store_codes_bytes(int width, uint32 nslots)
{
	return ((Size) nslots * (Size) width + 7) / 8;
}

/* Bytes of the null bitmap of nslots RAW slots. */
static inline Size
lion_store_bitmap_bytes(uint32 nslots)
{
	return ((Size) nslots + 7) / 8;
}

/* The length of a sub-array item. */
static inline Size
lion_store_sub_len(int mode, int width, uint32 nslots, bool absent)
{
	if (absent)
		return sizeof(LionStoreSub);
	if (mode == LION_STORE_DICT)
		return sizeof(LionStoreSub) + lion_store_codes_bytes(width, nslots);
	return sizeof(LionStoreSub) + (Size) nslots * (Size) width +
		lion_store_bitmap_bytes(nslots);
}

/*
 * The length of a dictionary item: ndict values of typlen bytes, or for a
 * varlena column (typlen -1) nbytes of values and ndict + 1 offsets.
 */
static inline Size
lion_store_dict_len(int typlen, uint32 ndict, Size nbytes)
{
	if (typlen > 0)
		return sizeof(LionStoreDict) + (Size) ndict * (Size) typlen;
	return sizeof(LionStoreDict) + nbytes + sizeof(uint16) * ((Size) ndict + 1);
}

static inline uint32
lion_store_code_get(const uint8 *codes, int width, uint32 i)
{
	if (width == 16)
		return (uint32) codes[2 * i] | ((uint32) codes[2 * i + 1] << 8);
	else
	{
		Size		bit = (Size) i * (Size) width;

		return (codes[bit / 8] >> (bit % 8)) & lion_store_width_max(width);
	}
}

static inline void
lion_store_code_set(uint8 *codes, int width, uint32 i, uint32 code)
{
	if (width == 16)
	{
		codes[2 * i] = (uint8) (code & 0xFF);
		codes[2 * i + 1] = (uint8) (code >> 8);
	}
	else
	{
		Size		bit = (Size) i * (Size) width;
		uint8		mask = (uint8) (lion_store_width_max(width) << (bit % 8));

		codes[bit / 8] = (uint8) ((codes[bit / 8] & ~mask) |
								  ((code << (bit % 8)) & mask));
	}
}

/* The byte a code lives in (the first of two at 16 bits). */
static inline Size
lion_store_code_byte(int width, uint32 i)
{
	return ((Size) i * (Size) width) / 8;
}

static inline bool
lion_store_null_get(const uint8 *bitmap, uint32 i)
{
	return (bitmap[i / 8] & (1 << (i % 8))) != 0;
}

static inline void
lion_store_null_set(uint8 *bitmap, uint32 i, bool isnull)
{
	if (isnull)
		bitmap[i / 8] |= (uint8) (1 << (i % 8));
	else
		bitmap[i / 8] &= (uint8) ~(1 << (i % 8));
}

/*
 * Offset k of a varlena dictionary item of `itemlen` bytes holding ndict
 * entries: the array is the last 2 * (ndict + 1) bytes of the item.
 */
static inline uint32
lion_store_vdict_off(const char *item, Size itemlen, uint32 ndict, uint32 k)
{
	uint16		v;

	memcpy(&v, item + itemlen - sizeof(uint16) * ((Size) ndict + 1) +
		   sizeof(uint16) * (Size) k, sizeof(uint16));
	return v;
}

/*
 * Entry i (0-based) of a varlena dictionary item: its bytes and length.  The
 * caller has checked the item (lion_store_vdict_check()).
 */
static inline const char *
lion_store_vdict_entry(const char *item, Size itemlen, uint32 ndict, uint32 i,
					   uint32 *len)
{
	uint32		start = lion_store_vdict_off(item, itemlen, ndict, i);
	uint32		end = lion_store_vdict_off(item, itemlen, ndict, i + 1);

	*len = end - start;
	return item + sizeof(LionStoreDict) + start;
}

/*
 * Is a varlena dictionary item of `itemlen` bytes well formed: as long as its
 * header says, offsets starting at 0, ascending, and ending at nbytes?
 */
static inline bool
lion_store_vdict_check(const char *item, Size itemlen)
{
	LionStoreDict d;
	uint32		k;
	uint32		prev = 0;

	if (itemlen < sizeof(LionStoreDict))
		return false;
	memcpy(&d, item, sizeof(LionStoreDict));
	if (itemlen != lion_store_dict_len(-1, d.ndict, d.nbytes))
		return false;
	for (k = 0; k <= d.ndict; k++)
	{
		uint32		o = lion_store_vdict_off(item, itemlen, d.ndict, k);

		if ((k == 0 && o != 0) || o < prev || o > d.nbytes)
			return false;
		prev = o;
	}
	return prev == d.nbytes;
}

/*
 * The rule that turns a DICT page RAW (DESIGN.md §40, "DICT or RAW"): when
 * the dictionary and the codes take more room than the values written out,
 * `rows` slots of `rawwidth` bytes.  dictbytes is what the dictionary
 * actually takes (values, and a varlena column's offsets).
 */
static inline bool
lion_store_prefer_raw(Size dictbytes, Size rows, int width, int rawwidth)
{
	if (rawwidth <= 0)
		return false;
	return dictbytes + (rows * (Size) width + 7) / 8 > rows * (Size) rawwidth;
}

#endif							/* LION_STORE_FMT_H */
