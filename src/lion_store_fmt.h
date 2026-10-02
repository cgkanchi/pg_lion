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
	uint8		flags;			/* none defined; zero */
	uint16		width;			/* DICT: bits per code; RAW: bytes per slot */
	uint16		ndict;			/* DICT: dictionary entries; RAW: 0 */
	int16		typlen;			/* the column's typlen, -1 for varlena */
} LionStoreHeader;

StaticAssertDecl(sizeof(LionStoreHeader) == 16,
				 "the lion store page header must not change size");

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
