/*-------------------------------------------------------------------------
 * store_test.c
 *	  Standalone unit tests for src/lion_store_fmt.h, the bytes of a window
 *	  store page (DESIGN.md §40).
 *
 *	  Built by "make unit PG_CONFIG=..." with -DFRONTEND; the header is plain
 *	  C over byte arrays, so this program links against nothing but libc and
 *	  libpgport.
 *
 *	  What it checks:
 *
 *	  - the code width a dictionary of n entries gets, at every boundary;
 *	  - the sizes of codes, null bitmaps, sub-arrays and dictionaries;
 *	  - DICT code packing at every width: each code read back as written,
 *		least significant bit first, no write disturbing a neighbour, and
 *		every byte past the codes left alone - against a reference array;
 *	  - the RAW null bitmap likewise;
 *	  - varlena dictionaries: built entry by entry (as an in-place append
 *		grows one), every entry read back, and lion_store_vdict_check()
 *		accepting each well-formed item and refusing every damaged one;
 *	  - the DICT-or-RAW rule.
 *
 *	  Exit status is 0 only if every check passed.
 *-------------------------------------------------------------------------
 */
#include "c.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lion_store_fmt.h"

/* ----------------------------------------------------------------
 *							bookkeeping
 * ----------------------------------------------------------------
 */

static const char *cur_phase = "startup";
static long nchecks = 0;
static long nfail = 0;

static void
check_impl(bool ok, int line, const char *msg)
{
	nchecks++;
	if (!ok)
	{
		nfail++;
		if (nfail <= 40)
			printf("FAIL [%s] store_test.c:%d: %s\n", cur_phase, line, msg);
		else if (nfail == 41)
			printf("... further failures suppressed\n");
	}
}

#define CHECK(ok, msg)	check_impl((ok), __LINE__, (msg))

static void
phase(const char *name)
{
	cur_phase = name;
	printf("  %s\n", name);
}

/* fixed-seed PRNG (xorshift64*) */
static uint64 rng_state = 1;

static void
rng_seed(uint64 s)
{
	rng_state = s ? s : UINT64CONST(0x9E3779B97F4A7C15);
}

static uint32
rng_next(void)
{
	uint64		x = rng_state;

	x ^= x >> 12;
	x ^= x << 25;
	x ^= x >> 27;
	rng_state = x;
	return (uint32) ((x * UINT64CONST(0x2545F4914F6CDD1D)) >> 32);
}

static const int widths[] = {1, 2, 4, 8, 16};

/* ----------------------------------------------------------------
 *							widths and sizes
 * ----------------------------------------------------------------
 */

static void
test_widths(void)
{
	uint32		n;
	int			w;
	bool		ok;

	phase("code widths");
	CHECK(lion_store_dict_width(0) == 1, "an empty dictionary has 1-bit codes");
	CHECK(lion_store_dict_width(1) == 1, "1 entry: 1 bit");
	CHECK(lion_store_dict_width(2) == 2, "2 entries: 2 bits");
	CHECK(lion_store_dict_width(3) == 2, "3 entries: 2 bits");
	CHECK(lion_store_dict_width(4) == 4, "4 entries: 4 bits");
	CHECK(lion_store_dict_width(15) == 4, "15 entries: 4 bits");
	CHECK(lion_store_dict_width(16) == 8, "16 entries: 8 bits");
	CHECK(lion_store_dict_width(255) == 8, "255 entries: 8 bits");
	CHECK(lion_store_dict_width(256) == 16, "256 entries: 16 bits");
	CHECK(lion_store_dict_width(LION_STORE_MAX_DICT) == 16, "65535 entries: 16 bits");
	CHECK(lion_store_dict_width(LION_STORE_MAX_DICT + 1) == 0, "65536 entries: none");
	CHECK(lion_store_dict_width(PG_UINT32_MAX) == 0, "2^32 - 1 entries: none");

	/* every n: the width is valid, holds n (code 0 is NULL) and is the least */
	ok = true;
	for (n = 0; n <= LION_STORE_MAX_DICT && ok; n++)
	{
		int			want = lion_store_dict_width(n);
		bool		least = true;
		int			i;

		for (i = 0; i < (int) lengthof(widths); i++)
			if (widths[i] < want && lion_store_width_max(widths[i]) >= n &&
				n > 0)
				least = false;
		if (!lion_store_width_valid(want) ||
			lion_store_width_max(want) < n || !least)
			ok = false;
	}
	CHECK(ok, "dict_width is the least valid width holding n, for every n");

	for (w = 0; w <= 32; w++)
	{
		bool		want = (w == 1 || w == 2 || w == 4 || w == 8 || w == 16);

		CHECK(lion_store_width_valid(w) == want, "width_valid");
	}
	CHECK(lion_store_width_max(1) == 1 && lion_store_width_max(2) == 3 &&
		  lion_store_width_max(4) == 15 && lion_store_width_max(8) == 255 &&
		  lion_store_width_max(16) == 65535, "width_max");
}

static void
test_sizes(void)
{
	phase("sizes");
	CHECK(sizeof(LionStoreHeader) == 16, "the page header is 16 bytes");
	CHECK(sizeof(LionStoreDict) == 4, "the dictionary header is 4 bytes");
	CHECK(sizeof(LionStoreSub) == 4, "the sub-array header is 4 bytes");
	CHECK(LION_STORE_MAX_SLOTS == (1 << LION_OFFSET_BITS) - 1,
		  "a sub-array has a slot for every offset a TID can carry");

	CHECK(lion_store_codes_bytes(1, 0) == 0, "no codes, no bytes");
	CHECK(lion_store_codes_bytes(1, 1) == 1, "1 x 1 bit");
	CHECK(lion_store_codes_bytes(1, 8) == 1, "8 x 1 bit");
	CHECK(lion_store_codes_bytes(1, 9) == 2, "9 x 1 bit");
	CHECK(lion_store_codes_bytes(2, 4) == 1, "4 x 2 bits");
	CHECK(lion_store_codes_bytes(2, 5) == 2, "5 x 2 bits");
	CHECK(lion_store_codes_bytes(4, 3) == 2, "3 x 4 bits");
	CHECK(lion_store_codes_bytes(8, 291) == 291, "291 x 8 bits");
	CHECK(lion_store_codes_bytes(16, 291) == 582, "291 x 16 bits");
	CHECK(lion_store_codes_bytes(16, LION_STORE_MAX_SLOTS) ==
		  2 * (Size) LION_STORE_MAX_SLOTS, "a full sub-array of 16-bit codes");

	CHECK(lion_store_bitmap_bytes(0) == 0 && lion_store_bitmap_bytes(1) == 1 &&
		  lion_store_bitmap_bytes(8) == 1 && lion_store_bitmap_bytes(9) == 2,
		  "bitmap bytes");

	CHECK(lion_store_sub_len(LION_STORE_DICT, 8, 100, true) == sizeof(LionStoreSub),
		  "an ABSENT sub-array is its header");
	CHECK(lion_store_sub_len(LION_STORE_RAW, 8, 100, true) == sizeof(LionStoreSub),
		  "an ABSENT sub-array is its header, RAW too");
	CHECK(lion_store_sub_len(LION_STORE_DICT, 4, 7, false) == sizeof(LionStoreSub) + 4,
		  "DICT sub-array: 7 x 4 bits");
	CHECK(lion_store_sub_len(LION_STORE_RAW, 4, 7, false) == sizeof(LionStoreSub) + 28 + 1,
		  "RAW sub-array: 7 x 4 bytes and a bitmap byte");
	CHECK(lion_store_sub_len(LION_STORE_RAW, 8, 9, false) == sizeof(LionStoreSub) + 72 + 2,
		  "RAW sub-array: 9 x 8 bytes and two bitmap bytes");
	CHECK(lion_store_sub_len(LION_STORE_DICT, 1, 0, false) == sizeof(LionStoreSub),
		  "a sub-array no row was written to is its header");

	CHECK(lion_store_dict_len(4, 0, 0) == sizeof(LionStoreDict), "empty fixed dictionary");
	CHECK(lion_store_dict_len(4, 10, 0) == sizeof(LionStoreDict) + 40, "10 int4 entries");
	CHECK(lion_store_dict_len(16, 3, 0) == sizeof(LionStoreDict) + 48, "3 uuid entries");
	CHECK(lion_store_dict_len(-1, 0, 0) == sizeof(LionStoreDict) + 2,
		  "an empty varlena dictionary still has its one offset");
	CHECK(lion_store_dict_len(-1, 3, 17) == sizeof(LionStoreDict) + 17 + 8,
		  "3 varlena entries of 17 bytes: 4 offsets");
}

/* ----------------------------------------------------------------
 *							code packing
 * ----------------------------------------------------------------
 */

#define GUARD		0xA5
#define MAXCODES	1100

static void
test_codes_width(int width, uint32 nslots)
{
	Size		nbytes = lion_store_codes_bytes(width, nslots);
	uint8	   *buf = (uint8 *) malloc(nbytes + 16);
	uint32	   *ref = (uint32 *) calloc(nslots + 1, sizeof(uint32));
	uint32		maxc = lion_store_width_max(width);
	uint32		i;
	int			round;
	bool		ok;

	/* zeroed codes, guard bytes after them */
	memset(buf, 0, nbytes);
	memset(buf + nbytes, GUARD, 16);
	ok = true;
	for (i = 0; i < nslots; i++)
		if (lion_store_code_get(buf, width, i) != 0)
			ok = false;
	CHECK(ok, "zeroed codes read 0 (NULL)");

	/* random writes, each checked against the reference everywhere */
	for (round = 0; round < 4; round++)
	{
		uint32		writes = nslots * 3;

		for (i = 0; i < writes; i++)
		{
			uint32		at = rng_next() % nslots;
			uint32		code = rng_next() & maxc;

			if (i % 7 == 0)
				code = maxc;	/* all ones: a mask that leaks shows */
			else if (i % 11 == 0)
				code = 0;
			lion_store_code_set(buf, width, at, code);
			ref[at] = code;
		}
		ok = true;
		for (i = 0; i < nslots; i++)
			if (lion_store_code_get(buf, width, i) != ref[i])
				ok = false;
		CHECK(ok, "every code reads back as last written");
		ok = true;
		for (i = 0; i < 16; i++)
			if (buf[nbytes + i] != GUARD)
				ok = false;
		CHECK(ok, "nothing is written past the codes");
		/* the spare bits of the last byte stay zero */
		if ((nslots * (uint32) width) % 8 != 0)
		{
			uint32		used = (nslots * (uint32) width) % 8;

			CHECK((buf[nbytes - 1] >> used) == 0,
				  "the last byte's unused bits stay zero");
		}
	}

	/* the layout: least significant bit first, 16 bits low byte first */
	memset(buf, 0, nbytes);
	lion_store_code_set(buf, width, 0, 1);
	CHECK(buf[0] == 1, "code 0 starts at bit 0 of byte 0");
	if (nslots > 1)
	{
		memset(buf, 0, nbytes);
		lion_store_code_set(buf, width, 1, 1);
		if (width == 16)
			CHECK(buf[0] == 0 && buf[1] == 0 && buf[2] == 1 && buf[3] == 0,
				  "16-bit code 1 is bytes 2-3, low byte first");
		else if (width == 8)
			CHECK(buf[0] == 0 && buf[1] == 1, "8-bit code 1 is byte 1");
		else
			CHECK(buf[0] == (uint8) (1 << width), "code 1 follows code 0 in byte 0");
		CHECK(lion_store_code_byte(width, 1) == (Size) width / 8,
			  "code_byte of code 1");
	}
	if (width == 16 && nslots > 0)
	{
		memset(buf, 0, nbytes);
		lion_store_code_set(buf, 16, 0, 0x1234);
		CHECK(buf[0] == 0x34 && buf[1] == 0x12, "0x1234 is 34 12");
	}
	ok = true;
	for (i = 0; i < nslots; i++)
		if (lion_store_code_byte(width, i) != ((Size) i * width) / 8 ||
			lion_store_code_byte(width, i) + (width == 16 ? 1 : 0) >= nbytes)
			ok = false;
	CHECK(ok, "code_byte is the code's byte, within the codes");

	free(buf);
	free(ref);
}

static void
test_codes(void)
{
	static const uint32 sizes[] = {1, 2, 3, 7, 8, 9, 15, 16, 17, 63, 64, 65,
	291, 292, 511, MAXCODES};
	int			w;
	int			s;

	phase("DICT code packing");
	for (w = 0; w < (int) lengthof(widths); w++)
		for (s = 0; s < (int) lengthof(sizes); s++)
			test_codes_width(widths[w], sizes[s]);
}

static void
test_bitmap(void)
{
	static const uint32 sizes[] = {1, 7, 8, 9, 64, 291, 511};
	int			s;

	phase("RAW null bitmap");
	for (s = 0; s < (int) lengthof(sizes); s++)
	{
		uint32		n = sizes[s];
		Size		nbytes = lion_store_bitmap_bytes(n);
		uint8	   *bm = (uint8 *) malloc(nbytes + 4);
		bool	   *ref = (bool *) calloc(n, sizeof(bool));
		uint32		i;
		bool		ok = true;

		memset(bm, 0, nbytes);
		memset(bm + nbytes, GUARD, 4);
		for (i = 0; i < n * 4; i++)
		{
			uint32		at = rng_next() % n;
			bool		v = (rng_next() & 1) != 0;

			lion_store_null_set(bm, at, v);
			ref[at] = v;
		}
		for (i = 0; i < n; i++)
			if (lion_store_null_get(bm, i) != ref[i])
				ok = false;
		CHECK(ok, "every null bit reads back as last written");
		CHECK(bm[nbytes] == GUARD && bm[nbytes + 3] == GUARD,
			  "nothing is written past the bitmap");

		/* setting and clearing one bit leaves the others alone */
		memset(bm, 0xFF, nbytes);
		lion_store_null_set(bm, n - 1, false);
		ok = !lion_store_null_get(bm, n - 1);
		for (i = 0; i + 1 < n; i++)
			if (!lion_store_null_get(bm, i))
				ok = false;
		CHECK(ok, "clearing a bit clears that bit only");
		lion_store_null_set(bm, n - 1, true);
		CHECK(lion_store_null_get(bm, n - 1), "and setting it sets it");

		free(bm);
		free(ref);
	}
}

/* ----------------------------------------------------------------
 *						varlena dictionaries
 * ----------------------------------------------------------------
 */

/*
 * Append v to the varlena dictionary item (item, *len), as an in-place write
 * grows one: the values, then v, then the offsets, the last one new.  Returns
 * a new malloc'd item.
 */
static char *
vdict_append(const char *item, Size *len, const char *v, uint32 vlen)
{
	LionStoreDict d;
	uint32		nd;
	Size		newlen;
	char	   *n;
	uint32		e;
	uint16		o;

	memcpy(&d, item, sizeof(LionStoreDict));
	nd = (uint32) d.ndict + 1;
	newlen = lion_store_dict_len(-1, nd, d.nbytes + vlen);
	n = (char *) calloc(1, newlen);
	memcpy(n + sizeof(LionStoreDict), item + sizeof(LionStoreDict), d.nbytes);
	memcpy(n + sizeof(LionStoreDict) + d.nbytes, v, vlen);
	for (e = 0; e < nd; e++)
	{
		o = (uint16) lion_store_vdict_off(item, *len, d.ndict, e);
		memcpy(n + newlen - sizeof(uint16) * (nd + 1) + sizeof(uint16) * e,
			   &o, sizeof(uint16));
	}
	o = (uint16) (d.nbytes + vlen);
	memcpy(n + newlen - sizeof(uint16), &o, sizeof(uint16));
	d.ndict = (uint16) nd;
	d.nbytes = o;
	memcpy(n, &d, sizeof(LionStoreDict));
	*len = newlen;
	return n;
}

static void
test_vdict(void)
{
	static const char *words[] = {"", "a", "bb", "", "dddd", "e e e e e",
	"value 400", "\xff\x00\xfe", "longer than the others by a stretch"};
	static const uint32 wlens[] = {0, 1, 2, 0, 4, 9, 9, 3, 35};
	char	   *item;
	Size		len;
	LionStoreDict d;
	uint32		i;
	uint32		k;
	bool		ok;

	phase("varlena dictionaries");

	/* the empty dictionary: its header and the one offset, 0 */
	len = lion_store_dict_len(-1, 0, 0);
	item = (char *) calloc(1, len);
	CHECK(lion_store_vdict_check(item, len), "the empty dictionary is well formed");

	for (i = 0; i < (uint32) lengthof(words); i++)
	{
		char	   *grown = vdict_append(item, &len, words[i], wlens[i]);

		free(item);
		item = grown;
		CHECK(lion_store_vdict_check(item, len), "each append leaves it well formed");
		memcpy(&d, item, sizeof(LionStoreDict));
		CHECK(d.ndict == i + 1, "ndict counts the entries");
		ok = true;
		for (k = 0; k <= i; k++)
		{
			uint32		elen;
			const char *e = lion_store_vdict_entry(item, len, d.ndict, k, &elen);

			if (elen != wlens[k] || memcmp(e, words[k], elen) != 0)
				ok = false;
		}
		CHECK(ok, "every entry reads back, empty ones included");
	}

	/* every damage is refused */
	memcpy(&d, item, sizeof(LionStoreDict));
	{
		char	   *bad = (char *) malloc(len + 8);
		uint16		o;
		Size		offs = len - sizeof(uint16) * ((Size) d.ndict + 1);

		CHECK(!lion_store_vdict_check(item, 0), "an item shorter than its header");
		CHECK(!lion_store_vdict_check(item, 3), "an item cut inside its header");
		CHECK(!lion_store_vdict_check(item, len - 1), "an item one byte short");
		memcpy(bad, item, len);
		bad[len] = 0;
		bad[len + 1] = 0;
		CHECK(!lion_store_vdict_check(bad, len + 2), "an item with two bytes too many");

		memcpy(bad, item, len);
		o = 1;
		memcpy(bad + offs, &o, sizeof(uint16));
		CHECK(!lion_store_vdict_check(bad, len), "offset 0 not 0");

		memcpy(bad, item, len);
		o = 0;
		memcpy(bad + offs + 4 * sizeof(uint16), &o, sizeof(uint16));
		CHECK(!lion_store_vdict_check(bad, len), "offsets descending");

		memcpy(bad, item, len);
		o = (uint16) (d.nbytes + 1);
		memcpy(bad + offs + 3 * sizeof(uint16), &o, sizeof(uint16));
		CHECK(!lion_store_vdict_check(bad, len), "an offset past the values");

		memcpy(bad, item, len);
		o = (uint16) (d.nbytes - 1);
		memcpy(bad + len - sizeof(uint16), &o, sizeof(uint16));
		CHECK(!lion_store_vdict_check(bad, len), "the last offset short of nbytes");

		memcpy(bad, item, len);
		{
			LionStoreDict bd = d;

			bd.ndict++;
			memcpy(bad, &bd, sizeof(LionStoreDict));
		}
		CHECK(!lion_store_vdict_check(bad, len), "ndict one too many");

		memcpy(bad, item, len);
		{
			LionStoreDict bd = d;

			bd.nbytes += 2;
			memcpy(bad, &bd, sizeof(LionStoreDict));
		}
		CHECK(!lion_store_vdict_check(bad, len), "nbytes two too many");
		free(bad);
	}
	free(item);

	/* many entries of random lengths, up to the 16-bit codes' worth */
	len = lion_store_dict_len(-1, 0, 0);
	item = (char *) calloc(1, len);
	{
		uint32		total = 0;
		uint32		n = 0;
		uint32	   *lens = (uint32 *) calloc(4000, sizeof(uint32));
		uint32	   *seeds = (uint32 *) calloc(4000, sizeof(uint32));
		char		v[40];

		while (n < 4000)
		{
			uint32		vlen = rng_next() % 13;
			uint32		seed = rng_next();
			char	   *grown;

			if (total + vlen > 30000)
				break;
			for (i = 0; i < vlen; i++)
				v[i] = (char) (seed >> (i % 4 * 8)) + (char) i;
			grown = vdict_append(item, &len, v, vlen);
			free(item);
			item = grown;
			lens[n] = vlen;
			seeds[n] = seed;
			n++;
			total += vlen;
		}
		CHECK(lion_store_vdict_check(item, len), "a dictionary of thousands of entries");
		memcpy(&d, item, sizeof(LionStoreDict));
		CHECK(d.ndict == n && d.nbytes == total, "its header counts them");
		CHECK(lion_store_dict_width(d.ndict) == 16, "and its codes are 16 bits");
		ok = true;
		for (k = 0; k < n; k++)
		{
			uint32		elen;
			const char *e = lion_store_vdict_entry(item, len, d.ndict, k, &elen);

			if (elen != lens[k])
				ok = false;
			else
				for (i = 0; i < elen; i++)
					if (e[i] != (char) ((char) (seeds[k] >> (i % 4 * 8)) + (char) i))
						ok = false;
		}
		CHECK(ok, "every one reads back");
		free(lens);
		free(seeds);
	}
	free(item);
}

/* ----------------------------------------------------------------
 *							DICT or RAW
 * ----------------------------------------------------------------
 */

static void
test_prefer_raw(void)
{
	phase("DICT or RAW");
	CHECK(!lion_store_prefer_raw(1000000, 10, 16, 0),
		  "a column with no RAW width never goes RAW");
	CHECK(!lion_store_prefer_raw(12, 300, 2, 4),
		  "int4, 3 values over 300 rows: DICT");
	CHECK(lion_store_prefer_raw(300 * 4, 300, 16, 4),
		  "int4, a value a row: RAW");
	/* the boundary: dict + codes against rows * rawwidth */
	CHECK(!lion_store_prefer_raw(100 * 8 - 100, 100, 8, 8),
		  "equal sizes stay DICT");
	CHECK(lion_store_prefer_raw(100 * 8 - 100 + 1, 100, 8, 8),
		  "one byte more goes RAW");
	CHECK(!lion_store_prefer_raw(0, 0, 1, 4), "no rows: DICT");
	/* codes rounded up to whole bytes */
	CHECK(lion_store_prefer_raw(0, 3, 16, 1),
		  "16-bit codes for bool-wide values: RAW");
	CHECK(!lion_store_prefer_raw(0, 9, 1, 1),
		  "1-bit codes for 9 one-byte values: DICT (2 bytes against 9)");
}

/* ----------------------------------------------------------------
 *				Key-ordered windows (DESIGN.md section 41)
 * ----------------------------------------------------------------
 */

static void
test_ordered(void)
{
	char		buf[1024];
	LionStoreWinHdr wh;
	LionStoreBucket bk[3];
	LionStoreDirEnt de[2];
	Size		len;

	phase("virtual addresses and the window header");
	CHECK(LION_STORE_MAX_VPAGES == 2 * LION_BLOCKS_PER_CONTAINER,
		  "a window has twice its heap pages in virtual pages");
	CHECK(lion_store_vlo_page(lion_store_vlo(LION_STORE_MAX_VPAGES - 1,
											 (1 << LION_OFFSET_BITS) - 1)) ==
		  LION_STORE_MAX_VPAGES - 1, "the last virtual page fits 16 bits");
	CHECK(lion_store_vlo_off(lion_store_vlo(5, 7)) == 7 &&
		  lion_store_vlo_page(lion_store_vlo(5, 7)) == 5, "vlo round trip");

	/* three buckets (the first LOW, one NULL, one with 4 fence bytes), two entries */
	memset(bk, 0, sizeof(bk));
	bk[0].vpage = 0;
	bk[0].flags = LION_STORE_BK_LOW;
	bk[0].used = 10;
	bk[1].vpage = 7;
	bk[1].used = 291;
	bk[1].lo = 3;
	bk[1].fenceoff = 0;
	bk[1].fencelen = 4;
	bk[2].vpage = 3;
	bk[2].flags = LION_STORE_BK_NULL;
	de[0].ord = 0;
	de[0].lo = 2;
	de[0].blk = 100;
	de[1].ord = 1;
	de[1].lo = 0;
	de[1].blk = 200;
	wh.gen = 9;
	wh.nbucket = 3;
	wh.ndir = 2;
	wh.fencebytes = 4;
	wh.flags = 0;
	len = lion_store_winhdr_len(3, 2, 4);
	CHECK(len == 12 + 30 + 16 + 4, "a window header's length");
	memcpy(buf, &wh, sizeof(wh));
	memcpy(buf + sizeof(wh), bk, sizeof(bk));
	memcpy(buf + sizeof(wh) + sizeof(bk), de, sizeof(de));
	memcpy(buf + sizeof(wh) + sizeof(bk) + sizeof(de), "abcd", 4);
	CHECK(lion_store_winhdr_check(buf, len, 291) == NULL, "a sound header passes");
	CHECK(lion_store_winhdr_check(buf, len - 1, 291) != NULL, "a short one does not");
	CHECK(lion_store_winhdr_check(buf, len, 290) != NULL,
		  "nor one whose bucket handed out more slots than a bucket has");

	bk[2].vpage = 7;
	memcpy(buf + sizeof(wh), bk, sizeof(bk));
	CHECK(lion_store_winhdr_check(buf, len, 291) != NULL,
		  "two buckets of one virtual page do not");
	bk[2].vpage = 3;
	bk[1].fencelen = 5;
	memcpy(buf + sizeof(wh), bk, sizeof(bk));
	CHECK(lion_store_winhdr_check(buf, len, 291) != NULL,
		  "a fence past the fence area does not");
	bk[1].fencelen = 4;
	bk[1].flags = LION_STORE_BK_LOW;
	memcpy(buf + sizeof(wh), bk, sizeof(bk));
	CHECK(lion_store_winhdr_check(buf, len, 291) != NULL,
		  "a second LOW bucket does not");
	bk[1].flags = 0;
	memcpy(buf + sizeof(wh), bk, sizeof(bk));
	de[1].ord = 0;
	de[1].lo = 2;
	memcpy(buf + sizeof(wh) + sizeof(bk), de, sizeof(de));
	CHECK(lion_store_winhdr_check(buf, len, 291) != NULL,
		  "a directory out of order does not");
}

int
main(void)
{
	printf("pg_lion window store format unit tests\n");
	printf("  LION_STORE_MAX_SLOTS=%d LION_STORE_MAX_DICT=%d\n",
		   LION_STORE_MAX_SLOTS, LION_STORE_MAX_DICT);

	rng_seed(UINT64CONST(0x5EED4000));
	test_widths();
	test_sizes();
	test_codes();
	test_bitmap();
	test_vdict();
	test_prefer_raw();
	test_ordered();

	printf("\n%ld checks, %ld failures\n", nchecks, nfail);
	if (nfail == 0)
		printf("ALL TESTS PASSED\n");
	else
		printf("*** %ld TESTS FAILED ***\n", nfail);
	return nfail == 0 ? 0 : 1;
}
