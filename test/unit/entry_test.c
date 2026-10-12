/*-------------------------------------------------------------------------
 * entry_test.c
 *	  Standalone unit tests for the directory entry layout of
 *	  src/lion_entryfmt.h (DESIGN.md §21, "Entry tuples", format version 10).
 *
 *	  Built by "make unit PG_CONFIG=..." with -DFRONTEND, so this program
 *	  links against nothing but libc.
 *
 *	  Every entry under test is read back two ways: through the accessors the
 *	  index uses, and byte by byte at the offsets DESIGN.md §21 documents,
 *	  with a reader of its own that knows nothing of the structs.  So a change
 *	  to the layout that the accessors follow but the documented format does
 *	  not - a moved field, a different header size, a key at another offset -
 *	  fails here rather than in an index on disk.
 *
 *	  Exit status is 0 only if every check passed.
 *-------------------------------------------------------------------------
 */
#include "c.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lion_entryfmt.h"

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
			printf("FAIL [%s] entry_test.c:%d: %s\n", cur_phase, line, msg);
		else if (nfail == 41)
			printf("... further failures suppressed\n");
	}
}

#define CHECK(ok, msg)	check_impl((ok), __LINE__, (msg))

static void
phase(const char *name)
{
	cur_phase = name;
}

/* ----------------------------------------------------------------
 *			an independent reader of the documented layout
 * ----------------------------------------------------------------
 */

/* The tests write native-endian fields, so read them the same way. */
static uint16
rd16(const char *p, Size off)
{
	uint16		v;

	memcpy(&v, p + off, sizeof(v));
	return v;
}

static uint32
rd32(const char *p, Size off)
{
	uint32		v;

	memcpy(&v, p + off, sizeof(v));
	return v;
}

static uint64
rd64(const char *p, Size off)
{
	uint64		v;

	memcpy(&v, p + off, sizeof(v));
	return v;
}

/*
 * The documented format, spelled out with numbers (DESIGN.md §21):
 *
 *	short header (INLINE, pivot), 12 bytes:
 *		0 flags, 2 keylen, 4 attno, 6 ncontainers (uint16), 8 ntids / child
 *	CHAIN header, 28 bytes:
 *		0 flags, 2 keylen, 4 attno, 6 zero, 8 head, 12 tail, 16 ntids
 *		(uint64), 24 ncontainers (uint32)
 *	the key at the header's end, or at the next 8-byte boundary with
 *	ALIGNKEY (0x0400); the positions extension (8 bytes) at the next 4-byte
 *	boundary after the key, with POSITIONS (0x0200); the payload after it,
 *	or after the key, unaligned.
 */
#define DOC_INLINE		0x0001
#define DOC_CHAIN		0x0002
#define DOC_HIGHKEY		0x0010
#define DOC_DOWNLINK	0x0020
#define DOC_POSITIONS	0x0200
#define DOC_ALIGNKEY	0x0400

static Size
doc_hdrsz(uint16 flags)
{
	return (flags & DOC_CHAIN) ? 28 : 12;
}

static Size
doc_keyoff(uint16 flags)
{
	Size		h = doc_hdrsz(flags);

	return (flags & DOC_ALIGNKEY) ? (h + 7) & ~(Size) 7 : h;
}

static Size
doc_payoff(uint16 flags, Size keylen)
{
	Size		end = doc_keyoff(flags) + keylen;

	if (flags & DOC_POSITIONS)
		end = ((end + 3) & ~(Size) 3) + 8;
	return end;
}

/* a MAXALIGNed buffer an entry is built in */
typedef union
{
	char		b[512];
	double		align_d;
	int64		align_l;
} EntryBuf;

/* ----------------------------------------------------------------
 *								tests
 * ----------------------------------------------------------------
 */

static void
test_constants(void)
{
	phase("constants");

	CHECK(LION_ENTRY_SHORT_HDRSZ == 12, "short header is 12 bytes");
	CHECK(LION_ENTRY_CHAIN_HDRSZ == 28, "CHAIN header is 28 bytes");
	CHECK(sizeof(LionEntryTuple) == 12, "the struct is the short header");
	CHECK(offsetof(LionEntryTuple, flags) == 0, "flags at 0");
	CHECK(offsetof(LionEntryTuple, keylen) == 2, "keylen at 2");
	CHECK(offsetof(LionEntryTuple, attno) == 4, "attno at 4");
	CHECK(offsetof(LionEntryTuple, sh_ncontainers) == 6, "short ncontainers at 6");
	CHECK(offsetof(LionEntryTuple, sh_word) == 8, "short word at 8");
	CHECK(offsetof(LionEntryChainHdr, head) == 8, "head at 8");
	CHECK(offsetof(LionEntryChainHdr, tail) == 12, "tail at 12");
	CHECK(offsetof(LionEntryChainHdr, ntids) == 16, "ntids at 16");
	CHECK(offsetof(LionEntryChainHdr, ncontainers) == 24, "ncontainers at 24");
	CHECK(LION_ENTRY_POSEXT_SIZE == 8, "positions extension is 8 bytes");
	CHECK(LION_ENTRY_INLINE == DOC_INLINE && LION_ENTRY_CHAIN == DOC_CHAIN &&
		  LION_ENTRY_HIGHKEY == DOC_HIGHKEY &&
		  LION_ENTRY_DOWNLINK == DOC_DOWNLINK &&
		  LION_ENTRY_POSITIONS == DOC_POSITIONS &&
		  LION_ENTRY_ALIGNKEY == DOC_ALIGNKEY, "flag values");
	CHECK((LION_ENTRY_ALLFLAGS & LION_ENTRY_ALIGNKEY) != 0,
		  "ALIGNKEY is a known flag");
	CHECK(LION_ENTRY_MAX_KEYOFF == 32, "the largest key offset");
}

/* Every combination of the layout flags against the documented offsets. */
static void
test_offsets(void)
{
	static const uint16 forms[] = {
		DOC_INLINE, DOC_CHAIN, DOC_HIGHKEY, DOC_DOWNLINK
	};
	static const Size keylens[] = {0, 1, 2, 3, 4, 5, 7, 8, 9, 16, 37, 2000};
	int			f,
				k,
				extra;

	phase("offsets");
	for (f = 0; f < (int) lengthof(forms); f++)
		for (extra = 0; extra < 4; extra++)
			for (k = 0; k < (int) lengthof(keylens); k++)
			{
				uint16		flags = forms[f] |
					((extra & 1) ? DOC_ALIGNKEY : 0) |
					((extra & 2) && (forms[f] & (DOC_INLINE | DOC_CHAIN)) ?
					 DOC_POSITIONS : 0);
				Size		keylen = keylens[k];
				char		msg[128];

				snprintf(msg, sizeof(msg), "flags 0x%04X keylen %zu",
						 flags, keylen);
				CHECK(lion_entry_hdrsz_for(flags) == doc_hdrsz(flags), msg);
				CHECK(lion_entry_keyoff_for(flags) == doc_keyoff(flags), msg);
				CHECK(lion_entry_payoff_for(flags, keylen) ==
					  doc_payoff(flags, keylen), msg);
				if (flags & DOC_ALIGNKEY)
					CHECK(lion_entry_keyoff_for(flags) % 8 == 0, msg);
				else
					CHECK(lion_entry_keyoff_for(flags) % 4 == 0, msg);
				if (flags & DOC_POSITIONS)
					CHECK(lion_entry_extoff_for(flags, keylen) % 4 == 0, msg);
			}

	/* the sizes the format is for: an int4 key, an int8 key, no padding */
	CHECK(lion_entry_payoff_for(LION_ENTRY_INLINE, 4) == 16,
		  "INLINE entry of an int4 key: payload at 16");
	CHECK(lion_entry_payoff_for(LION_ENTRY_INLINE, 8) == 20,
		  "INLINE entry of an int8 key: payload at 20");
	CHECK(lion_entry_payoff_for(LION_ENTRY_INLINE, 2) == 14,
		  "INLINE entry of an int2 key: payload at 14");
	CHECK(lion_entry_payoff_for(LION_ENTRY_CHAIN, 4) == 32,
		  "CHAIN entry of an int4 key is 32 bytes");
	CHECK(lion_entry_payoff_for(LION_ENTRY_INLINE | LION_ENTRY_ALIGNKEY, 16) == 32,
		  "INLINE entry of a 16-byte double-aligned key: payload at 32");
	CHECK(lion_entry_payoff_for(LION_ENTRY_INLINE | LION_ENTRY_POSITIONS, 5) == 28,
		  "positions extension after a 5-byte key at 20, payload at 28");
}

/* An INLINE entry: init, counts, key, payload, read back by the bytes. */
static void
test_inline(void)
{
	EntryBuf	buf;
	LionEntryTuple *e = (LionEntryTuple *) buf.b;
	const char	key[4] = {0x11, 0x22, 0x33, 0x44};
	const char	payload[5] = {9, 8, 7, 6, 5};
	Size		payoff;

	phase("inline");
	memset(buf.b, 0x7f, sizeof(buf.b));
	lion_entry_init(e, LION_ENTRY_INLINE, 3, 4);
	payoff = LionEntryPayloadOffset(e);
	CHECK(payoff == 16, "payload offset");
	memcpy(LionEntryGetKey(e), key, 4);
	memcpy(LionEntryGetPayload(e), payload, 5);

	CHECK(!lion_entry_is_chain(e), "not a chain");
	CHECK(lion_entry_ntids(e) == 0 && lion_entry_ncontainers(e) == 0,
		  "empty counts");
	CHECK(lion_entry_head(e) == InvalidBlockNumber &&
		  lion_entry_tail(e) == InvalidBlockNumber, "no head or tail");

	CHECK(lion_entry_try_set_ntids(e, 123456789), "set ntids");
	CHECK(lion_entry_try_set_ncontainers(e, 777), "set ncontainers");
	CHECK(lion_entry_ntids(e) == 123456789, "ntids");
	CHECK(lion_entry_ncontainers(e) == 777, "ncontainers");

	CHECK(rd16(buf.b, 0) == LION_ENTRY_INLINE, "flags bytes");
	CHECK(rd16(buf.b, 2) == 4, "keylen bytes");
	CHECK(rd16(buf.b, 4) == 3, "attno bytes");
	CHECK(rd16(buf.b, 6) == 777, "ncontainers bytes");
	CHECK(rd32(buf.b, 8) == 123456789, "ntids bytes");
	CHECK(memcmp(buf.b + 12, key, 4) == 0, "key bytes at 12");
	CHECK(memcmp(buf.b + 16, payload, 5) == 0, "payload bytes at 16");

	/* the short form's limits: refused, and nothing stored */
	CHECK(lion_entry_try_set_ntids(e, PG_UINT32_MAX), "ntids at the limit");
	CHECK(lion_entry_ntids(e) == PG_UINT32_MAX, "ntids at the limit read");
	CHECK(!lion_entry_try_set_ntids(e, (uint64) PG_UINT32_MAX + 1),
		  "ntids past the short form refused");
	CHECK(lion_entry_ntids(e) == PG_UINT32_MAX, "a refused ntids stores nothing");
	CHECK(lion_entry_try_set_ncontainers(e, PG_UINT16_MAX),
		  "ncontainers at the limit");
	CHECK(!lion_entry_try_set_ncontainers(e, PG_UINT16_MAX + 1),
		  "ncontainers past the short form refused");
	CHECK(lion_entry_ncontainers(e) == PG_UINT16_MAX,
		  "a refused ncontainers stores nothing");
}

/* A CHAIN entry, made from an INLINE one the way a spill makes it. */
static void
test_chain_reshape(void)
{
	EntryBuf	ibuf;
	EntryBuf	cbuf;
	LionEntryTuple *in = (LionEntryTuple *) ibuf.b;
	LionEntryTuple *ch = (LionEntryTuple *) cbuf.b;
	const char	key[9] = "abcdefgh";
	uint16		iflags = LION_ENTRY_INLINE | LION_ENTRY_POSITIONS |
		LION_ENTRY_SUMMARY;
	uint16		cflags = (iflags & ~LION_ENTRY_INLINE) | LION_ENTRY_CHAIN;
	LionEntryPosExt x;
	Size		payoff;

	phase("chain");
	memset(ibuf.b, 0x55, sizeof(ibuf.b));
	memset(cbuf.b, 0x66, sizeof(cbuf.b));
	lion_entry_init(in, iflags, 2, 9);
	memcpy(LionEntryGetKey(in), key, 9);
	x.pos_root = 4242;
	x.pos_len = 0;
	memcpy(ibuf.b + LionEntryExtOffset(in), &x, sizeof(x));
	CHECK(lion_entry_try_set_ntids(in, 5000), "inline ntids");
	CHECK(lion_entry_try_set_ncontainers(in, 4), "inline ncontainers");

	payoff = lion_entry_copy_reshaped(ch, in, cflags);
	CHECK(payoff == doc_payoff(cflags, 9), "CHAIN payload offset");
	CHECK(payoff == 48, "CHAIN entry with a 9-byte key and positions is 48 bytes");
	CHECK(lion_entry_is_chain(ch), "is a chain");
	CHECK(ch->attno == 2 && ch->keylen == 9, "column and key length kept");
	CHECK(memcmp(LionEntryGetKey(ch), key, 9) == 0, "key kept");
	CHECK(memcmp(cbuf.b + 28, key, 9) == 0, "key bytes at 28");
	CHECK(rd32(cbuf.b, 40) == 4242 && rd32(cbuf.b, 44) == 0,
		  "positions extension at 40, copied");
	CHECK(lion_entry_head(ch) == InvalidBlockNumber &&
		  lion_entry_tail(ch) == InvalidBlockNumber, "no links yet");
	CHECK(lion_entry_ntids(ch) == 0 && lion_entry_ncontainers(ch) == 0,
		  "counts are the caller's");

	lion_entry_set_head(ch, 17);
	lion_entry_set_tail(ch, 99);
	CHECK(lion_entry_try_set_ntids(ch, (uint64) 1 << 40), "a 64-bit ntids");
	CHECK(lion_entry_try_set_ncontainers(ch, 1000000), "a 32-bit ncontainers");
	CHECK(rd16(cbuf.b, 0) == cflags, "flags bytes");
	CHECK(rd16(cbuf.b, 2) == 9 && rd16(cbuf.b, 4) == 2, "keylen, attno bytes");
	CHECK(rd16(cbuf.b, 6) == 0, "the unused field is zero");
	CHECK(rd32(cbuf.b, 8) == 17, "head bytes at 8");
	CHECK(rd32(cbuf.b, 12) == 99, "tail bytes at 12");
	CHECK(rd64(cbuf.b, 16) == ((uint64) 1 << 40), "ntids bytes at 16");
	CHECK(rd32(cbuf.b, 24) == 1000000, "ncontainers bytes at 24");
	CHECK(lion_entry_head(ch) == 17 && lion_entry_tail(ch) == 99, "links");

	/* and back the other way: a copy keeps the form it is given */
	{
		EntryBuf	b2;
		LionEntryTuple *again = (LionEntryTuple *) b2.b;

		(void) lion_entry_copy_reshaped(again, ch, cflags);
		CHECK(memcmp(LionEntryGetKey(again), key, 9) == 0, "copy keeps the key");
	}
}

/* Pivots: the short header with the child in it, and no counts. */
static void
test_pivot(void)
{
	EntryBuf	buf;
	LionEntryTuple *p = (LionEntryTuple *) buf.b;

	phase("pivot");
	memset(buf.b, 0x33, sizeof(buf.b));
	lion_entry_init(p, LION_ENTRY_DOWNLINK | LION_ENTRY_ALIGNKEY, 1, 16);
	CHECK(LionEntryKeyOffset(p) == 16, "a double-aligned key after the short header");
	CHECK(rd32(buf.b, 12) == 0, "the alignment padding is zeroed");
	CHECK(lion_entry_head(p) == InvalidBlockNumber, "no child yet");
	lion_entry_set_head(p, 4321);
	CHECK(lion_entry_head(p) == 4321, "child");
	CHECK(rd32(buf.b, 8) == 4321, "child bytes at 8");
	CHECK(lion_entry_ntids(p) == 0 && lion_entry_ncontainers(p) == 0,
		  "a pivot has no counts");
	CHECK(lion_entry_try_set_ntids(p, 0), "a pivot takes a zero count");
	CHECK(!lion_entry_try_set_ntids(p, 1), "a pivot refuses a count");
	CHECK(lion_entry_head(p) == 4321, "a refused count leaves the child");

	lion_entry_init(p, LION_ENTRY_HIGHKEY, 1, 4);
	CHECK(lion_entry_head(p) == InvalidBlockNumber, "a high key has no child");
	CHECK(LionEntryPayloadOffset(p) == 16, "a high key of 4 bytes is 16 bytes");
}

/* By-value keys at their width, read back as fetch_att() reads them. */
static void
test_byval(void)
{
	char		b[8];
	static const int64 vals[] = {
		0, 1, -1, 127, -128, 255, 32767, -32768, 65535, PG_INT32_MAX,
		PG_INT32_MIN, PG_INT64_MAX, PG_INT64_MIN, 0x0123456789abcdefLL
	};
	int			i;

	phase("byval");
	for (i = 0; i < (int) lengthof(vals); i++)
	{
		char		msg[96];
		int16		v2 = (int16) vals[i];
		int32		v4 = (int32) vals[i];
		char		v1 = (char) vals[i];

		snprintf(msg, sizeof(msg), "value " INT64_FORMAT, vals[i]);

		memset(b, 0xee, sizeof(b));
		lion_key_store_byval(b, (uint64) (int64) v1, 1);
		CHECK(memcmp(b, &v1, 1) == 0 && (uint8) b[1] == 0xee, msg);
		CHECK(lion_key_fetch_byval(b, 1) == (uint64) v1, msg);

		memset(b, 0xee, sizeof(b));
		lion_key_store_byval(b, (uint64) (int64) v2, 2);
		CHECK(memcmp(b, &v2, 2) == 0 && (uint8) b[2] == 0xee, msg);
		CHECK(lion_key_fetch_byval(b, 2) == (uint64) (int64) v2, msg);

		memset(b, 0xee, sizeof(b));
		lion_key_store_byval(b, (uint64) (int64) v4, 4);
		CHECK(memcmp(b, &v4, 4) == 0 && (uint8) b[4] == 0xee, msg);
		CHECK(lion_key_fetch_byval(b, 4) == (uint64) (int64) v4, msg);

		memset(b, 0xee, sizeof(b));
		lion_key_store_byval(b, (uint64) vals[i], 8);
		CHECK(memcmp(b, &vals[i], 8) == 0, msg);
		CHECK(lion_key_fetch_byval(b, 8) == (uint64) vals[i], msg);
	}

	/* the store reads only the low bytes: high garbage does not matter */
	lion_key_store_byval(b, UINT64CONST(0xdeadbeef00000005), 4);
	CHECK(lion_key_fetch_byval(b, 4) == 5, "high bits of the value are dropped");

	/* and it may be unaligned, as a key after an odd payload offset is */
	{
		char		u[16];

		lion_key_store_byval(u + 3, (uint64) (int64) -42, 4);
		CHECK(lion_key_fetch_byval(u + 3, 4) == (uint64) (int64) -42,
			  "an unaligned key");
		lion_key_store_byval(u + 5, UINT64CONST(0x8000000000000001), 8);
		CHECK(lion_key_fetch_byval(u + 5, 8) == UINT64CONST(0x8000000000000001),
			  "an unaligned 8-byte key");
	}
}

/* lion_entry_init() leaves nothing of the buffer it was given before the payload. */
static void
test_init_deterministic(void)
{
	static const uint16 flagsets[] = {
		LION_ENTRY_INLINE, LION_ENTRY_CHAIN,
		LION_ENTRY_INLINE | LION_ENTRY_POSITIONS | LION_ENTRY_ALIGNKEY,
		LION_ENTRY_CHAIN | LION_ENTRY_POSITIONS,
		LION_ENTRY_HIGHKEY, LION_ENTRY_DOWNLINK | LION_ENTRY_ALIGNKEY,
		LION_ENTRY_INLINE | LION_ENTRY_NULLKEY
	};
	int			i;

	phase("init");
	for (i = 0; i < (int) lengthof(flagsets); i++)
	{
		EntryBuf	a;
		EntryBuf	b;
		Size		keylen = (flagsets[i] & LION_ENTRY_NULLKEY) ? 0 : 6;
		Size		payoff = lion_entry_payoff_for(flagsets[i], keylen);

		memset(a.b, 0x00, sizeof(a.b));
		memset(b.b, 0xff, sizeof(b.b));
		lion_entry_init((LionEntryTuple *) a.b, flagsets[i], 1, (uint16) keylen);
		lion_entry_init((LionEntryTuple *) b.b, flagsets[i], 1, (uint16) keylen);
		/* the key itself is the caller's; everything else is defined */
		memset(a.b + lion_entry_keyoff_for(flagsets[i]), 0, keylen);
		memset(b.b + lion_entry_keyoff_for(flagsets[i]), 0, keylen);
		CHECK(memcmp(a.b, b.b, payoff) == 0,
			  "an initialised header does not depend on the buffer's old bytes");
		if (flagsets[i] & LION_ENTRY_POSITIONS)
		{
			Size		xo = lion_entry_extoff_for(flagsets[i], keylen);

			CHECK(rd32(a.b, xo) == InvalidBlockNumber && rd32(a.b, xo + 4) == 0,
				  "an empty positions extension");
		}
	}
}

int
main(void)
{
	test_constants();
	test_offsets();
	test_inline();
	test_chain_reshape();
	test_pivot();
	test_byval();
	test_init_deterministic();

	printf("entry_test: %ld checks, %ld failures\n", nchecks, nfail);
	if (nfail != 0)
		return 1;
	printf("ALL TESTS PASSED\n");
	return 0;
}
