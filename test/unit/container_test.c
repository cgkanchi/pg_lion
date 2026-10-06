/*-------------------------------------------------------------------------
 * container_test.c
 *	  Standalone unit tests for src/lion_container.c.
 *
 *	  Built by "make unit PG_CONFIG=..." with -DFRONTEND, so this program
 *	  links against nothing but libc and the container library itself.
 *
 *	  Every container under test is shadowed by a brute-force reference: a
 *	  plain bool[32768].  After each phase (and periodically inside the
 *	  randomized phases) the container is compared against the reference for
 *	  all 32768 possible members, for cardinality, for to_array() and for
 *	  iterate() order, and lion_container_check() must accept it.
 *
 *	  Exit status is 0 only if every check passed.
 *-------------------------------------------------------------------------
 */
#include "c.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "port/pg_bitutils.h"

#include "lion_container.h"

#define TEST_CKEY	0x0BADF00D

/* ----------------------------------------------------------------
 *							bookkeeping
 * ----------------------------------------------------------------
 */

static const char *cur_phase = "startup";
static const char *phase_suffix = NULL;	/* the kernel forced, if any */
static char phase_buf[128];
static long nchecks = 0;
static long nfail = 0;
static long nmember_cmps = 0;

static void
check_impl(bool ok, int line, const char *msg)
{
	nchecks++;
	if (!ok)
	{
		nfail++;
		if (nfail <= 40)
			printf("FAIL [%s] container_test.c:%d: %s\n", cur_phase, line, msg);
		else if (nfail == 41)
			printf("... further failures suppressed\n");
	}
}

#define CHECK(ok, msg)	check_impl((ok), __LINE__, (msg))

static void
phase(const char *name)
{
	if (phase_suffix == NULL)
		cur_phase = name;
	else
	{
		snprintf(phase_buf, sizeof(phase_buf), "%s; %s", name, phase_suffix);
		cur_phase = phase_buf;
	}
}

/* ----------------------------------------------------------------
 *						fixed-seed PRNG (xorshift64*)
 * ----------------------------------------------------------------
 */

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
	return (uint32) ((x * UINT64CONST(2685821657736338717)) >> 32);
}

static uint32
rng_below(uint32 n)
{
	return rng_next() % n;
}

/* ----------------------------------------------------------------
 *						container work buffer
 * ----------------------------------------------------------------
 */

typedef union CBuf
{
	LionContainer c;
	uint64		force_align;
	char		data[LION_CONTAINER_MAX_SIZE];
} CBuf;

/* ----------------------------------------------------------------
 *						brute-force reference
 * ----------------------------------------------------------------
 */

typedef struct Ref
{
	bool		m[LION_CONTAINER_RANGE];
	uint32		card;
} Ref;

static void
ref_init(Ref *r)
{
	memset(r->m, 0, sizeof(r->m));
	r->card = 0;
}

static bool
ref_add(Ref *r, uint32 lo)
{
	if (r->m[lo])
		return false;
	r->m[lo] = true;
	r->card++;
	return true;
}

static bool
ref_remove(Ref *r, uint32 lo)
{
	if (!r->m[lo])
		return false;
	r->m[lo] = false;
	r->card--;
	return true;
}

static uint32
ref_range_card(const Ref *r, uint32 s, uint32 e)
{
	uint32		i;
	uint32		n = 0;

	if (s > e)
		return 0;
	for (i = s; i <= e; i++)
		if (r->m[i])
			n++;
	return n;
}

static uint32
ref_remove_range(Ref *r, uint32 s, uint32 e)
{
	uint32		i;
	uint32		n = 0;

	if (s > e)
		return 0;
	for (i = s; i <= e; i++)
		if (r->m[i])
		{
			r->m[i] = false;
			r->card--;
			n++;
		}
	return n;
}

static uint32
ref_nruns(const Ref *r)
{
	uint32		i;
	uint32		n = 0;

	for (i = 0; i < LION_CONTAINER_RANGE; i++)
		if (r->m[i] && (i == 0 || !r->m[i - 1]))
			n++;
	return n;
}

/*
 * NARROW (DESIGN.md §38), written here from lion_container.h's layout rather
 * than through the library.  A NARROW of width k, 1 .. LION_NARROW_MAX_WIDTH,
 * holds the members whose offset is below 64 * k: member lo is bit
 * (block * 64 * k + offset) of the payload's LION_WIDTH_WORDS(k) words, the
 * width in the header's flags byte.  A BITSET is the same layout at the full
 * width, LION_BITSET_WIDTH.
 */
#define TEST_KMAX			((uint32) LION_NARROW_MAX_WIDTH)
#define TEST_KMAX_LABELS	(3 * LION_NARROW_MAX_WIDTH)
#define TEST_FULL			((uint32) LION_BITSET_WIDTH)
#define TEST_NARROW_DATA(c)	((uint64 *) LION_CONTAINER_PAYLOAD(c))
#define TEST_OFFSET_MASK	((1U << LION_OFFSET_BITS) - 1)

/* The narrowest width whose bits hold lo's offset. */
static uint32
test_lo_width(uint32 lo)
{
	return ((lo & TEST_OFFSET_MASK) >> 6) + 1;
}

/* Does a bitmap of width k hold lo's offset? */
static bool
test_lo_fits(uint32 lo, uint32 k)
{
	return (lo & TEST_OFFSET_MASK) < 64 * k;
}

static uint32
test_narrow_bit(uint32 lo, uint32 k)
{
	return (lo >> LION_OFFSET_BITS) * 64 * k + (lo & TEST_OFFSET_MASK);
}

/* The member bit i of a payload of width k stands for. */
static uint32
test_narrow_lo(uint32 i, uint32 k)
{
	return ((i / (64 * k)) << LION_OFFSET_BITS) | (i % (64 * k));
}

/* The narrowest width that holds every member of r (1 when it has none). */
static uint32
ref_width(const Ref *r)
{
	uint32		k = 1;
	uint32		i;

	for (i = 0; i < LION_CONTAINER_RANGE; i++)
		if (r->m[i])
			k = Max(k, test_lo_width(i));
	return k;
}

/* Does some NARROW hold every member of r? */
static bool
ref_narrow_ok(const Ref *r)
{
	return ref_width(r) <= TEST_KMAX;
}

/* A container's bitmap width: a NARROW's flags, a BITSET's full one, or 0. */
static uint32
test_width(const LionContainer *c)
{
	if (c->type == LION_CT_BITSET)
		return TEST_FULL;
	if (c->type == LION_CT_NARROW)
		return c->flags;
	return 0;
}

/* A width for a test: 1 .. LION_NARROW_MAX_WIDTH. */
static uint32
rand_width(void)
{
	return 1 + rng_below(TEST_KMAX);
}

/*
 * Write the NARROW encoding of r at width k (0: the narrowest that holds it)
 * directly, whatever optimize() would pick: for sets a NARROW is not the
 * smallest form of, which a NARROW can still be after the mutators have been
 * at it, and at widths wider than its members need, which VACUUM and the set
 * algebra may meet.
 */
static void
build_narrow_direct(CBuf *b, const Ref *r, uint32 k)
{
	uint64	   *w = TEST_NARROW_DATA(&b->c);
	uint32		i;

	if (k == 0)
		k = ref_width(r);
	CHECK(k >= 1 && k <= TEST_KMAX && ref_width(r) <= k,
		  "build_narrow_direct() given members a NARROW of its width holds");
	lion_container_init(&b->c, TEST_CKEY);
	b->c.type = LION_CT_NARROW;
	b->c.flags = (uint8) k;
	memset(w, 0, LION_WIDTH_BYTES(k));
	for (i = 0; i < LION_CONTAINER_RANGE; i++)
		if (r->m[i] && test_lo_fits(i, k))
		{
			uint32		bit = test_narrow_bit(i, k);

			w[bit >> 6] |= UINT64CONST(1) << (bit & 63);
		}
	b->c.cardinality = (uint16) r->card;
}

/* ----------------------------------------------------------------
 *							verification
 * ----------------------------------------------------------------
 */

static uint16 scratch_array[LION_CONTAINER_RANGE];
static uint16 scratch_iter[LION_CONTAINER_RANGE];
static uint64 scratch_img[LION_BITSET_WORDS];

/* Every bit block_mask() may set: the LION_BLOCKS_PER_CONTAINER blocks. */
#define TEST_BLOCK_BITS	(~UINT64CONST(0) >> (64 - LION_BLOCKS_PER_CONTAINER))

/* The block_mask() of a bitset image: the blocks with a bit set. */
static uint64
image_blocks(const uint64 *w)
{
	uint32		words = LION_BITSET_WORDS / LION_BLOCKS_PER_CONTAINER;
	uint64		mask = 0;
	uint32		b;
	uint32		k;

	for (b = 0; b < LION_BLOCKS_PER_CONTAINER; b++)
		for (k = 0; k < words; k++)
			if (w[b * words + k] != 0)
				mask |= UINT64CONST(1) << b;
	return mask;
}

/* The block of lo, which the caller has already masked into range. */
static uint64
lo_block_bit(uint32 lo)
{
	return UINT64CONST(1) << (lo >> LION_OFFSET_BITS);
}

typedef struct IterState
{
	uint32		n;
	uint32		limit;			/* stop after this many */
	uint16	   *out;
} IterState;

static bool
iter_cb(uint16 lo, void *arg)
{
	IterState  *st = (IterState *) arg;

	if (st->n < LION_CONTAINER_RANGE)
		st->out[st->n] = lo;
	st->n++;
	return st->n < st->limit;
}

/* Cheap per-operation checks. */
static void
verify_light(const LionContainer *c, const Ref *r)
{
	const char *msg = "?";

	CHECK(lion_container_size(c) <= LION_CONTAINER_MAX_SIZE, "size invariant");
	if (!lion_container_check(c, LION_CONTAINER_MAX_SIZE, &msg))
		CHECK(false, msg);
	else
		CHECK(true, "check");
	CHECK(lion_container_cardinality(c) == r->card, "cardinality vs reference");
	CHECK(c->ckey == TEST_CKEY, "ckey preserved");
	if (c->type == LION_CT_NARROW)
		CHECK(c->flags >= 1 && c->flags <= TEST_KMAX,
			  "a NARROW's flags are its width, 1 .. LION_NARROW_MAX_WIDTH");
	else
		CHECK(c->flags == 0, "flags zero");
}

/* Full comparison against the reference. */
static void
verify_full(const LionContainer *c, const Ref *r)
{
	uint32		i;
	uint32		n;
	uint32		k;
	uint32		bad;
	IterState	st;
	Size		expected;
	uint64		expect_blocks;

	verify_light(c, r);

	/* contains() for every possible member */
	bad = 0;
	for (i = 0; i < LION_CONTAINER_RANGE; i++)
	{
		nmember_cmps++;
		if (lion_container_contains(c, (uint16) i) != r->m[i])
			bad++;
	}
	CHECK(bad == 0, "contains() disagrees with the reference");

	/* to_array() */
	n = lion_container_to_array(c, scratch_array);
	CHECK(n == r->card, "to_array() count");
	k = 0;
	bad = 0;
	for (i = 0; i < LION_CONTAINER_RANGE && k < n; i++)
		if (r->m[i])
		{
			if (scratch_array[k] != (uint16) i)
				bad++;
			k++;
		}
	CHECK(bad == 0, "to_array() contents/order");
	for (i = 1; i < n; i++)
		if (scratch_array[i] <= scratch_array[i - 1])
			bad++;
	CHECK(bad == 0, "to_array() is strictly ascending");

	/* iterate() must produce exactly the same sequence */
	st.n = 0;
	st.limit = LION_CONTAINER_RANGE + 1;
	st.out = scratch_iter;
	lion_container_iterate(c, iter_cb, &st);
	CHECK(st.n == r->card, "iterate() visited the wrong number of members");
	bad = 0;
	for (i = 0; i < n && i < st.n; i++)
		if (scratch_iter[i] != scratch_array[i])
			bad++;
	CHECK(bad == 0, "iterate() order differs from to_array()");

	/*
	 * The count engine's two readers: or_into_bitset() into an image with
	 * bits of its own, which it has to keep, and block_mask(), against the
	 * blocks of the reference's members.
	 */
	memset(scratch_img, 0, sizeof(scratch_img));
	for (i = 0; i < LION_CONTAINER_RANGE; i += 61)
		scratch_img[i >> 6] |= UINT64CONST(1) << (i & 63);
	lion_container_or_into_bitset(c, scratch_img);
	bad = 0;
	expect_blocks = 0;
	for (i = 0; i < LION_CONTAINER_RANGE; i++)
	{
		bool		got = ((scratch_img[i >> 6] >> (i & 63)) & 1) != 0;

		if (got != (r->m[i] || i % 61 == 0))
			bad++;
		if (r->m[i])
			expect_blocks |= lo_block_bit(i);
	}
	CHECK(bad == 0, "or_into_bitset() ORs exactly the members into the image");
	CHECK(lion_container_block_mask(c) == expect_blocks,
		  "block_mask() names exactly the blocks with members");

	/* the size must be exactly what the representation implies */
	switch (c->type)
	{
		case LION_CT_ARRAY:
			expected = LION_CONTAINER_HDRSZ + (Size) r->card * sizeof(uint16);
			CHECK(r->card <= LION_ARRAY_MAX_CARD, "array over LION_ARRAY_MAX_CARD");
			break;
		case LION_CT_BITSET:
			expected = LION_CONTAINER_HDRSZ + LION_BITSET_BYTES;
			break;
		case LION_CT_NARROW:
			/* k words a block */
			expected = LION_CONTAINER_HDRSZ + (Size) c->flags *
				LION_BLOCKS_PER_CONTAINER * sizeof(uint64);
			CHECK(expected == LION_NARROW_SIZE(c->flags),
				  "a NARROW of width k is 8 + 512 * k bytes (at 8K)");
			CHECK(ref_width(r) <= c->flags,
				  "a NARROW of width k holds only members at offsets below 64 * k");
			break;
		default:
			expected = LION_CONTAINER_HDRSZ + sizeof(uint16) +
				(Size) LION_RUN_NRUNS(c) * sizeof(LionRun);
			CHECK(LION_RUN_NRUNS(c) == ref_nruns(r), "run count vs reference");
			break;
	}
	CHECK(lion_container_size(c) == expected, "lion_container_size()");
	CHECK(lion_container_min_width(c) == ref_width(r),
		  "min_width() is the narrowest width that holds every member");
}

/* Build a container from a reference with repeated add(). */
static void
build_by_add(CBuf *b, const Ref *r)
{
	uint32		i;

	lion_container_init(&b->c, TEST_CKEY);
	for (i = 0; i < LION_CONTAINER_RANGE; i++)
		if (r->m[i])
			(void) lion_container_add(&b->c, (uint16) i);
}

/* Build a container from a reference with the bulk builder. */
static void
build_by_append(CBuf *b, const Ref *r)
{
	uint32		i;

	lion_container_init(&b->c, TEST_CKEY);
	for (i = 0; i < LION_CONTAINER_RANGE; i++)
		if (r->m[i])
			lion_container_append_sorted(&b->c, (uint16) i);
}

/*
 * Write the RUN encoding of a reference directly, whatever optimize() would
 * pick: for shapes where an ARRAY is as small or smaller, which the mutators
 * can still meet as a RUN (a RUN that grew by insertion is never optimized).
 */
static void
build_run_direct(CBuf *b, const Ref *r)
{
	LionRun    *runs = LION_RUN_DATA(&b->c);
	uint32		n = 0;
	uint32		i;

	lion_container_init(&b->c, TEST_CKEY);
	b->c.type = LION_CT_RUN;
	for (i = 0; i < LION_CONTAINER_RANGE; i++)
	{
		if (!r->m[i])
			continue;
		if (i > 0 && r->m[i - 1])
			runs[n - 1].len_minus_1++;
		else
		{
			runs[n].start = (uint16) i;
			runs[n].len_minus_1 = 0;
			n++;
		}
	}
	LION_RUN_NRUNS(&b->c) = (uint16) n;
	b->c.cardinality = (uint16) r->card;
}

/* Shared work objects (a Ref is 32KB, so keep them out of the stack). */
static Ref	ref_a;
static Ref	ref_b;
static Ref	ref_r;
static CBuf buf_a;
static CBuf buf_b;
static CBuf buf_d;
static CBuf buf_e;

/* ----------------------------------------------------------------
 *					deterministic transition tests
 * ----------------------------------------------------------------
 */

static void
test_empty(void)
{
	phase("empty container");
	ref_init(&ref_a);
	lion_container_init(&buf_a.c, TEST_CKEY);

	CHECK(buf_a.c.type == LION_CT_ARRAY, "init() produces an ARRAY");
	CHECK(buf_a.c.cardinality == 0, "init() cardinality is 0");
	CHECK(lion_container_size(&buf_a.c) == LION_CONTAINER_HDRSZ,
		  "empty container is 8 bytes");
	verify_full(&buf_a.c, &ref_a);

	CHECK(!lion_container_remove(&buf_a.c, 0), "remove() from empty is false");
	CHECK(!lion_container_remove(&buf_a.c, 32767), "remove() from empty is false");
	CHECK(!lion_container_contains(&buf_a.c, 12345), "contains() on empty");
	CHECK(lion_container_range_cardinality(&buf_a.c, 0, 32767) == 0,
		  "range_cardinality() on empty");
	CHECK(lion_container_remove_range(&buf_a.c, 0, 32767) == 0,
		  "remove_range() on empty");

	lion_container_optimize(&buf_a.c);
	CHECK(buf_a.c.type == LION_CT_ARRAY, "optimize() of empty picks ARRAY");
	CHECK(lion_container_size(&buf_a.c) == 8, "optimized empty is 8 bytes");
	verify_full(&buf_a.c, &ref_a);

	lion_container_to_bitset(&buf_a.c);
	CHECK(buf_a.c.type == LION_CT_BITSET, "to_bitset() forces BITSET");
	CHECK(lion_container_size(&buf_a.c) == LION_CONTAINER_MAX_SIZE,
		  "BITSET is always 4104 bytes");
	verify_full(&buf_a.c, &ref_a);

	lion_container_optimize(&buf_a.c);
	CHECK(buf_a.c.type == LION_CT_ARRAY, "optimize() returns an empty BITSET to ARRAY");
	verify_full(&buf_a.c, &ref_a);
}

static void
test_boundaries(void)
{
	phase("lo boundaries 0 and 32767");
	ref_init(&ref_a);
	lion_container_init(&buf_a.c, TEST_CKEY);

	CHECK(lion_container_add(&buf_a.c, 0), "add(0)");
	(void) ref_add(&ref_a, 0);
	CHECK(lion_container_size(&buf_a.c) == 10, "one-member ARRAY is 10 bytes");
	verify_full(&buf_a.c, &ref_a);

	CHECK(!lion_container_add(&buf_a.c, 0), "add(0) again is false");
	CHECK(lion_container_add(&buf_a.c, 32767), "add(32767)");
	(void) ref_add(&ref_a, 32767);
	CHECK(lion_container_size(&buf_a.c) == 12, "two-member ARRAY is 12 bytes");
	verify_full(&buf_a.c, &ref_a);

	CHECK(lion_container_range_cardinality(&buf_a.c, 0, 0) == 1, "range [0,0]");
	CHECK(lion_container_range_cardinality(&buf_a.c, 32767, 32767) == 1,
		  "range [32767,32767]");
	CHECK(lion_container_range_cardinality(&buf_a.c, 1, 32766) == 0,
		  "range [1,32766]");
	CHECK(lion_container_range_cardinality(&buf_a.c, 0, 32767) == 2,
		  "range [0,32767]");

	/* the same two boundary members as a BITSET and as a RUN */
	lion_container_to_bitset(&buf_a.c);
	verify_full(&buf_a.c, &ref_a);
	CHECK(lion_container_range_cardinality(&buf_a.c, 0, 32767) == 2,
		  "BITSET range [0,32767]");
	lion_container_optimize(&buf_a.c);
	CHECK(buf_a.c.type == LION_CT_ARRAY, "2 scattered members optimize to ARRAY");

	/* a RUN touching both ends */
	ref_init(&ref_a);
	lion_container_init(&buf_a.c, TEST_CKEY);
	(void) lion_container_add(&buf_a.c, 0);
	(void) lion_container_add(&buf_a.c, 1);
	(void) lion_container_add(&buf_a.c, 2);
	(void) lion_container_add(&buf_a.c, 32765);
	(void) lion_container_add(&buf_a.c, 32766);
	(void) lion_container_add(&buf_a.c, 32767);
	(void) ref_add(&ref_a, 0);
	(void) ref_add(&ref_a, 1);
	(void) ref_add(&ref_a, 2);
	(void) ref_add(&ref_a, 32765);
	(void) ref_add(&ref_a, 32766);
	(void) ref_add(&ref_a, 32767);
	lion_container_optimize(&buf_a.c);
	CHECK(buf_a.c.type == LION_CT_RUN, "two 3-member runs optimize to RUN");
	CHECK(LION_RUN_NRUNS(&buf_a.c) == 2, "two runs");
	CHECK(lion_container_size(&buf_a.c) == 8 + 2 + 8, "2-run container is 18 bytes");
	verify_full(&buf_a.c, &ref_a);

	CHECK(lion_container_remove(&buf_a.c, 32767), "remove(32767) from a RUN");
	(void) ref_remove(&ref_a, 32767);
	verify_full(&buf_a.c, &ref_a);
	CHECK(lion_container_add(&buf_a.c, 32767), "add(32767) back");
	(void) ref_add(&ref_a, 32767);
	verify_full(&buf_a.c, &ref_a);
}

static void
test_array_bitset_transition(void)
{
	uint32		i;

	phase("ARRAY <-> BITSET at 2048/2049");
	ref_init(&ref_a);
	lion_container_init(&buf_a.c, TEST_CKEY);

	/* 2048 scattered members: the largest legal ARRAY */
	for (i = 0; i < LION_ARRAY_MAX_CARD; i++)
	{
		CHECK(lion_container_add(&buf_a.c, (uint16) (i * 3)), "add");
		(void) ref_add(&ref_a, i * 3);
	}
	CHECK(buf_a.c.type == LION_CT_ARRAY, "2048 members still fit in an ARRAY");
	CHECK(buf_a.c.cardinality == LION_ARRAY_MAX_CARD, "cardinality 2048");
	CHECK(lion_container_size(&buf_a.c) == LION_CONTAINER_MAX_SIZE,
		  "2048-member ARRAY is 4104 bytes");
	verify_full(&buf_a.c, &ref_a);

	/* a duplicate must not trigger the conversion */
	CHECK(!lion_container_add(&buf_a.c, 0), "duplicate add on a full ARRAY");
	CHECK(buf_a.c.type == LION_CT_ARRAY, "duplicate add leaves the ARRAY alone");

	/* member 2049 forces BITSET */
	CHECK(lion_container_add(&buf_a.c, (uint16) (LION_ARRAY_MAX_CARD * 3)), "add 2049th");
	(void) ref_add(&ref_a, LION_ARRAY_MAX_CARD * 3);
	CHECK(buf_a.c.type == LION_CT_BITSET, "2049 members force BITSET");
	CHECK(buf_a.c.cardinality == LION_ARRAY_MAX_CARD + 1, "cardinality 2049");
	CHECK(lion_container_size(&buf_a.c) == LION_CONTAINER_MAX_SIZE, "BITSET is 4104");
	verify_full(&buf_a.c, &ref_a);

	/* removing back down to 2048 returns to ARRAY */
	CHECK(lion_container_remove(&buf_a.c, (uint16) (LION_ARRAY_MAX_CARD * 3)), "remove 2049th");
	(void) ref_remove(&ref_a, LION_ARRAY_MAX_CARD * 3);
	CHECK(buf_a.c.type == LION_CT_ARRAY, "2048 members return to ARRAY");
	CHECK(lion_container_size(&buf_a.c) == LION_CONTAINER_MAX_SIZE, "still 4104 bytes");
	verify_full(&buf_a.c, &ref_a);

	/* one more removal stays in ARRAY and shrinks */
	CHECK(lion_container_remove(&buf_a.c, 0), "remove(0)");
	(void) ref_remove(&ref_a, 0);
	CHECK(buf_a.c.type == LION_CT_ARRAY, "still ARRAY");
	CHECK(lion_container_size(&buf_a.c) == LION_CONTAINER_MAX_SIZE - 2, "4102 bytes");
	verify_full(&buf_a.c, &ref_a);

	/* the same transition through append_sorted */
	phase("append_sorted ARRAY -> BITSET");
	ref_init(&ref_a);
	lion_container_init(&buf_a.c, TEST_CKEY);
	for (i = 0; i <= LION_ARRAY_MAX_CARD; i++)
	{
		lion_container_append_sorted(&buf_a.c, (uint16) (i * 3));
		(void) ref_add(&ref_a, i * 3);
		if (i < LION_ARRAY_MAX_CARD)
			CHECK(buf_a.c.type == LION_CT_ARRAY, "append_sorted keeps ARRAY <= 2048");
		else
			CHECK(buf_a.c.type == LION_CT_BITSET, "append_sorted switches at 2049");
	}
	verify_full(&buf_a.c, &ref_a);
}

static void
test_run_merge_and_split(void)
{
	uint32		i;

	phase("RUN merge on add, split on remove");
	ref_init(&ref_a);
	lion_container_init(&buf_a.c, TEST_CKEY);

	/* [0..2] [4..6] [8..10] */
	for (i = 0; i <= 10; i++)
		if (i % 4 != 3)
		{
			(void) lion_container_add(&buf_a.c, (uint16) i);
			(void) ref_add(&ref_a, i);
		}
	lion_container_optimize(&buf_a.c);
	CHECK(buf_a.c.type == LION_CT_RUN, "three 3-member runs optimize to RUN");
	CHECK(LION_RUN_NRUNS(&buf_a.c) == 3, "3 runs");
	CHECK(lion_container_size(&buf_a.c) == 8 + 2 + 12, "3-run container is 22 bytes");
	verify_full(&buf_a.c, &ref_a);

	/* adding 3 merges runs 0 and 1 */
	CHECK(lion_container_add(&buf_a.c, 3), "add(3) into the gap");
	(void) ref_add(&ref_a, 3);
	CHECK(LION_RUN_NRUNS(&buf_a.c) == 2, "add merged two runs");
	verify_full(&buf_a.c, &ref_a);

	/* adding 7 merges the rest into one run */
	CHECK(lion_container_add(&buf_a.c, 7), "add(7) into the gap");
	(void) ref_add(&ref_a, 7);
	CHECK(LION_RUN_NRUNS(&buf_a.c) == 1, "add merged into a single run");
	CHECK(lion_container_size(&buf_a.c) == 8 + 2 + 4, "1-run container is 14 bytes");
	verify_full(&buf_a.c, &ref_a);

	/* removing from the middle splits */
	CHECK(lion_container_remove(&buf_a.c, 5), "remove(5) splits the run");
	(void) ref_remove(&ref_a, 5);
	CHECK(LION_RUN_NRUNS(&buf_a.c) == 2, "remove split one run into two");
	verify_full(&buf_a.c, &ref_a);

	/* removing the first member of the first run */
	CHECK(lion_container_remove(&buf_a.c, 0), "remove(0), run start");
	(void) ref_remove(&ref_a, 0);
	CHECK(LION_RUN_NRUNS(&buf_a.c) == 2, "run start removal keeps the run count");
	verify_full(&buf_a.c, &ref_a);

	/* removing the last member of the last run */
	CHECK(lion_container_remove(&buf_a.c, 10), "remove(10), run end");
	(void) ref_remove(&ref_a, 10);
	CHECK(LION_RUN_NRUNS(&buf_a.c) == 2, "run end removal keeps the run count");
	verify_full(&buf_a.c, &ref_a);

	/* shrink the second run to one member and then delete it */
	for (i = 6; i <= 9; i++)
	{
		if (!ref_remove(&ref_a, i))
			continue;
		CHECK(lion_container_remove(&buf_a.c, (uint16) i), "remove from the second run");
		verify_full(&buf_a.c, &ref_a);
	}
	CHECK(LION_RUN_NRUNS(&buf_a.c) == 1, "second run disappeared");

	/* drain the first run completely, reaching cardinality 0 */
	for (i = 1; i <= 4; i++)
		if (ref_remove(&ref_a, i))
		{
			CHECK(lion_container_remove(&buf_a.c, (uint16) i), "drain the run");
			verify_full(&buf_a.c, &ref_a);
		}
	CHECK(buf_a.c.cardinality == 0, "cardinality reached 0");
	CHECK(LION_RUN_NRUNS(&buf_a.c) == 0, "no runs left");
	CHECK(lion_container_size(&buf_a.c) == 10, "empty RUN container is 10 bytes");
}

static void
test_run_overflow(void)
{
	uint32		i;
	uint32		j;

	/* --- add that would need a 1024th run --- */
	phase("RUN add overflow -> BITSET");
	ref_init(&ref_a);
	lion_container_init(&buf_a.c, TEST_CKEY);
	for (i = 0; i < LION_RUN_MAX_NRUNS; i++)
		for (j = 0; j < 3; j++)
		{
			lion_container_append_sorted(&buf_a.c, (uint16) (6 * i + j));
			(void) ref_add(&ref_a, 6 * i + j);
		}
	lion_container_optimize(&buf_a.c);
	CHECK(buf_a.c.type == LION_CT_RUN, "1023 runs optimize to RUN");
	CHECK(LION_RUN_NRUNS(&buf_a.c) == LION_RUN_MAX_NRUNS, "1023 runs");
	CHECK(lion_container_size(&buf_a.c) == 8 + 2 + 4 * LION_RUN_MAX_NRUNS,
		  "1023-run container is 4102 bytes");
	verify_full(&buf_a.c, &ref_a);

	/* 4 has no neighbour, so it needs a new (1024th) run */
	CHECK(lion_container_add(&buf_a.c, 4), "add an isolated value to a full RUN");
	(void) ref_add(&ref_a, 4);
	CHECK(buf_a.c.type == LION_CT_BITSET, "run overflow converts to BITSET");
	verify_full(&buf_a.c, &ref_a);

	/* --- remove that would split into a 1024th run --- */
	phase("RUN split overflow -> BITSET");
	ref_init(&ref_a);
	lion_container_init(&buf_a.c, TEST_CKEY);
	for (i = 0; i < LION_RUN_MAX_NRUNS; i++)
		for (j = 0; j < 3; j++)
		{
			lion_container_append_sorted(&buf_a.c, (uint16) (4 * i + j));
			(void) ref_add(&ref_a, 4 * i + j);
		}
	lion_container_optimize(&buf_a.c);
	CHECK(buf_a.c.type == LION_CT_RUN, "1023 runs optimize to RUN");
	CHECK(LION_RUN_NRUNS(&buf_a.c) == LION_RUN_MAX_NRUNS, "1023 runs");
	verify_full(&buf_a.c, &ref_a);

	CHECK(lion_container_remove(&buf_a.c, 1), "remove the middle of a run");
	(void) ref_remove(&ref_a, 1);
	CHECK(buf_a.c.type == LION_CT_BITSET, "split overflow converts to BITSET");
	verify_full(&buf_a.c, &ref_a);

	/* --- remove_range that would split into a 1024th run --- */
	phase("RUN remove_range overflow -> BITSET");
	ref_init(&ref_a);
	lion_container_init(&buf_a.c, TEST_CKEY);
	for (i = 0; i < LION_RUN_MAX_NRUNS; i++)
		for (j = 0; j < 5; j++)
		{
			lion_container_append_sorted(&buf_a.c, (uint16) (7 * i + j));
			(void) ref_add(&ref_a, 7 * i + j);
		}
	lion_container_optimize(&buf_a.c);
	CHECK(buf_a.c.type == LION_CT_RUN, "1023 5-member runs optimize to RUN");
	CHECK(lion_container_remove_range(&buf_a.c, 2, 2) == 1,
		  "remove_range() of one interior value");
	(void) ref_remove_range(&ref_a, 2, 2);
	CHECK(buf_a.c.type == LION_CT_BITSET, "remove_range split overflow -> BITSET");
	verify_full(&buf_a.c, &ref_a);

	/*
	 * --- the same overflows when the members fit an ARRAY ---
	 *
	 * DESIGN.md §3: 20 runs of 10 are a 90-byte RUN; 1003 scattered inserts
	 * make it 1023 runs of 1203 members (4102 bytes), and the next isolated
	 * member needs a 1024th run.  1204 members fit an ARRAY of 2416 bytes,
	 * which is what the container must become - not a 4104-byte BITSET,
	 * which the insert path would then have kept (it does not optimize).
	 */
	phase("RUN add overflow -> ARRAY when the members fit one");
	ref_init(&ref_a);
	lion_container_init(&buf_a.c, TEST_CKEY);
	for (i = 0; i < 20; i++)
		for (j = 0; j < 10; j++)
		{
			(void) lion_container_add(&buf_a.c, (uint16) (i * 20 + j));
			(void) ref_add(&ref_a, i * 20 + j);
		}
	lion_container_optimize(&buf_a.c);
	CHECK(buf_a.c.type == LION_CT_RUN, "20 runs of 10 optimize to RUN");
	CHECK(lion_container_size(&buf_a.c) == 90, "20 runs of 10 are 90 bytes");
	for (i = 0; i < 1003; i++)
	{
		uint32		lo = 1000 + 2 * i;	/* isolated: every other value */

		CHECK(lion_container_add(&buf_a.c, (uint16) lo), "add a scattered member");
		(void) ref_add(&ref_a, lo);
		verify_light(&buf_a.c, &ref_a);
	}
	CHECK(buf_a.c.type == LION_CT_RUN, "still a RUN at 1023 runs");
	CHECK(LION_RUN_NRUNS(&buf_a.c) == LION_RUN_MAX_NRUNS, "1023 runs");
	CHECK(buf_a.c.cardinality == 1203, "1203 members");
	CHECK(lion_container_size(&buf_a.c) == 4102, "4102 bytes");
	verify_full(&buf_a.c, &ref_a);

	CHECK(lion_container_add(&buf_a.c, 30000), "the 1024th run");
	(void) ref_add(&ref_a, 30000);
	CHECK(buf_a.c.type == LION_CT_ARRAY, "run overflow with 1204 members converts to ARRAY");
	CHECK(lion_container_size(&buf_a.c) == 2416, "an ARRAY of 1204 members is 2416 bytes");
	verify_full(&buf_a.c, &ref_a);

	/*
	 * The boundary: 2047 members plus the new one still fit an ARRAY ...
	 * (1023 runs of 2, one of them of 3, four apart)
	 */
	phase("RUN add overflow at the ARRAY boundary");
	ref_init(&ref_a);
	for (i = 0; i < LION_RUN_MAX_NRUNS; i++)
	{
		uint32		len = (i < 1) ? 3 : 2;

		for (j = 0; j < len; j++)
			(void) ref_add(&ref_a, 4 * i + j);
	}
	build_run_direct(&buf_a, &ref_a);
	CHECK(ref_a.card == 2047, "1023 runs holding 2047 members");
	verify_full(&buf_a.c, &ref_a);
	CHECK(lion_container_add(&buf_a.c, 30000), "the 1024th run, 2048th member");
	(void) ref_add(&ref_a, 30000);
	CHECK(buf_a.c.type == LION_CT_ARRAY, "2048 members after the overflow: ARRAY");
	verify_full(&buf_a.c, &ref_a);

	/* ... and 2048 plus the new one do not */
	ref_init(&ref_a);
	for (i = 0; i < LION_RUN_MAX_NRUNS; i++)
	{
		uint32		len = (i < 2) ? 3 : 2;

		for (j = 0; j < len; j++)
			(void) ref_add(&ref_a, 4 * i + j);
	}
	build_run_direct(&buf_a, &ref_a);
	CHECK(ref_a.card == 2048, "1023 runs holding 2048 members");
	verify_full(&buf_a.c, &ref_a);
	CHECK(lion_container_add(&buf_a.c, 30000), "the 1024th run, 2049th member");
	(void) ref_add(&ref_a, 30000);
	CHECK(buf_a.c.type == LION_CT_BITSET, "2049 members after the overflow: BITSET");
	verify_full(&buf_a.c, &ref_a);

	/* a split that needs a 1024th run with few members goes to ARRAY too */
	phase("RUN split overflow -> ARRAY when the members fit one");
	ref_init(&ref_a);
	for (j = 0; j < 3; j++)
		(void) ref_add(&ref_a, j);
	for (i = 1; i < LION_RUN_MAX_NRUNS; i++)
		(void) ref_add(&ref_a, 4 * i);
	build_run_direct(&buf_a, &ref_a);
	CHECK(LION_RUN_NRUNS(&buf_a.c) == LION_RUN_MAX_NRUNS,
		  "one run of 3 and 1022 singletons are 1023 runs");
	verify_full(&buf_a.c, &ref_a);
	CHECK(lion_container_remove(&buf_a.c, 1), "remove the middle of the run of 3");
	(void) ref_remove(&ref_a, 1);
	CHECK(buf_a.c.type == LION_CT_ARRAY, "the split overflow leaves an ARRAY of 1024");
	verify_full(&buf_a.c, &ref_a);

	/* 2049 members: the split goes through a BITSET, which shrinks to ARRAY */
	ref_init(&ref_a);
	for (i = 0; i < LION_RUN_MAX_NRUNS; i++)
	{
		uint32		len = (i < 3) ? 3 : 2;

		for (j = 0; j < len; j++)
			(void) ref_add(&ref_a, 4 * i + j);
	}
	build_run_direct(&buf_a, &ref_a);
	CHECK(ref_a.card == 2049, "1023 runs holding 2049 members");
	verify_full(&buf_a.c, &ref_a);
	CHECK(lion_container_remove(&buf_a.c, 1), "remove the middle of the run of 3");
	(void) ref_remove(&ref_a, 1);
	CHECK(buf_a.c.type == LION_CT_ARRAY, "2048 members after the split: ARRAY");
	verify_full(&buf_a.c, &ref_a);
}

static void
test_full_container(void)
{
	uint32		i;

	phase("full container (32768 members)");
	ref_init(&ref_a);
	lion_container_init(&buf_a.c, TEST_CKEY);
	for (i = 0; i < LION_CONTAINER_RANGE; i++)
	{
		lion_container_append_sorted(&buf_a.c, (uint16) i);
		(void) ref_add(&ref_a, i);
	}
	CHECK(buf_a.c.type == LION_CT_BITSET, "append_sorted ends in a BITSET");
	CHECK(buf_a.c.cardinality == LION_CONTAINER_RANGE, "cardinality 32768");
	verify_full(&buf_a.c, &ref_a);

	lion_container_optimize(&buf_a.c);
	CHECK(buf_a.c.type == LION_CT_RUN, "a full container optimizes to RUN");
	CHECK(LION_RUN_NRUNS(&buf_a.c) == 1, "a full container is one run");
	CHECK(lion_container_size(&buf_a.c) == 8 + 2 + 4,
		  "a full container is 14 bytes");
	verify_full(&buf_a.c, &ref_a);

	/* punch a hole and fill it again */
	CHECK(lion_container_remove(&buf_a.c, 16000), "remove(16000)");
	(void) ref_remove(&ref_a, 16000);
	CHECK(LION_RUN_NRUNS(&buf_a.c) == 2, "the hole split the run");
	CHECK(lion_container_size(&buf_a.c) == 8 + 2 + 8, "18 bytes");
	verify_full(&buf_a.c, &ref_a);

	CHECK(lion_container_add(&buf_a.c, 16000), "add(16000) back");
	(void) ref_add(&ref_a, 16000);
	CHECK(LION_RUN_NRUNS(&buf_a.c) == 1, "the runs merged again");
	verify_full(&buf_a.c, &ref_a);

	/* both ends */
	CHECK(lion_container_remove(&buf_a.c, 0), "remove(0)");
	(void) ref_remove(&ref_a, 0);
	CHECK(lion_container_remove(&buf_a.c, 32767), "remove(32767)");
	(void) ref_remove(&ref_a, 32767);
	CHECK(LION_RUN_NRUNS(&buf_a.c) == 1, "still one run");
	verify_full(&buf_a.c, &ref_a);

	CHECK(lion_container_range_cardinality(&buf_a.c, 0, 32767) == ref_a.card,
		  "range_cardinality() over everything");
	CHECK(lion_container_remove_range(&buf_a.c, 100, 200) == 101,
		  "remove_range() of 101 members");
	(void) ref_remove_range(&ref_a, 100, 200);
	CHECK(LION_RUN_NRUNS(&buf_a.c) == 2, "remove_range() split the run");
	verify_full(&buf_a.c, &ref_a);

	/* and empty it completely */
	CHECK(lion_container_remove_range(&buf_a.c, 0, 32767) == ref_a.card,
		  "remove_range() of everything");
	(void) ref_remove_range(&ref_a, 0, 32767);
	CHECK(buf_a.c.cardinality == 0, "container is empty");
	verify_full(&buf_a.c, &ref_a);
}

static void
test_optimize_choices(void)
{
	uint32		i;

	/* sparse: ARRAY is smallest */
	phase("optimize picks ARRAY for sparse data");
	ref_init(&ref_a);
	lion_container_init(&buf_a.c, TEST_CKEY);
	for (i = 0; i < 1000; i++)
	{
		lion_container_append_sorted(&buf_a.c, (uint16) (i * 7));
		(void) ref_add(&ref_a, i * 7);
	}
	lion_container_optimize(&buf_a.c);
	CHECK(buf_a.c.type == LION_CT_ARRAY, "1000 isolated members -> ARRAY");
	CHECK(lion_container_size(&buf_a.c) == 8 + 2000, "2008 bytes");
	verify_full(&buf_a.c, &ref_a);

	/* clustered: RUN is smallest */
	phase("optimize picks RUN for clustered data");
	ref_init(&ref_a);
	lion_container_init(&buf_a.c, TEST_CKEY);
	for (i = 0; i < 1000; i++)
	{
		uint32		lo = (i / 10) * 100 + (i % 10);

		lion_container_append_sorted(&buf_a.c, (uint16) lo);
		(void) ref_add(&ref_a, lo);
	}
	lion_container_optimize(&buf_a.c);
	CHECK(buf_a.c.type == LION_CT_RUN, "100 runs of 10 -> RUN");
	CHECK(LION_RUN_NRUNS(&buf_a.c) == 100, "100 runs");
	CHECK(lion_container_size(&buf_a.c) == 8 + 2 + 400, "410 bytes");
	verify_full(&buf_a.c, &ref_a);

	/* scattered and too big for either ARRAY or RUN: BITSET */
	phase("optimize picks BITSET for scattered dense data");
	ref_init(&ref_a);
	lion_container_init(&buf_a.c, TEST_CKEY);
	for (i = 0; i < 3000; i++)
	{
		lion_container_append_sorted(&buf_a.c, (uint16) (i * 5));
		(void) ref_add(&ref_a, i * 5);
	}
	lion_container_optimize(&buf_a.c);
	CHECK(buf_a.c.type == LION_CT_BITSET, "3000 isolated members -> BITSET");
	CHECK(lion_container_size(&buf_a.c) == LION_CONTAINER_MAX_SIZE, "4104 bytes");
	verify_full(&buf_a.c, &ref_a);

	/* exact tie between ARRAY (8+2c) and RUN (10+4r): prefer ARRAY */
	phase("optimize tie-break prefers ARRAY over RUN");
	ref_init(&ref_a);
	lion_container_init(&buf_a.c, TEST_CKEY);
	{
		static const uint16 tie[] = {0, 1, 2, 10, 11};

		for (i = 0; i < lengthof(tie); i++)
		{
			lion_container_append_sorted(&buf_a.c, tie[i]);
			(void) ref_add(&ref_a, tie[i]);
		}
	}
	lion_container_optimize(&buf_a.c);
	CHECK(buf_a.c.type == LION_CT_ARRAY, "5 members in 2 runs tie at 18 bytes -> ARRAY");
	CHECK(lion_container_size(&buf_a.c) == 18, "18 bytes");
	verify_full(&buf_a.c, &ref_a);
}

/* ----------------------------------------------------------------
 *						data generators
 * ----------------------------------------------------------------
 */

/* n random members, no two adjacent, so the set optimizes to an ARRAY. */
static void
gen_sparse(Ref *r, uint32 n)
{
	ref_init(r);
	while (r->card < n)
	{
		uint32		lo = rng_below(LION_CONTAINER_RANGE);

		if (r->m[lo])
			continue;
		if (lo > 0 && r->m[lo - 1])
			continue;
		if (lo < LION_CONTAINER_RANGE - 1 && r->m[lo + 1])
			continue;
		(void) ref_add(r, lo);
	}
}

/* nruns random runs, long enough that the set optimizes to a RUN. */
static void
gen_runs(Ref *r, uint32 nruns, uint32 minlen, uint32 maxlen)
{
	uint32		i;
	uint32		j;

	ref_init(r);
	for (i = 0; i < nruns; i++)
	{
		uint32		len = minlen + rng_below(maxlen - minlen + 1);
		uint32		start = rng_below(LION_CONTAINER_RANGE - len);

		for (j = 0; j < len; j++)
			(void) ref_add(r, start + j);
	}
}

/* n random members, no shape constraints. */
static void
gen_random(Ref *r, uint32 n)
{
	ref_init(r);
	if (n >= LION_CONTAINER_RANGE)
	{
		uint32		i;

		for (i = 0; i < LION_CONTAINER_RANGE; i++)
			(void) ref_add(r, i);
		return;
	}
	while (r->card < n)
		(void) ref_add(r, rng_below(LION_CONTAINER_RANGE));
}

/*
 * n random members at offsets a NARROW of width k holds (n at most its
 * 64 * 64 * k bits), one of them at an offset only width k holds, so that k
 * is the narrowest width that holds them.
 */
static void
gen_narrow(Ref *r, uint32 n, uint32 k)
{
	ref_init(r);
	n = Min(n, (uint32) LION_BLOCKS_PER_CONTAINER * 64 * k);
	if (n > 0)
		(void) ref_add(r, (rng_below(LION_BLOCKS_PER_CONTAINER) << LION_OFFSET_BITS) |
					   (64 * (k - 1) + rng_below(64)));
	while (r->card < n)
		(void) ref_add(r, (rng_below(LION_BLOCKS_PER_CONTAINER) << LION_OFFSET_BITS) |
					   rng_below(64 * k));
}

/*
 * n of them, no two adjacent (n well below 64 * 32 * k), one at offset
 * 64 * k - 1: an ARRAY, or a NARROW of width k.
 */
static void
gen_narrow_sparse(Ref *r, uint32 n, uint32 k)
{
	ref_init(r);
	if (n > 0)
		(void) ref_add(r, (rng_below(LION_BLOCKS_PER_CONTAINER) << LION_OFFSET_BITS) |
					   (64 * k - 1));
	while (r->card < n)
	{
		uint32		lo = (rng_below(LION_BLOCKS_PER_CONTAINER) << LION_OFFSET_BITS) |
			rng_below(64 * k);

		if (r->m[lo])
			continue;
		if (lo > 0 && r->m[lo - 1])
			continue;
		if (lo < LION_CONTAINER_RANGE - 1 && r->m[lo + 1])
			continue;
		(void) ref_add(r, lo);
	}
}

/*
 * nruns runs at offsets a NARROW of width k holds, each inside one block;
 * with edges set, a block's runs end at offset 64 * k - 1 and the next
 * block's start at 0, which are adjacent bits of the payload and not
 * adjacent members.
 */
static void
gen_narrow_runs(Ref *r, uint32 nruns, uint32 minlen, uint32 maxlen, bool edges,
				uint32 k)
{
	uint32		i;
	uint32		j;

	maxlen = Min(maxlen, 64 * k);
	minlen = Min(minlen, maxlen);
	ref_init(r);
	for (i = 0; i < nruns; i++)
	{
		uint32		len = minlen + rng_below(maxlen - minlen + 1);
		uint32		blk = rng_below(LION_BLOCKS_PER_CONTAINER);
		uint32		start = rng_below(64 * k - len + 1);

		if (edges)
			start = (i % 2 == 0) ? 64 * k - len : 0;
		for (j = 0; j < len; j++)
			(void) ref_add(r, (blk << LION_OFFSET_BITS) | (start + j));
	}
}

/* Build a container of exactly the requested representation. */
static void
gen_typed(Ref *r, CBuf *b, LionContainerType t)
{
	switch (t)
	{
		case LION_CT_ARRAY:
			gen_sparse(r, 300 + rng_below(300));
			build_by_append(b, r);
			lion_container_optimize(&b->c);
			break;
		case LION_CT_RUN:
			gen_runs(r, 8 + rng_below(12), 150, 600);
			build_by_append(b, r);
			lion_container_optimize(&b->c);
			break;
		case LION_CT_BITSET:
			gen_random(r, 3000 + rng_below(6000));
			build_by_append(b, r);
			lion_container_to_bitset(&b->c);
			break;
		case LION_CT_NARROW:
			{
				/*
				 * Of a random width k: more than 256 * k members (at 8K) are
				 * no ARRAY as small, and at a density below a half no RUN.
				 */
				uint32		k = rand_width();

				gen_narrow(r, LION_WIDTH_ARRAY_CARD(k) + 50 +
						   rng_below(6 * LION_WIDTH_ARRAY_CARD(k)), k);
				build_by_append(b, r);
				lion_container_optimize(&b->c);
				CHECK(b->c.type != LION_CT_NARROW || b->c.flags == k,
					  "generator produced a NARROW of the width it asked for");
				break;
			}
		case LION_CT_SPARSE:
			/* not a container: see test/unit/sparse_test.c */
			CHECK(false, "gen_typed() asked for a sparse segment");
			break;
	}
	CHECK(b->c.type == t, "generator produced the requested representation");
}

/* Pick a random existing member, or LION_CONTAINER_RANGE if there is none. */
static uint32
ref_pick_member(const Ref *r)
{
	uint32		start;
	uint32		i;

	if (r->card == 0)
		return LION_CONTAINER_RANGE;
	start = rng_below(LION_CONTAINER_RANGE);
	for (i = 0; i < LION_CONTAINER_RANGE; i++)
	{
		uint32		lo = start + i;

		if (lo >= LION_CONTAINER_RANGE)
			lo -= LION_CONTAINER_RANGE;
		if (r->m[lo])
			return lo;
	}
	return LION_CONTAINER_RANGE;
}

/* ----------------------------------------------------------------
 *						append_sorted vs add
 * ----------------------------------------------------------------
 */

static void
test_append_matches_add(void)
{
	static const uint32 sizes[] = {0, 1, 2047, 2048, 2049, 5000, 20000};
	uint32		k;

	phase("append_sorted + optimize == add + optimize");
	for (k = 0; k < lengthof(sizes); k++)
	{
		gen_random(&ref_a, sizes[k]);

		build_by_add(&buf_a, &ref_a);
		lion_container_optimize(&buf_a.c);
		build_by_append(&buf_b, &ref_a);
		lion_container_optimize(&buf_b.c);

		CHECK(lion_container_size(&buf_a.c) == lion_container_size(&buf_b.c),
			  "append_sorted and add produce the same size");
		CHECK(buf_a.c.type == buf_b.c.type,
			  "append_sorted and add produce the same type");
		CHECK(memcmp(&buf_a.c, &buf_b.c, lion_container_size(&buf_a.c)) == 0,
			  "append_sorted and add produce identical bytes");
		verify_full(&buf_b.c, &ref_a);
	}

	/* the same for clustered data, where optimize lands on RUN */
	for (k = 0; k < 6; k++)
	{
		gen_runs(&ref_a, 5 + k * 40, 3, 40);

		build_by_add(&buf_a, &ref_a);
		lion_container_optimize(&buf_a.c);
		build_by_append(&buf_b, &ref_a);
		lion_container_optimize(&buf_b.c);

		CHECK(memcmp(&buf_a.c, &buf_b.c, lion_container_size(&buf_a.c)) == 0,
			  "clustered: append_sorted and add produce identical bytes");
		verify_full(&buf_b.c, &ref_a);
	}
}

/* ----------------------------------------------------------------
 *							remove_if
 * ----------------------------------------------------------------
 */

static bool
pred_mod3(uint16 lo, void *arg)
{
	return (lo % 3) == 0;
}

static bool
pred_even(uint16 lo, void *arg)
{
	return (lo % 2) == 0;
}

static bool
pred_all(uint16 lo, void *arg)
{
	return true;
}

static bool
pred_none(uint16 lo, void *arg)
{
	return false;
}

static uint32
ref_remove_if(Ref *r, bool (*pred) (uint16, void *))
{
	uint32		i;
	uint32		n = 0;

	for (i = 0; i < LION_CONTAINER_RANGE; i++)
		if (r->m[i] && pred((uint16) i, NULL))
		{
			r->m[i] = false;
			r->card--;
			n++;
		}
	return n;
}

static void
test_remove_if(void)
{
	static const LionContainerType types[] = {LION_CT_ARRAY, LION_CT_BITSET, LION_CT_RUN, LION_CT_NARROW};
	uint32		t;
	uint32		removed;
	uint32		expected;

	phase("remove_if over every representation");
	rng_seed(UINT64CONST(0x5EED0001));
	for (t = 0; t < lengthof(types); t++)
	{
		gen_typed(&ref_a, &buf_a, types[t]);
		verify_full(&buf_a.c, &ref_a);

		expected = ref_remove_if(&ref_a, pred_mod3);
		removed = lion_container_remove_if(&buf_a.c, pred_mod3, NULL);
		CHECK(removed == expected, "remove_if() removal count");
		verify_full(&buf_a.c, &ref_a);

		/* a second pass must remove nothing */
		removed = lion_container_remove_if(&buf_a.c, pred_mod3, NULL);
		CHECK(removed == 0, "remove_if() is idempotent");
		verify_full(&buf_a.c, &ref_a);

		/* a predicate that keeps everything */
		removed = lion_container_remove_if(&buf_a.c, pred_none, NULL);
		CHECK(removed == 0, "remove_if(false) removes nothing");
		verify_full(&buf_a.c, &ref_a);

		lion_container_optimize(&buf_a.c);
		verify_full(&buf_a.c, &ref_a);

		/* and one that removes everything */
		expected = ref_a.card;
		removed = lion_container_remove_if(&buf_a.c, pred_all, NULL);
		CHECK(removed == expected, "remove_if(true) empties the container");
		(void) ref_remove_if(&ref_a, pred_all);
		CHECK(buf_a.c.cardinality == 0, "cardinality 0 after remove_if(true)");
		verify_full(&buf_a.c, &ref_a);
		lion_container_optimize(&buf_a.c);
		verify_full(&buf_a.c, &ref_a);
	}

	/* a RUN shredded into more than 1023 runs must leave RUN behind */
	phase("remove_if shreds a RUN past LION_RUN_MAX_NRUNS");
	{
		uint32		i;
		uint32		j;

		ref_init(&ref_a);
		lion_container_init(&buf_a.c, TEST_CKEY);
		for (i = 0; i < LION_RUN_MAX_NRUNS; i++)
			for (j = 0; j < 5; j++)
			{
				lion_container_append_sorted(&buf_a.c, (uint16) (7 * i + j));
				(void) ref_add(&ref_a, 7 * i + j);
			}
		lion_container_optimize(&buf_a.c);
		CHECK(buf_a.c.type == LION_CT_RUN, "starts as a RUN");

		expected = ref_remove_if(&ref_a, pred_even);
		removed = lion_container_remove_if(&buf_a.c, pred_even, NULL);
		CHECK(removed == expected, "remove_if() removal count");
		CHECK(buf_a.c.type != LION_CT_RUN, "a shredded RUN leaves the RUN encoding");
		CHECK(lion_container_size(&buf_a.c) <= LION_CONTAINER_MAX_SIZE, "size invariant");
		verify_full(&buf_a.c, &ref_a);
	}
}

/* ----------------------------------------------------------------
 *						ranges over every type
 * ----------------------------------------------------------------
 */

static void
test_ranges(void)
{
	static const LionContainerType types[] = {LION_CT_ARRAY, LION_CT_BITSET, LION_CT_RUN, LION_CT_NARROW};
	uint32		t;
	uint32		i;

	phase("range_cardinality / remove_range");
	rng_seed(UINT64CONST(0x5EED0002));
	for (t = 0; t < lengthof(types); t++)
	{
		gen_typed(&ref_a, &buf_a, types[t]);
		verify_full(&buf_a.c, &ref_a);

		/* read-only range queries */
		for (i = 0; i < 200; i++)
		{
			uint32		s = rng_below(LION_CONTAINER_RANGE);
			uint32		e = rng_below(LION_CONTAINER_RANGE);

			if (s > e)
			{
				uint32		tmp = s;

				s = e;
				e = tmp;
			}
			CHECK(lion_container_range_cardinality(&buf_a.c, (uint16) s, (uint16) e) ==
				  ref_range_card(&ref_a, s, e), "range_cardinality()");
		}
		CHECK(lion_container_range_cardinality(&buf_a.c, 0, 32767) == ref_a.card,
			  "range_cardinality() over the whole range");
		CHECK(lion_container_range_cardinality(&buf_a.c, 5, 4) == 0,
			  "range_cardinality() of an empty range");
		CHECK(lion_container_remove_range(&buf_a.c, 5, 4) == 0,
			  "remove_range() of an empty range");

		/* destructive ranges, with a full verify after each */
		for (i = 0; i < 25 && ref_a.card > 0; i++)
		{
			uint32		s = rng_below(LION_CONTAINER_RANGE);
			uint32		width = rng_below(2000);
			uint32		e = s + width;
			uint32		expected;
			uint32		removed;

			if (e > 32767)
				e = 32767;
			expected = ref_range_card(&ref_a, s, e);
			removed = lion_container_remove_range(&buf_a.c, (uint16) s, (uint16) e);
			CHECK(removed == expected, "remove_range() removal count");
			(void) ref_remove_range(&ref_a, s, e);
			verify_full(&buf_a.c, &ref_a);
		}
	}

	/* single-value ranges at both boundaries */
	phase("boundary ranges");
	for (t = 0; t < lengthof(types); t++)
	{
		gen_typed(&ref_a, &buf_a, types[t]);
		(void) lion_container_add(&buf_a.c, 0);
		(void) ref_add(&ref_a, 0);
		(void) lion_container_add(&buf_a.c, 32767);
		(void) ref_add(&ref_a, 32767);
		verify_full(&buf_a.c, &ref_a);

		CHECK(lion_container_range_cardinality(&buf_a.c, 0, 0) == 1, "range [0,0]");
		CHECK(lion_container_range_cardinality(&buf_a.c, 32767, 32767) == 1,
			  "range [32767,32767]");
		CHECK(lion_container_remove_range(&buf_a.c, 0, 0) == 1, "remove_range [0,0]");
		(void) ref_remove_range(&ref_a, 0, 0);
		verify_full(&buf_a.c, &ref_a);
		CHECK(lion_container_remove_range(&buf_a.c, 32767, 32767) == 1,
			  "remove_range [32767,32767]");
		(void) ref_remove_range(&ref_a, 32767, 32767);
		verify_full(&buf_a.c, &ref_a);
	}
}

/* ----------------------------------------------------------------
 *							set algebra
 * ----------------------------------------------------------------
 */

#define OP_AND		0
#define OP_OR		1
#define OP_ANDNOT	2

static void
ref_binop(const Ref *a, const Ref *b, Ref *out, int op)
{
	uint32		i;

	ref_init(out);
	for (i = 0; i < LION_CONTAINER_RANGE; i++)
	{
		bool		v;

		switch (op)
		{
			case OP_AND:
				v = a->m[i] && b->m[i];
				break;
			case OP_OR:
				v = a->m[i] || b->m[i];
				break;
			default:
				v = a->m[i] && !b->m[i];
				break;
		}
		if (v)
			(void) ref_add(out, i);
	}
}

/*
 * The representation optimize_ext() chooses for r's members: the smallest of
 * DESIGN.md §3's and §38's sizes, ties going to ARRAY, then RUN, then
 * NARROW, then BITSET.  A NARROW is of the narrowest width that holds them,
 * 8 + 512 * k bytes at 8K.
 */
static uint8
ref_opt_type(const Ref *r, bool allow_narrow)
{
	uint32		nruns = ref_nruns(r);
	uint32		k = ref_width(r);
	Size		asz = (r->card <= LION_ARRAY_MAX_CARD) ?
		LION_CONTAINER_HDRSZ + (Size) r->card * sizeof(uint16) : SIZE_MAX;
	Size		rsz = (nruns <= LION_RUN_MAX_NRUNS) ?
		LION_CONTAINER_HDRSZ + sizeof(uint16) + (Size) nruns * sizeof(LionRun) : SIZE_MAX;
	Size		nsz = (allow_narrow && k <= TEST_KMAX) ?
		LION_CONTAINER_HDRSZ + (Size) k * LION_BLOCKS_PER_CONTAINER * sizeof(uint64) :
		SIZE_MAX;
	Size		bsz = LION_CONTAINER_MAX_SIZE;

	if (asz <= rsz && asz <= nsz && asz <= bsz)
		return LION_CT_ARRAY;
	if (rsz <= nsz && rsz <= bsz)
		return LION_CT_RUN;
	if (nsz <= bsz)
		return LION_CT_NARROW;
	return LION_CT_BITSET;
}

/* Is c in the representation, and a NARROW of the width, r optimizes to? */
static bool
ref_opt_ok(const LionContainer *c, const Ref *r, bool allow_narrow)
{
	if (c->type != ref_opt_type(r, allow_narrow))
		return false;
	return c->type != LION_CT_NARROW || c->flags == ref_width(r);
}

/* Run all four set operations on buf_a/buf_b and check them. */
static void
run_binops(void)
{
	Size		asz = lion_container_size(&buf_a.c);
	uint32		card;

	memcpy(&buf_e, &buf_a, asz);

	/* AND */
	ref_binop(&ref_a, &ref_b, &ref_r, OP_AND);
	card = lion_container_and(&buf_a.c, &buf_b.c, &buf_d.c);
	CHECK(card == ref_r.card, "and() returned the wrong cardinality");
	CHECK(buf_d.c.ckey == buf_a.c.ckey, "and() dest ckey");
	CHECK(ref_opt_ok(&buf_d.c, &ref_r, true),
		  "and() leaves the representation its members optimize to");
	verify_full(&buf_d.c, &ref_r);
	CHECK(lion_container_and_cardinality(&buf_a.c, &buf_b.c) == ref_r.card,
		  "and_cardinality() disagrees with and()");
	CHECK(lion_container_and_cardinality(&buf_b.c, &buf_a.c) == ref_r.card,
		  "and_cardinality() is not symmetric");

	/* OR */
	ref_binop(&ref_a, &ref_b, &ref_r, OP_OR);
	card = lion_container_or(&buf_a.c, &buf_b.c, &buf_d.c);
	CHECK(card == ref_r.card, "or() returned the wrong cardinality");
	CHECK(buf_d.c.ckey == buf_a.c.ckey, "or() dest ckey");
	CHECK(ref_opt_ok(&buf_d.c, &ref_r, true),
		  "or() leaves the representation its members optimize to");
	verify_full(&buf_d.c, &ref_r);

	/* ANDNOT both ways round */
	ref_binop(&ref_a, &ref_b, &ref_r, OP_ANDNOT);
	card = lion_container_andnot(&buf_a.c, &buf_b.c, &buf_d.c);
	CHECK(card == ref_r.card, "andnot() returned the wrong cardinality");
	CHECK(buf_d.c.ckey == buf_a.c.ckey, "andnot() dest ckey");
	CHECK(ref_opt_ok(&buf_d.c, &ref_r, true),
		  "andnot() leaves the representation its members optimize to");
	verify_full(&buf_d.c, &ref_r);

	ref_binop(&ref_b, &ref_a, &ref_r, OP_ANDNOT);
	card = lion_container_andnot(&buf_b.c, &buf_a.c, &buf_d.c);
	CHECK(card == ref_r.card, "reverse andnot() cardinality");
	verify_full(&buf_d.c, &ref_r);

	/*
	 * The unoptimized forms: the same members and cardinality, in whatever
	 * representation the operation built them in - which every reader takes.
	 */
	ref_binop(&ref_a, &ref_b, &ref_r, OP_AND);
	card = lion_container_and_raw(&buf_a.c, &buf_b.c, &buf_d.c);
	CHECK(card == ref_r.card, "and_raw() returned the wrong cardinality");
	verify_full(&buf_d.c, &ref_r);
	card = lion_container_and_raw(&buf_b.c, &buf_a.c, &buf_d.c);
	CHECK(card == ref_r.card, "and_raw() is not symmetric");
	verify_full(&buf_d.c, &ref_r);

	/*
	 * An ARRAY against an image of the other operand: the AND's count and
	 * block mask, without the AND (the grouped walk of the count engine)
	 */
	if (buf_a.c.type == LION_CT_ARRAY)
	{
		uint64		img[LION_BITSET_WORDS];
		uint64		blocks = ~UINT64CONST(0);
		uint64		expect_blocks = 0;
		uint32		i;

		memset(img, 0, sizeof(img));
		lion_container_or_into_bitset(&buf_b.c, img);
		card = lion_container_and_image_count(&buf_a.c, img, &blocks);
		CHECK(card == ref_r.card, "and_image_count() returned the wrong cardinality");
		for (i = 0; i < LION_CONTAINER_RANGE; i++)
			if (ref_r.m[i])
				expect_blocks |= lo_block_bit(i);
		CHECK(blocks == expect_blocks,
			  "and_image_count() names exactly the blocks of the AND");
	}

	ref_binop(&ref_a, &ref_b, &ref_r, OP_OR);
	card = lion_container_or_raw(&buf_a.c, &buf_b.c, &buf_d.c);
	CHECK(card == ref_r.card, "or_raw() returned the wrong cardinality");
	verify_full(&buf_d.c, &ref_r);
	lion_container_bitset_init(&buf_d.c, TEST_CKEY);
	lion_container_or_into_bitset(&buf_a.c, LION_BITSET_DATA(&buf_d.c));
	lion_container_or_into_bitset(&buf_b.c, LION_BITSET_DATA(&buf_d.c));
	card = lion_container_bitset_recount(&buf_d.c);
	CHECK(card == ref_r.card && buf_d.c.type == LION_CT_BITSET,
		  "bitset_init() and or_into_bitset() make the union, recount() its cardinality");
	verify_full(&buf_d.c, &ref_r);

	ref_binop(&ref_a, &ref_b, &ref_r, OP_ANDNOT);
	card = lion_container_andnot_raw(&buf_a.c, &buf_b.c, &buf_d.c);
	CHECK(card == ref_r.card, "andnot_raw() returned the wrong cardinality");
	verify_full(&buf_d.c, &ref_r);
	ref_binop(&ref_b, &ref_a, &ref_r, OP_ANDNOT);
	card = lion_container_andnot_raw(&buf_b.c, &buf_a.c, &buf_d.c);
	CHECK(card == ref_r.card, "reverse andnot_raw() cardinality");
	verify_full(&buf_d.c, &ref_r);

	/* the operands must not have been touched */
	CHECK(lion_container_size(&buf_a.c) == asz && memcmp(&buf_e, &buf_a, asz) == 0,
		  "a set operation modified its left operand");
}

static void
test_setops(void)
{
	static const LionContainerType types[] = {LION_CT_ARRAY, LION_CT_BITSET, LION_CT_RUN, LION_CT_NARROW};
	uint32		ta;
	uint32		tb;
	uint32		trial;

	phase("set algebra over all 16 type combinations");
	rng_seed(UINT64CONST(0x5EED0003));
	for (ta = 0; ta < lengthof(types); ta++)
		for (tb = 0; tb < lengthof(types); tb++)
			for (trial = 0; trial < 20; trial++)
			{
				gen_typed(&ref_a, &buf_a, types[ta]);
				gen_typed(&ref_b, &buf_b, types[tb]);
				run_binops();
			}
}

static void
test_setops_special(void)
{
	static const LionContainerType types[] = {LION_CT_ARRAY, LION_CT_BITSET, LION_CT_RUN, LION_CT_NARROW};
	uint32		t;
	uint32		i;

	phase("set algebra with empty, identical and disjoint operands");
	rng_seed(UINT64CONST(0x5EED0004));

	/* empty on either side, against every type */
	for (t = 0; t < lengthof(types); t++)
	{
		gen_typed(&ref_a, &buf_a, types[t]);
		ref_init(&ref_b);
		lion_container_init(&buf_b.c, TEST_CKEY);
		run_binops();

		ref_init(&ref_a);
		lion_container_init(&buf_a.c, TEST_CKEY);
		gen_typed(&ref_b, &buf_b, types[t]);
		run_binops();
	}

	/* both empty */
	ref_init(&ref_a);
	ref_init(&ref_b);
	lion_container_init(&buf_a.c, TEST_CKEY);
	lion_container_init(&buf_b.c, TEST_CKEY);
	run_binops();

	/* identical operands, in every representation */
	for (t = 0; t < lengthof(types); t++)
	{
		gen_typed(&ref_a, &buf_a, types[t]);
		memcpy(&ref_b, &ref_a, sizeof(Ref));
		memcpy(&buf_b, &buf_a, lion_container_size(&buf_a.c));
		run_binops();
	}

	/* disjoint halves */
	ref_init(&ref_a);
	ref_init(&ref_b);
	lion_container_init(&buf_a.c, TEST_CKEY);
	lion_container_init(&buf_b.c, TEST_CKEY);
	for (i = 0; i < LION_CONTAINER_RANGE; i++)
	{
		if (i < LION_CONTAINER_RANGE / 2)
		{
			lion_container_append_sorted(&buf_a.c, (uint16) i);
			(void) ref_add(&ref_a, i);
		}
		else
		{
			lion_container_append_sorted(&buf_b.c, (uint16) i);
			(void) ref_add(&ref_b, i);
		}
	}
	lion_container_optimize(&buf_a.c);
	lion_container_optimize(&buf_b.c);
	CHECK(buf_a.c.type == LION_CT_RUN && buf_b.c.type == LION_CT_RUN,
		  "half-full containers optimize to RUN");
	run_binops();

	/* two full containers */
	ref_init(&ref_a);
	lion_container_init(&buf_a.c, TEST_CKEY);
	for (i = 0; i < LION_CONTAINER_RANGE; i++)
	{
		lion_container_append_sorted(&buf_a.c, (uint16) i);
		(void) ref_add(&ref_a, i);
	}
	lion_container_optimize(&buf_a.c);
	memcpy(&ref_b, &ref_a, sizeof(Ref));
	memcpy(&buf_b, &buf_a, lion_container_size(&buf_a.c));
	run_binops();

	/* full against a single member, so AND leaves exactly one */
	ref_init(&ref_b);
	lion_container_init(&buf_b.c, TEST_CKEY);
	(void) lion_container_add(&buf_b.c, 4242);
	(void) ref_add(&ref_b, 4242);
	run_binops();

	/* a BITSET against a RUN spanning both boundaries of the container */
	phase("set algebra: BITSET against a RUN touching lo=0 and lo=32767");
	gen_random(&ref_a, 9000);
	build_by_append(&buf_a, &ref_a);
	lion_container_to_bitset(&buf_a.c);
	CHECK(buf_a.c.type == LION_CT_BITSET, "a is a BITSET");
	ref_init(&ref_b);
	lion_container_init(&buf_b.c, TEST_CKEY);
	for (i = 0; i < LION_CONTAINER_RANGE; i++)
	{
		lion_container_append_sorted(&buf_b.c, (uint16) i);
		(void) ref_add(&ref_b, i);
	}
	lion_container_optimize(&buf_b.c);
	CHECK(buf_b.c.type == LION_CT_RUN && LION_RUN_NRUNS(&buf_b.c) == 1,
		  "b is the full container as one run");
	run_binops();

	/* the same, but with the run stopping just short of each boundary */
	ref_init(&ref_b);
	lion_container_init(&buf_b.c, TEST_CKEY);
	for (i = 1; i < LION_CONTAINER_RANGE - 1; i++)
	{
		lion_container_append_sorted(&buf_b.c, (uint16) i);
		(void) ref_add(&ref_b, i);
	}
	lion_container_optimize(&buf_b.c);
	CHECK(buf_b.c.type == LION_CT_RUN, "b is one run inside the boundaries");
	run_binops();

	/* RUN x RUN whose intersection needs more than 1023 runs */
	phase("RUN x RUN intersection overflowing LION_RUN_MAX_NRUNS");
	ref_init(&ref_a);
	ref_init(&ref_b);
	lion_container_init(&buf_a.c, TEST_CKEY);
	lion_container_init(&buf_b.c, TEST_CKEY);
	for (i = 0; i < LION_CONTAINER_RANGE; i++)
	{
		/* a: [0..32767] minus every 8th value; b: [0..32767] minus every 8th+4 */
		if ((i % 8) != 0)
		{
			lion_container_append_sorted(&buf_a.c, (uint16) i);
			(void) ref_add(&ref_a, i);
		}
		if ((i % 8) != 4)
		{
			lion_container_append_sorted(&buf_b.c, (uint16) i);
			(void) ref_add(&ref_b, i);
		}
	}
	lion_container_optimize(&buf_a.c);
	lion_container_optimize(&buf_b.c);
	CHECK(buf_a.c.type == LION_CT_BITSET, "4096 runs cannot be a RUN container");
	run_binops();

	/* the same shape, but few enough runs to stay RUN encoded */
	ref_init(&ref_a);
	ref_init(&ref_b);
	lion_container_init(&buf_a.c, TEST_CKEY);
	lion_container_init(&buf_b.c, TEST_CKEY);
	for (i = 0; i < 500 * 8; i++)
	{
		if ((i % 8) != 0)
		{
			lion_container_append_sorted(&buf_a.c, (uint16) i);
			(void) ref_add(&ref_a, i);
		}
		if ((i % 8) != 4)
		{
			lion_container_append_sorted(&buf_b.c, (uint16) i);
			(void) ref_add(&ref_b, i);
		}
	}
	lion_container_optimize(&buf_a.c);
	lion_container_optimize(&buf_b.c);
	CHECK(buf_a.c.type == LION_CT_RUN && buf_b.c.type == LION_CT_RUN,
		  "500 runs stay RUN encoded");
	run_binops();
}

/* ----------------------------------------------------------------
 *					randomized mixed operations
 * ----------------------------------------------------------------
 */

/*
 * A member to add or remove: anywhere, or with a width k mostly one a NARROW
 * of that width holds (DESIGN.md §38), now and then one at the offsets just
 * past it, which widen it by a word a block, and now and then one anywhere.
 */
static uint32
random_op_lo(uint32 k)
{
	uint32		blk = rng_below(LION_BLOCKS_PER_CONTAINER) << LION_OFFSET_BITS;
	uint32		r = rng_below(80);

	if (k == 0 || r == 0)
		return rng_below(LION_CONTAINER_RANGE);
	if (r == 1)
		return blk | (64 * k + rng_below(64));
	return blk | rng_below(64 * k);
}

static void
test_random_ops(uint32 target, const char *label, uint32 nops, uint64 seed,
				uint32 k)
{
	uint32		i;

	phase(label);
	rng_seed(seed);
	if (k > 0)
		gen_narrow(&ref_a, target, k);
	else
		gen_random(&ref_a, target);
	build_by_add(&buf_a, &ref_a);
	lion_container_optimize(&buf_a.c);
	verify_full(&buf_a.c, &ref_a);

	for (i = 0; i < nops; i++)
	{
		uint32		op = rng_below(100);

		if (op < 30)
		{
			uint32		lo = random_op_lo(k);
			bool		expected = ref_add(&ref_a, lo);
			bool		got = lion_container_add(&buf_a.c, (uint16) lo);

			CHECK(expected == got, "add() return value");
			CHECK(lion_container_contains(&buf_a.c, (uint16) lo),
				  "the member is present after add()");
		}
		else if (op < 52)
		{
			uint32		lo = random_op_lo(k);
			bool		expected = ref_remove(&ref_a, lo);
			bool		got = lion_container_remove(&buf_a.c, (uint16) lo);

			CHECK(expected == got, "remove() return value");
			CHECK(!lion_container_contains(&buf_a.c, (uint16) lo),
				  "the member is gone after remove()");
		}
		else if (op < 70)
		{
			uint32		lo = ref_pick_member(&ref_a);

			if (lo < LION_CONTAINER_RANGE)
			{
				CHECK(lion_container_remove(&buf_a.c, (uint16) lo),
					  "remove() of a known member returns true");
				(void) ref_remove(&ref_a, lo);
			}
		}
		else if (op < 80)
		{
			lion_container_optimize(&buf_a.c);
			CHECK(ref_opt_ok(&buf_a.c, &ref_a, true),
				  "optimize() picks the reference's representation");
		}
		else if (op < 85)
			lion_container_to_bitset(&buf_a.c);
		else if (op < 93)
		{
			uint32		s = rng_below(LION_CONTAINER_RANGE);
			uint32		e = s + rng_below(4096);

			if (e > 32767)
				e = 32767;
			CHECK(lion_container_range_cardinality(&buf_a.c, (uint16) s, (uint16) e) ==
				  ref_range_card(&ref_a, s, e), "range_cardinality()");
		}
		else
		{
			uint32		s = rng_below(LION_CONTAINER_RANGE);
			uint32		e = s + rng_below(1024);
			uint32		expected;

			if (e > 32767)
				e = 32767;
			expected = ref_range_card(&ref_a, s, e);
			CHECK(lion_container_remove_range(&buf_a.c, (uint16) s, (uint16) e) == expected,
				  "remove_range() removal count");
			(void) ref_remove_range(&ref_a, s, e);
		}

		verify_light(&buf_a.c, &ref_a);
		if ((i % 500) == 499)
			verify_full(&buf_a.c, &ref_a);
	}
	verify_full(&buf_a.c, &ref_a);
}

/* ----------------------------------------------------------------
 *						remaining edge cases
 * ----------------------------------------------------------------
 */

static void
test_or_array_boundary(void)
{
	uint32		i;

	/*
	 * lion_container_or() may only use its ARRAY fast path while the two
	 * cardinalities together still fit in an ARRAY payload; exercise both
	 * sides of that boundary and well past it.
	 */
	phase("or() of two ARRAY operands around the 2048 boundary");

	/* exactly 1024 + 1024: the fast path fills the payload to the brim */
	ref_init(&ref_a);
	ref_init(&ref_b);
	lion_container_init(&buf_a.c, TEST_CKEY);
	lion_container_init(&buf_b.c, TEST_CKEY);
	for (i = 0; i < LION_ARRAY_MAX_CARD / 2; i++)
	{
		lion_container_append_sorted(&buf_a.c, (uint16) (4 * i));
		(void) ref_add(&ref_a, 4 * i);
		lion_container_append_sorted(&buf_b.c, (uint16) (4 * i + 2));
		(void) ref_add(&ref_b, 4 * i + 2);
	}
	lion_container_optimize(&buf_a.c);
	lion_container_optimize(&buf_b.c);
	CHECK(buf_a.c.type == LION_CT_ARRAY && buf_b.c.type == LION_CT_ARRAY,
		  "both operands are ARRAY containers");
	CHECK((uint32) buf_a.c.cardinality + buf_b.c.cardinality == LION_ARRAY_MAX_CARD,
		  "the cardinalities add up to exactly 2048");
	run_binops();

	/* one member more: the union no longer fits in an ARRAY payload */
	lion_container_append_sorted(&buf_b.c, (uint16) (4 * LION_ARRAY_MAX_CARD));
	(void) ref_add(&ref_b, 4 * LION_ARRAY_MAX_CARD);
	lion_container_optimize(&buf_b.c);
	CHECK(buf_b.c.type == LION_CT_ARRAY, "b is still an ARRAY");
	run_binops();

	/* and far past it: 1500 + 1500 interleaved members */
	ref_init(&ref_a);
	ref_init(&ref_b);
	lion_container_init(&buf_a.c, TEST_CKEY);
	lion_container_init(&buf_b.c, TEST_CKEY);
	for (i = 0; i < 1500; i++)
	{
		lion_container_append_sorted(&buf_a.c, (uint16) (2 * i));
		(void) ref_add(&ref_a, 2 * i);
		lion_container_append_sorted(&buf_b.c, (uint16) (2 * i + 1));
		(void) ref_add(&ref_b, 2 * i + 1);
	}
	lion_container_optimize(&buf_a.c);
	lion_container_optimize(&buf_b.c);
	CHECK(buf_a.c.type == LION_CT_ARRAY && buf_b.c.type == LION_CT_ARRAY,
		  "both operands are ARRAY containers");
	run_binops();

	/* the union of those two is [0..2999], a single run */
	(void) lion_container_or(&buf_a.c, &buf_b.c, &buf_d.c);
	CHECK(buf_d.c.type == LION_CT_RUN && LION_RUN_NRUNS(&buf_d.c) == 1,
		  "the interleaved union optimizes to a single run");
	CHECK(buf_d.c.cardinality == 3000, "3000 members");
}

static void
test_run_edge_cases(void)
{
	uint32		i;

	phase("RUN add/remove edge cases");

	/* [0..2] and [10..12] */
	ref_init(&ref_a);
	lion_container_init(&buf_a.c, TEST_CKEY);
	for (i = 0; i < 13; i++)
		if (i <= 2 || i >= 10)
		{
			lion_container_append_sorted(&buf_a.c, (uint16) i);
			(void) ref_add(&ref_a, i);
		}
	lion_container_optimize(&buf_a.c);
	CHECK(buf_a.c.type == LION_CT_RUN, "two runs optimize to RUN");
	CHECK(LION_RUN_NRUNS(&buf_a.c) == 2, "2 runs");
	verify_full(&buf_a.c, &ref_a);

	/* 9 is adjacent to the left edge of the second run only */
	CHECK(lion_container_add(&buf_a.c, 9), "add(9) extends a run to the left");
	(void) ref_add(&ref_a, 9);
	CHECK(LION_RUN_NRUNS(&buf_a.c) == 2, "extend-left keeps the run count");
	verify_full(&buf_a.c, &ref_a);

	/* removals that hit nothing */
	CHECK(!lion_container_remove(&buf_a.c, 5), "remove() inside a gap is false");
	CHECK(!lion_container_remove(&buf_a.c, 20), "remove() past the last run is false");
	CHECK(!lion_container_remove(&buf_a.c, 32767), "remove() at the top is false");
	verify_full(&buf_a.c, &ref_a);

	/* a single run, then inserts below it */
	phase("RUN insert below every run");
	ref_init(&ref_a);
	lion_container_init(&buf_a.c, TEST_CKEY);
	for (i = 10; i <= 20; i++)
	{
		lion_container_append_sorted(&buf_a.c, (uint16) i);
		(void) ref_add(&ref_a, i);
	}
	lion_container_optimize(&buf_a.c);
	CHECK(buf_a.c.type == LION_CT_RUN, "[10..20] is a RUN");
	CHECK(LION_RUN_NRUNS(&buf_a.c) == 1, "1 run");

	CHECK(!lion_container_remove(&buf_a.c, 0), "remove() below every run is false");
	CHECK(!lion_container_contains(&buf_a.c, 0), "contains(0) below every run");

	CHECK(lion_container_add(&buf_a.c, 5), "add(5) below every run");
	(void) ref_add(&ref_a, 5);
	CHECK(LION_RUN_NRUNS(&buf_a.c) == 2, "a new run was inserted first");
	verify_full(&buf_a.c, &ref_a);

	/* walk the gap shut from the right, then from the left */
	for (i = 9; i >= 7; i--)
	{
		CHECK(lion_container_add(&buf_a.c, (uint16) i), "close the gap from the right");
		(void) ref_add(&ref_a, i);
		verify_full(&buf_a.c, &ref_a);
	}
	CHECK(LION_RUN_NRUNS(&buf_a.c) == 2, "still two runs");
	CHECK(lion_container_add(&buf_a.c, 6), "add(6) merges the two runs");
	(void) ref_add(&ref_a, 6);
	CHECK(LION_RUN_NRUNS(&buf_a.c) == 1, "the runs merged");
	verify_full(&buf_a.c, &ref_a);

	/* append_sorted onto a RUN container (the defensive fallback) */
	phase("append_sorted onto a RUN container");
	lion_container_append_sorted(&buf_a.c, 100);
	(void) ref_add(&ref_a, 100);
	CHECK(lion_container_contains(&buf_a.c, 100), "append_sorted onto a RUN works");
	verify_full(&buf_a.c, &ref_a);
}

static void
test_iterate_early_stop(void)
{
	static const LionContainerType types[] = {LION_CT_ARRAY, LION_CT_BITSET, LION_CT_RUN, LION_CT_NARROW};
	uint32		t;
	uint32		k;

	phase("iterate() honours an early stop");
	rng_seed(UINT64CONST(0x5EED0006));
	for (t = 0; t < lengthof(types); t++)
	{
		IterState	st;

		gen_typed(&ref_a, &buf_a, types[t]);
		(void) lion_container_to_array(&buf_a.c, scratch_array);
		for (k = 1; k <= 5; k++)
		{
			uint32		x;

			st.n = 0;
			st.limit = k;
			st.out = scratch_iter;
			memset(scratch_iter, 0xFF, k * sizeof(uint16));
			lion_container_iterate(&buf_a.c, iter_cb, &st);
			CHECK(st.n == k, "iterate() stopped where the callback asked");
			for (x = 0; x < k; x++)
				CHECK(scratch_iter[x] == scratch_array[x],
					  "the truncated iteration matches the full one");
		}
		st.n = 0;
		st.limit = ref_a.card + 100;
		st.out = scratch_iter;
		lion_container_iterate(&buf_a.c, iter_cb, &st);
		CHECK(st.n == ref_a.card, "iterate() without an early stop sees everything");
	}
}

static void
test_gallop_intersection(void)
{
	uint32		i;

	phase("ARRAY x ARRAY with very different sizes (galloping)");
	rng_seed(UINT64CONST(0x5EED0005));

	for (i = 0; i < 4; i++)
	{
		/* small on the left half of the trials, on the right for the rest */
		gen_sparse((i & 1) ? &ref_b : &ref_a, 25 + rng_below(20));
		gen_sparse((i & 1) ? &ref_a : &ref_b, 1800 + rng_below(200));
		build_by_append(&buf_a, &ref_a);
		lion_container_optimize(&buf_a.c);
		build_by_append(&buf_b, &ref_b);
		lion_container_optimize(&buf_b.c);
		CHECK(buf_a.c.type == LION_CT_ARRAY && buf_b.c.type == LION_CT_ARRAY,
			  "both operands are ARRAY containers");
		run_binops();
	}

	/* a small array entirely above a large one: the gallop runs off the end */
	ref_init(&ref_a);
	ref_init(&ref_b);
	lion_container_init(&buf_a.c, TEST_CKEY);
	lion_container_init(&buf_b.c, TEST_CKEY);
	for (i = 0; i < 40; i++)
	{
		lion_container_append_sorted(&buf_a.c, (uint16) (30000 + i * 3));
		(void) ref_add(&ref_a, 30000 + i * 3);
	}
	for (i = 0; i < 2000; i++)
	{
		lion_container_append_sorted(&buf_b.c, (uint16) (i * 2));
		(void) ref_add(&ref_b, i * 2);
	}
	lion_container_optimize(&buf_a.c);
	lion_container_optimize(&buf_b.c);
	CHECK(buf_a.c.type == LION_CT_ARRAY && buf_b.c.type == LION_CT_ARRAY,
		  "both operands are ARRAY containers");
	run_binops();

	/* and entirely below it */
	ref_init(&ref_a);
	lion_container_init(&buf_a.c, TEST_CKEY);
	for (i = 0; i < 40; i++)
	{
		lion_container_append_sorted(&buf_a.c, (uint16) (i * 3));
		(void) ref_add(&ref_a, i * 3);
	}
	lion_container_optimize(&buf_a.c);
	run_binops();
}

/*
 * The ways an AND of an ARRAY meets the other side (lion_container.c, "how
 * two sides of an AND meet"): a merge of two small sides, a gallop or a run
 * search for a few members against many, and a bitset image of the larger
 * side for everything else - each on either side of its threshold, against
 * ARRAYs and against RUNs of short and long runs, few and many of them.
 */
static void
test_and_shapes(void)
{
	static const uint32 asizes[] = {4, 5, 9, 16, 20, 30, 44, 60, 150, 600, 2000};
	static const uint32 nrunss[] = {1, 3, 20, 44, 100, 400, 1000};
	uint32		i;
	uint32		j;
	uint32		k;

	phase("AND of an ARRAY: merged, galloped, searched and imaged");
	rng_seed(UINT64CONST(0x5EED000A));
	for (i = 0; i < lengthof(asizes); i++)
	{
		for (j = 0; j < lengthof(asizes); j++)
		{
			gen_random(&ref_a, asizes[i]);
			build_by_append(&buf_a, &ref_a);
			gen_random(&ref_b, asizes[j]);
			build_by_append(&buf_b, &ref_b);
			if (buf_a.c.type != LION_CT_ARRAY || buf_b.c.type != LION_CT_ARRAY)
				continue;
			run_binops();
		}
		for (k = 0; k < lengthof(nrunss); k++)
		{
			gen_random(&ref_a, asizes[i]);
			build_by_append(&buf_a, &ref_a);
			gen_runs(&ref_b, nrunss[k], 1 + rng_below(3), 4 + rng_below(20));
			build_run_direct(&buf_b, &ref_b);
			if (buf_a.c.type != LION_CT_ARRAY || buf_b.c.type != LION_CT_RUN)
				continue;
			run_binops();
		}
	}
}

static bool raw_in_range(const LionContainer *c);

/*
 * An ARRAY of one to three members against every kind of operand: and()
 * looks each member up in the other one instead of merging (the count
 * engine's commonest AND, a key's container of a row or two against a
 * filter's).  Members that hit and miss, at 0 and 32767, and at a run's
 * edges and just past them; the small operand on either side.
 */
/* copy() of a well-formed container of every type is the container, byte for byte. */
static void
test_copy(void)
{
	static const LionContainerType types[] = {LION_CT_ARRAY, LION_CT_BITSET, LION_CT_RUN, LION_CT_NARROW};
	uint32		t;
	uint32		trial;

	phase("copy() of a well-formed container");
	rng_seed(UINT64CONST(0x5EED0C09));
	for (t = 0; t < lengthof(types); t++)
		for (trial = 0; trial < 20; trial++)
		{
			Size		size;

			gen_typed(&ref_a, &buf_a, types[t]);
			size = lion_container_size(&buf_a.c);
			memset(buf_d.data, 0xA5, sizeof(buf_d.data));
			lion_container_copy(&buf_a.c, &buf_d.c);
			CHECK(memcmp(buf_a.data, buf_d.data, size) == 0,
				  "copy() of a well-formed container is byte for byte");
			CHECK(size == sizeof(buf_d.data) || (uint8) buf_d.data[size] == 0xA5,
				  "copy() of a well-formed container writes its size and no more");
		}
}

static void
test_and_probe(void)
{
	static const LionContainerType types[] = {LION_CT_ARRAY, LION_CT_BITSET, LION_CT_RUN, LION_CT_NARROW};
	uint32		t;
	uint32		trial;

	phase("ARRAY of 1-3 members x every type (member lookups)");
	rng_seed(UINT64CONST(0x5EED0006));
	for (t = 0; t < lengthof(types); t++)
		for (trial = 0; trial < 60; trial++)
		{
			Ref		   *big = (trial & 1) ? &ref_a : &ref_b;
			Ref		   *small = (trial & 1) ? &ref_b : &ref_a;
			CBuf	   *bigbuf = (trial & 1) ? &buf_a : &buf_b;
			CBuf	   *smallbuf = (trial & 1) ? &buf_b : &buf_a;
			uint32		want = 1 + rng_below(3);
			uint32		i;

			if (types[t] == LION_CT_ARRAY && trial % 3 == 0)
			{
				/* the thinnest ARRAYs too, where the lookup is a few steps */
				gen_sparse(big, 1 + rng_below(8));
				build_by_append(bigbuf, big);
				lion_container_optimize(&bigbuf->c);
			}
			else
				gen_typed(big, bigbuf, types[t]);

			ref_init(small);
			while (small->card < want)
			{
				uint32		lo;

				switch (rng_below(5))
				{
					case 0:
						lo = 0;
						break;
					case 1:
						lo = LION_CONTAINER_RANGE - 1;
						break;
					case 2:
						lo = ref_pick_member(big);
						if (lo >= LION_CONTAINER_RANGE)
							lo = rng_below(LION_CONTAINER_RANGE);
						break;
					case 3:
						/* just past a member: a run's end + 1, or a neighbour */
						lo = ref_pick_member(big);
						lo = (lo >= LION_CONTAINER_RANGE - 1) ? 0 : lo + 1;
						break;
					default:
						lo = rng_below(LION_CONTAINER_RANGE);
						break;
				}
				(void) ref_add(small, lo);
			}
			lion_container_init(&smallbuf->c, TEST_CKEY);
			for (i = 0; i < LION_CONTAINER_RANGE; i++)
				if (small->m[i])
					lion_container_append_sorted(&smallbuf->c, (uint16) i);
			CHECK(smallbuf->c.type == LION_CT_ARRAY &&
				  smallbuf->c.cardinality == want,
				  "the small operand is an ARRAY of 1-3 members");
			run_binops();
		}

	phase("damaged: an ARRAY of 1-3 members out of order, repeated, past 32767");
	{
		static const uint16 bad[][3] = {
			{7, 5, 9}, {5, 5, 9}, {40000, 5, 7}, {65535, 65535, 3}, {9, 40000, 40000}
		};
		uint32		k;

		for (k = 0; k < lengthof(bad); k++)
		{
			for (t = 0; t < lengthof(types); t++)
			{
				const LionContainer *o;
				const uint16 *arr;
				uint32		i;
				bool		ascending = true;

				/* the other operand holds every member, as it reads them */
				gen_typed(&ref_b, &buf_b, types[t]);
				for (i = 0; i < 3; i++)
					(void) ref_add(&ref_b, bad[k][i] & (LION_CONTAINER_RANGE - 1));
				if (types[t] == LION_CT_NARROW && !ref_narrow_ok(&ref_b))
					continue;	/* a member at offset 511: no NARROW holds it */
				if (types[t] == LION_CT_RUN)
					build_run_direct(&buf_b, &ref_b);
				else if (types[t] == LION_CT_NARROW)
					build_narrow_direct(&buf_b, &ref_b, 0);
				else
				{
					build_by_append(&buf_b, &ref_b);
					if (types[t] == LION_CT_BITSET)
						lion_container_to_bitset(&buf_b.c);
				}

				lion_container_init(&buf_a.c, TEST_CKEY);
				buf_a.c.cardinality = 3;
				for (i = 0; i < 3; i++)
					LION_ARRAY_DATA(&buf_a.c)[i] = bad[k][i];
				(void) lion_container_and(&buf_a.c, &buf_b.c, &buf_d.c);
				o = &buf_d.c;
				arr = (const uint16 *) LION_CONTAINER_PAYLOAD(o);
				for (i = 1; i < o->cardinality && i < 3; i++)
					if (arr[i] <= arr[i - 1])
						ascending = false;
				CHECK(o->type == LION_CT_ARRAY && o->cardinality <= 3 &&
					  o->ckey == TEST_CKEY && o->flags == 0,
					  "and() of a damaged small ARRAY is an ARRAY of at most 3");
				CHECK(raw_in_range(o) && ascending,
					  "... strictly ascending, and in range");
				(void) lion_container_and(&buf_b.c, &buf_a.c, &buf_d.c);
				CHECK(raw_in_range(&buf_d.c), "... either way round");
			}
		}
	}
}

static void
test_run_intersection_overflow(void)
{
	uint32		i;
	uint32		j;

	/*
	 * Two RUN containers whose intersection needs about 2 * 600 runs, so
	 * lion_container_and() has to abandon the run-wise path.
	 */
	phase("RUN x RUN intersection needing more than 1023 runs");
	ref_init(&ref_a);
	ref_init(&ref_b);
	lion_container_init(&buf_a.c, TEST_CKEY);
	lion_container_init(&buf_b.c, TEST_CKEY);
	for (i = 0; i < 600; i++)
		for (j = 0; j < 15; j++)
		{
			lion_container_append_sorted(&buf_a.c, (uint16) (20 * i + j));
			(void) ref_add(&ref_a, 20 * i + j);
		}
	for (i = 0; i < 600; i++)
		for (j = 0; j < 15; j++)
		{
			lion_container_append_sorted(&buf_b.c, (uint16) (20 * i + 10 + j));
			(void) ref_add(&ref_b, 20 * i + 10 + j);
		}
	lion_container_optimize(&buf_a.c);
	lion_container_optimize(&buf_b.c);
	CHECK(buf_a.c.type == LION_CT_RUN && LION_RUN_NRUNS(&buf_a.c) == 600,
		  "a is a 600-run container");
	CHECK(buf_b.c.type == LION_CT_RUN && LION_RUN_NRUNS(&buf_b.c) == 600,
		  "b is a 600-run container");
	run_binops();

	/* the same shape, small enough that the run-wise path succeeds */
	phase("RUN x RUN intersection fitting in 1023 runs");
	ref_init(&ref_a);
	ref_init(&ref_b);
	lion_container_init(&buf_a.c, TEST_CKEY);
	lion_container_init(&buf_b.c, TEST_CKEY);
	for (i = 0; i < 200; i++)
		for (j = 0; j < 15; j++)
		{
			lion_container_append_sorted(&buf_a.c, (uint16) (20 * i + j));
			(void) ref_add(&ref_a, 20 * i + j);
		}
	for (i = 0; i < 200; i++)
		for (j = 0; j < 15; j++)
		{
			lion_container_append_sorted(&buf_b.c, (uint16) (20 * i + 10 + j));
			(void) ref_add(&ref_b, 20 * i + 10 + j);
		}
	lion_container_optimize(&buf_a.c);
	lion_container_optimize(&buf_b.c);
	run_binops();
}

static bool
pred_below_5000(uint16 lo, void *arg)
{
	return lo < 5000;
}

static bool
pred_mod4_is_1(uint16 lo, void *arg)
{
	return (lo % 4) == 1;
}

static void
test_remove_if_rebuild(void)
{
	uint32		i;
	uint32		j;
	uint32		removed;
	uint32		expected;

	/* a RUN that stays a RUN: whole runs disappear, the rest are trimmed */
	phase("remove_if leaves a RUN encoded as a RUN");
	rng_seed(UINT64CONST(0x5EED0007));
	gen_runs(&ref_a, 20, 200, 600);
	build_by_append(&buf_a, &ref_a);
	lion_container_optimize(&buf_a.c);
	CHECK(buf_a.c.type == LION_CT_RUN, "starts as a RUN");
	expected = ref_remove_if(&ref_a, pred_below_5000);
	removed = lion_container_remove_if(&buf_a.c, pred_below_5000, NULL);
	CHECK(removed == expected, "remove_if() removal count");
	CHECK(buf_a.c.type == LION_CT_RUN, "a RUN that still fits stays a RUN");
	verify_full(&buf_a.c, &ref_a);

	/*
	 * A RUN shredded into 2046 runs, but only 2046 members: too many runs for
	 * a RUN container and few enough members for an ARRAY.
	 */
	phase("remove_if turns a shredded RUN into an ARRAY");
	ref_init(&ref_a);
	lion_container_init(&buf_a.c, TEST_CKEY);
	for (i = 0; i < LION_RUN_MAX_NRUNS; i++)
		for (j = 0; j < 3; j++)
		{
			lion_container_append_sorted(&buf_a.c, (uint16) (4 * i + j));
			(void) ref_add(&ref_a, 4 * i + j);
		}
	lion_container_optimize(&buf_a.c);
	CHECK(buf_a.c.type == LION_CT_RUN, "starts as a RUN of 1023 runs");
	expected = ref_remove_if(&ref_a, pred_mod4_is_1);
	removed = lion_container_remove_if(&buf_a.c, pred_mod4_is_1, NULL);
	CHECK(removed == expected, "remove_if() removal count");
	CHECK(ref_a.card == 2046, "2046 members are left");
	CHECK(buf_a.c.type == LION_CT_ARRAY, "2046 members in 2046 runs become an ARRAY");
	verify_full(&buf_a.c, &ref_a);

	/* remove_if() emptying a RUN container outright */
	phase("remove_if empties a RUN container");
	gen_runs(&ref_a, 12, 100, 400);
	build_by_append(&buf_a, &ref_a);
	lion_container_optimize(&buf_a.c);
	CHECK(buf_a.c.type == LION_CT_RUN, "starts as a RUN");
	expected = ref_a.card;
	removed = lion_container_remove_if(&buf_a.c, pred_all, NULL);
	CHECK(removed == expected, "remove_if(true) removed everything");
	(void) ref_remove_if(&ref_a, pred_all);
	CHECK(buf_a.c.cardinality == 0, "the container is empty");
	verify_full(&buf_a.c, &ref_a);
	lion_container_optimize(&buf_a.c);
	CHECK(buf_a.c.type == LION_CT_ARRAY, "the emptied container optimizes to ARRAY");
	verify_full(&buf_a.c, &ref_a);
}

/* ----------------------------------------------------------------
 *					lion_container_check() negatives
 * ----------------------------------------------------------------
 */

static void
expect_bad(LionContainer *c, Size avail, const char *what)
{
	const char *msg = NULL;
	bool		ok = lion_container_check(c, avail, &msg);

	CHECK(!ok, what);
	CHECK(!ok && msg != NULL, "check() must set *errmsg when it fails");
}

static void
test_check_rejects(void)
{
	const char *msg = NULL;
	uint32		i;

	phase("lion_container_check() accepts and rejects");

	/* ---- a valid ARRAY ---- */
	lion_container_init(&buf_a.c, TEST_CKEY);
	for (i = 0; i < 100; i++)
		lion_container_append_sorted(&buf_a.c, (uint16) (i * 5));
	CHECK(lion_container_check(&buf_a.c, LION_CONTAINER_MAX_SIZE, &msg),
		  "a valid ARRAY passes");
	CHECK(msg == NULL, "check() clears *errmsg on success");
	CHECK(lion_container_check(&buf_a.c, lion_container_size(&buf_a.c), &msg),
		  "an exactly-sized ARRAY passes");

	memcpy(&buf_b, &buf_a, LION_CONTAINER_MAX_SIZE);
	expect_bad(&buf_b.c, 4, "header does not fit in avail_bytes");
	expect_bad(&buf_b.c, lion_container_size(&buf_a.c) - 1,
			   "array does not fit in avail_bytes");

	buf_b.c.type = 0;
	expect_bad(&buf_b.c, LION_CONTAINER_MAX_SIZE, "container type 0");
	buf_b.c.type = 4;
	expect_bad(&buf_b.c, LION_CONTAINER_MAX_SIZE, "container type 4");

	memcpy(&buf_b, &buf_a, LION_CONTAINER_MAX_SIZE);
	buf_b.c.flags = 1;
	expect_bad(&buf_b.c, LION_CONTAINER_MAX_SIZE, "nonzero flags");

	memcpy(&buf_b, &buf_a, LION_CONTAINER_MAX_SIZE);
	buf_b.c.type = LION_CT_BITSET;
	buf_b.c.cardinality = 40000;
	expect_bad(&buf_b.c, LION_CONTAINER_MAX_SIZE, "cardinality above 32768");

	memcpy(&buf_b, &buf_a, LION_CONTAINER_MAX_SIZE);
	buf_b.c.cardinality = LION_ARRAY_MAX_CARD + 1;
	expect_bad(&buf_b.c, LION_CONTAINER_MAX_SIZE, "array cardinality above 2048");

	memcpy(&buf_b, &buf_a, LION_CONTAINER_MAX_SIZE);
	LION_ARRAY_DATA(&buf_b.c)[50] = 40000;
	expect_bad(&buf_b.c, LION_CONTAINER_MAX_SIZE, "array member above 32767");

	memcpy(&buf_b, &buf_a, LION_CONTAINER_MAX_SIZE);
	LION_ARRAY_DATA(&buf_b.c)[11] = LION_ARRAY_DATA(&buf_b.c)[10];
	expect_bad(&buf_b.c, LION_CONTAINER_MAX_SIZE, "array members not ascending");

	memcpy(&buf_b, &buf_a, LION_CONTAINER_MAX_SIZE);
	LION_ARRAY_DATA(&buf_b.c)[11] = 1;
	expect_bad(&buf_b.c, LION_CONTAINER_MAX_SIZE, "array members out of order");

	/* ---- a valid BITSET ---- */
	lion_container_to_bitset(&buf_a.c);
	CHECK(lion_container_check(&buf_a.c, LION_CONTAINER_MAX_SIZE, &msg),
		  "a valid BITSET passes");

	memcpy(&buf_b, &buf_a, LION_CONTAINER_MAX_SIZE);
	expect_bad(&buf_b.c, LION_CONTAINER_MAX_SIZE - 1,
			   "bitset does not fit in avail_bytes");
	buf_b.c.cardinality++;
	expect_bad(&buf_b.c, LION_CONTAINER_MAX_SIZE, "bitset cardinality too high");
	buf_b.c.cardinality -= 2;
	expect_bad(&buf_b.c, LION_CONTAINER_MAX_SIZE, "bitset cardinality too low");

	/* ---- a valid RUN ---- */
	lion_container_init(&buf_a.c, TEST_CKEY);
	for (i = 0; i < 40; i++)
	{
		uint32		k;

		for (k = 0; k < 6; k++)
			lion_container_append_sorted(&buf_a.c, (uint16) (i * 20 + k));
	}
	lion_container_optimize(&buf_a.c);
	CHECK(buf_a.c.type == LION_CT_RUN, "40 runs of 6 optimize to RUN");
	CHECK(lion_container_check(&buf_a.c, LION_CONTAINER_MAX_SIZE, &msg),
		  "a valid RUN passes");

	memcpy(&buf_b, &buf_a, LION_CONTAINER_MAX_SIZE);
	expect_bad(&buf_b.c, 9, "run header does not fit in avail_bytes");
	expect_bad(&buf_b.c, lion_container_size(&buf_a.c) - 1,
			   "run payload does not fit in avail_bytes");

	memcpy(&buf_b, &buf_a, LION_CONTAINER_MAX_SIZE);
	LION_RUN_NRUNS(&buf_b.c) = LION_RUN_MAX_NRUNS + 1;
	expect_bad(&buf_b.c, LION_CONTAINER_MAX_SIZE, "more than 1023 runs");

	memcpy(&buf_b, &buf_a, LION_CONTAINER_MAX_SIZE);
	LION_RUN_NRUNS(&buf_b.c) = 0;
	expect_bad(&buf_b.c, LION_CONTAINER_MAX_SIZE, "no runs but nonzero cardinality");

	memcpy(&buf_b, &buf_a, LION_CONTAINER_MAX_SIZE);
	LION_RUN_DATA(&buf_b.c)[39].start = 32760;
	LION_RUN_DATA(&buf_b.c)[39].len_minus_1 = 100;
	expect_bad(&buf_b.c, LION_CONTAINER_MAX_SIZE, "run extends past 32767");

	memcpy(&buf_b, &buf_a, LION_CONTAINER_MAX_SIZE);
	LION_RUN_DATA(&buf_b.c)[1].start = (uint16)
		(LION_RUN_DATA(&buf_b.c)[0].start + LION_RUN_DATA(&buf_b.c)[0].len_minus_1 + 1);
	expect_bad(&buf_b.c, LION_CONTAINER_MAX_SIZE, "adjacent runs are not merged");

	memcpy(&buf_b, &buf_a, LION_CONTAINER_MAX_SIZE);
	LION_RUN_DATA(&buf_b.c)[1].start = LION_RUN_DATA(&buf_b.c)[0].start;
	expect_bad(&buf_b.c, LION_CONTAINER_MAX_SIZE, "runs are not ascending");

	memcpy(&buf_b, &buf_a, LION_CONTAINER_MAX_SIZE);
	buf_b.c.cardinality++;
	expect_bad(&buf_b.c, LION_CONTAINER_MAX_SIZE, "run cardinality does not match");

	/* an empty ARRAY is valid in exactly 8 bytes */
	lion_container_init(&buf_b.c, TEST_CKEY);
	CHECK(lion_container_check(&buf_b.c, LION_CONTAINER_HDRSZ, &msg),
		  "an empty container fits in 8 bytes");

	/*
	 * ---- a NARROW of each width k: its 8 + 512 * k bytes, a count that is
	 * its payload's, and a width that is one of 1 .. LION_NARROW_MAX_WIDTH ----
	 */
	{
		uint32		k;

		for (k = 1; k <= TEST_KMAX; k++)
		{
			Size		sz = LION_NARROW_SIZE(k);
			uint32		f;

			gen_narrow(&ref_a, 300 * k, k);
			build_narrow_direct(&buf_a, &ref_a, k);
			CHECK(sz == LION_CONTAINER_HDRSZ + 512 * k * (LION_BLOCKS_PER_CONTAINER / 64) ||
				  LION_BLOCKS_PER_CONTAINER < 64,
				  "a NARROW of width k is 8 + 512 * k bytes at 8K");
			CHECK(lion_container_size(&buf_a.c) == sz &&
				  lion_container_size_for(LION_CT_NARROW, 9999, k) == sz,
				  "size() and size_for(NARROW, width) agree");
			CHECK(lion_container_check(&buf_a.c, sz, &msg),
				  "a NARROW is valid in exactly its size");
			expect_bad(&buf_a.c, sz - 1, "a NARROW does not fit in a byte less");
			memcpy(&buf_b, &buf_a, sz);
			buf_b.c.cardinality++;
			expect_bad(&buf_b.c, LION_CONTAINER_MAX_SIZE, "narrow cardinality does not match");
			buf_b.c.cardinality = 0;
			expect_bad(&buf_b.c, LION_CONTAINER_MAX_SIZE, "a NARROW claiming no members");

			/* a width this build does not have, whatever the bytes past it */
			for (f = 0; f < 4; f++)
			{
				uint32		bad = (f == 0) ? 0 : (f == 1) ? TEST_KMAX + 1 :
					(f == 2) ? TEST_FULL : 255;

				memcpy(&buf_b, &buf_a, sz);
				memset(buf_b.data + sz, 0, LION_CONTAINER_MAX_SIZE - sz);
				buf_b.c.flags = (uint8) bad;
				msg = NULL;
				CHECK(!lion_container_check(&buf_b.c, LION_CONTAINER_MAX_SIZE, &msg) &&
					  msg != NULL && strstr(msg, "width") != NULL,
					  "a NARROW of width 0, LION_NARROW_MAX_WIDTH + 1, the full width or 255 is rejected, as a width");
				CHECK(lion_container_size(&buf_b.c) <= LION_NARROW_SIZE(TEST_KMAX) &&
					  lion_container_size(&buf_b.c) >= LION_NARROW_SIZE(1),
					  "... and sized as a NARROW of a width it can have, never past the largest");
			}

			/*
			 * The same payload read at a neighbouring width: every bit is a
			 * legal member at any width, so it is the count, and the size,
			 * that tell.
			 */
			if (k > 1)
			{
				memcpy(&buf_b, &buf_a, sz);
				buf_b.c.flags = (uint8) (k - 1);
				CHECK(lion_container_check(&buf_b.c, LION_CONTAINER_MAX_SIZE, &msg) ==
					  (lion_container_cardinality(&buf_b.c) ==
					   lion_container_range_cardinality(&buf_b.c, 0, LION_CONTAINER_RANGE - 1)),
					  "a NARROW read a word a block narrower passes only with its count");
			}
			if (k < TEST_KMAX)
			{
				memcpy(&buf_b, &buf_a, sz);
				buf_b.c.flags = (uint8) (k + 1);
				expect_bad(&buf_b.c, sz, "a NARROW a word a block wider does not fit its item");
			}
		}

		/* every other kind's flags are zero */
		gen_typed(&ref_a, &buf_a, LION_CT_BITSET);
		buf_a.c.flags = 1;
		expect_bad(&buf_a.c, LION_CONTAINER_MAX_SIZE, "a BITSET with flags");
		buf_a.c.flags = (uint8) TEST_FULL;
		expect_bad(&buf_a.c, LION_CONTAINER_MAX_SIZE, "a BITSET with its width in its flags");
		gen_typed(&ref_a, &buf_a, LION_CT_RUN);
		buf_a.c.flags = 2;
		expect_bad(&buf_a.c, LION_CONTAINER_MAX_SIZE, "a RUN with flags");
	}
	gen_narrow(&ref_a, 1000, 2);
	build_narrow_direct(&buf_a, &ref_a, 2);
	memcpy(&buf_b, &buf_a, LION_CONTAINER_MAX_SIZE);
	buf_b.c.type = LION_CT_NARROW + 1;
	expect_bad(&buf_b.c, LION_CONTAINER_MAX_SIZE, "type 6 is no container");
	buf_b.c.type = 0;
	expect_bad(&buf_b.c, LION_CONTAINER_MAX_SIZE, "type 0 is no container");

	/* lion_container_size_for() agrees with lion_container_size() */
	CHECK(lion_container_size_for(LION_CT_ARRAY, 100, 0) == 8 + 200,
		  "size_for(ARRAY, 100)");
	CHECK(lion_container_size_for(LION_CT_BITSET, 9999, 0) == LION_CONTAINER_MAX_SIZE,
		  "size_for(BITSET)");
	CHECK(lion_container_size_for(LION_CT_RUN, 9999, 7) == 8 + 2 + 28,
		  "size_for(RUN, 7 runs)");
}

/* ----------------------------------------------------------------
 *				lion_container_check_offsets()
 *
 * MaxHeapTuplesPerPage of a server with this BLCKSZ, which the frontend
 * cannot include: (BLCKSZ - SizeOfPageHeaderData) /
 * (MAXALIGN(SizeofHeapTupleHeader) + sizeof(ItemIdData)), 291 at 8K.
 * ----------------------------------------------------------------
 */

#define TEST_MAXOFF		((uint32) ((BLCKSZ - 24) / (24 + 4)))

/* The lo of offset off of block blk (within the container's range). */
static uint16
tuple_lo(uint32 blk, uint32 off)
{
	return (uint16) ((blk << LION_OFFSET_BITS) | off);
}

static void
expect_offsets(LionContainer *c, bool good, const char *what)
{
	const char *msg = NULL;
	bool		ok;

	CHECK(lion_container_check(c, LION_CONTAINER_MAX_SIZE, &msg),
		  "the container is structurally sound");
	ok = lion_container_check_offsets(c, TEST_MAXOFF, &msg);
	CHECK(ok == good, what);
	CHECK(ok == (msg == NULL), "check_offsets() sets *errmsg exactly when it fails");
}

/* Build buf_a from lo values, add one more, and optimize to type. */
static void
offsets_case(int type, const uint16 *los, int n, int extra, const char *what,
			 bool good)
{
	int			i;

	lion_container_init(&buf_a.c, TEST_CKEY);
	for (i = 0; i < n; i++)
		lion_container_add(&buf_a.c, los[i]);
	if (extra >= 0)
		lion_container_add(&buf_a.c, (uint16) extra);
	lion_container_optimize(&buf_a.c);
	if (type == LION_CT_BITSET && buf_a.c.type != LION_CT_BITSET)
		lion_container_to_bitset(&buf_a.c);
	CHECK(buf_a.c.type == type, "the case has the representation it tests");
	expect_offsets(&buf_a.c, good, what);
}

static void
test_check_offsets(void)
{
	static uint16 los[LION_CONTAINER_RANGE];
	uint32		nblocks = LION_BLOCKS_PER_CONTAINER;
	uint32		maxlo = (1U << LION_OFFSET_BITS) - 1;
	int			n;
	uint32		b;
	uint32		o;
	int			iter;

	phase("lion_container_check_offsets()");

	CHECK(TEST_MAXOFF < (1U << LION_OFFSET_BITS),
		  "MaxHeapTuplesPerPage fits the offset bits");

	/* ARRAY: a few members of a few blocks, first and last offsets included */
	n = 0;
	los[n++] = tuple_lo(0, 1);
	los[n++] = tuple_lo(0, TEST_MAXOFF);
	los[n++] = tuple_lo(5, 17);
	los[n++] = tuple_lo(nblocks - 1, 1);
	offsets_case(LION_CT_ARRAY, los, n, -1, "an ARRAY of tuples passes", true);
	offsets_case(LION_CT_ARRAY, los, n, tuple_lo(0, 0),
				 "an ARRAY member at offset 0 is rejected", false);
	offsets_case(LION_CT_ARRAY, los, n, tuple_lo(1, 0),
				 "an ARRAY member at offset 0 of block 1 (lo 512 at 8K) is rejected",
				 false);
	offsets_case(LION_CT_ARRAY, los, n, tuple_lo(3, TEST_MAXOFF + 1),
				 "an ARRAY member past MaxHeapTuplesPerPage is rejected", false);
	offsets_case(LION_CT_ARRAY, los, n, tuple_lo(nblocks - 1, maxlo),
				 "an ARRAY member at the last lo of a block is rejected", false);

	/* BITSET: every tuple of every block, the most a container can hold */
	n = 0;
	for (b = 0; b < nblocks; b++)
		for (o = 1; o <= TEST_MAXOFF; o++)
			los[n++] = tuple_lo(b, o);
	offsets_case(LION_CT_BITSET, los, n, -1, "a BITSET of every tuple passes", true);
	offsets_case(LION_CT_BITSET, los, n, tuple_lo(0, 0),
				 "a BITSET member at offset 0 is rejected", false);
	offsets_case(LION_CT_BITSET, los, n, tuple_lo(1, 0),
				 "a BITSET member at offset 0 of block 1 is rejected", false);
	offsets_case(LION_CT_BITSET, los, n, tuple_lo(nblocks - 1, TEST_MAXOFF + 1),
				 "a BITSET member past MaxHeapTuplesPerPage is rejected", false);
	offsets_case(LION_CT_BITSET, los, n, tuple_lo(7, maxlo),
				 "a BITSET member at the last lo of a block is rejected", false);

	/* RUN: whole blocks' worth of tuples, one run each */
	n = 0;
	for (b = 2; b < 6; b++)
		for (o = 1; o <= TEST_MAXOFF; o++)
			los[n++] = tuple_lo(b, o);
	offsets_case(LION_CT_RUN, los, n, -1, "a RUN of whole blocks passes", true);
	offsets_case(LION_CT_RUN, los, n, tuple_lo(3, 0),
				 "a run that starts at offset 0 is rejected", false);
	offsets_case(LION_CT_RUN, los, n, tuple_lo(4, TEST_MAXOFF + 1),
				 "a run that ends past MaxHeapTuplesPerPage is rejected", false);
	offsets_case(LION_CT_RUN, los, n, tuple_lo(9, 0),
				 "a run of offset 0 alone is rejected", false);

	/*
	 * NARROW of each width k: the odd offsets of every block below 64 * k
	 * and no greater than MaxHeapTuplesPerPage, the last of them the largest
	 * maxoff that rejects nothing
	 */
	{
		uint32		k;

		for (k = 1; k <= TEST_KMAX; k++)
		{
			const char *msg = NULL;
			uint32		last = 0;

			n = 0;
			for (b = 0; b < nblocks; b++)
				for (o = 1; o < 64 * k && o <= TEST_MAXOFF; o += 2)
				{
					los[n++] = tuple_lo(b, o);
					last = o;
				}
			offsets_case(LION_CT_NARROW, los, n, -1, "a NARROW of tuples passes", true);
			CHECK(buf_a.c.flags == k, "the case is a NARROW of the width it tests");
			CHECK(lion_container_check_offsets(&buf_a.c, last, &msg),
				  "a NARROW passes a maxoff of its last offset");
			CHECK(!lion_container_check_offsets(&buf_a.c, last - 1, &msg) && msg != NULL,
				  "a NARROW member past maxoff is rejected");
			CHECK(!lion_container_check_offsets(&buf_a.c, 64 * (k - 1), &msg) && msg != NULL,
				  "a NARROW member in its last word a block past maxoff is rejected");
			offsets_case(LION_CT_NARROW, los, n, tuple_lo(0, 0),
						 "a NARROW member at offset 0 is rejected", false);
			offsets_case(LION_CT_NARROW, los, n, tuple_lo(nblocks - 1, 0),
						 "a NARROW member at offset 0 of the last block is rejected", false);
		}
	}

	/* a run from one block into the next, written by hand */
	lion_container_init(&buf_a.c, TEST_CKEY);
	for (o = 1; o <= 8; o++)
		lion_container_add(&buf_a.c, tuple_lo(2, o));
	lion_container_optimize(&buf_a.c);
	CHECK(buf_a.c.type == LION_CT_RUN && LION_RUN_NRUNS(&buf_a.c) == 1,
		  "eight members in a row are one RUN");
	LION_RUN_DATA(&buf_a.c)[0].start = tuple_lo(2, TEST_MAXOFF);
	LION_RUN_DATA(&buf_a.c)[0].len_minus_1 =
		(uint16) (tuple_lo(3, 1) - tuple_lo(2, TEST_MAXOFF));
	buf_a.c.cardinality = (uint16) (LION_RUN_DATA(&buf_a.c)[0].len_minus_1 + 1);
	expect_offsets(&buf_a.c, false, "a run across two blocks is rejected");

	/* the answer is "every member is a tuple", for any container */
	for (iter = 0; iter < 300; iter++)
	{
		uint32		nmem = 1 + rng_below(iter % 3 == 0 ? 3000 : 200);
		bool		expect = true;
		const char *msg = NULL;
		uint32		v;
		uint32		i;

		lion_container_init(&buf_a.c, TEST_CKEY);
		for (i = 0; i < nmem; i++)
		{
			uint32		blk = rng_below(nblocks);
			uint32		off = 1 + rng_below(TEST_MAXOFF);

			/* now and then one that is not a tuple: offset 0, or any */
			if (rng_below(50) == 0)
				off = rng_below(2) ? 0 : rng_below(maxlo + 1);

			lion_container_add(&buf_a.c, tuple_lo(blk, off));
		}
		if (iter % 2 == 0)
			lion_container_optimize(&buf_a.c);
		for (v = 0; v < LION_CONTAINER_RANGE; v++)
		{
			if (lion_container_contains(&buf_a.c, (uint16) v) &&
				((v & maxlo) == 0 || (v & maxlo) > TEST_MAXOFF))
				expect = false;
		}
		CHECK(lion_container_check_offsets(&buf_a.c, TEST_MAXOFF, &msg) == expect,
			  "check_offsets() agrees with the members, one by one");
	}
}

/* ----------------------------------------------------------------
 *			exact-size buffers (damaged containers, growth in place)
 *
 * A heap buffer of exactly the size under test, so that a build with
 * -fsanitize=address reports any access past it.  Without the sanitizer a
 * guard of LION_CONTAINER_MAX_SIZE bytes follows it - no function here writes
 * further than that past any buffer - and guard_ok() checks it is untouched.
 * ----------------------------------------------------------------
 */

#if defined(__SANITIZE_ADDRESS__)
#define UNIT_ASAN 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define UNIT_ASAN 1
#endif
#endif

/* a variable, not a macro: 0 would make the loops below "always false" */
#ifdef UNIT_ASAN
static Size guard_bytes = 0;
#else
static Size guard_bytes = LION_CONTAINER_MAX_SIZE;
#endif
#define GUARD_FILL	0xA5

static void *
exact_alloc(Size size)
{
	unsigned char *p = malloc(size + guard_bytes);
	Size		i;

	if (p == NULL)
	{
		printf("out of memory\n");
		exit(2);
	}
	for (i = 0; i < guard_bytes; i++)
		p[size + i] = GUARD_FILL;
	return p;
}

static bool
guard_ok(const void *p, Size size)
{
	const unsigned char *g = (const unsigned char *) p + size;
	Size		i;

	for (i = 0; i < guard_bytes; i++)
		if (g[i] != GUARD_FILL)
			return false;
	return true;
}

/* ----------------------------------------------------------------
 *				damaged containers (DESIGN.md §3)
 *
 * A container read from disk has passed a check of its header and size and
 * nothing else when the library works on it.  These hand the library
 * containers whose payload is anything at all, each in an exact
 * LION_CONTAINER_MAX_SIZE buffer, with every output array of exactly its
 * documented size.  What is checked is what the library promises for ANY
 * payload: no access outside a buffer, every lo it hands out below
 * LION_CONTAINER_RANGE and never more than LION_CONTAINER_RANGE of them, and
 * a container a mutator leaves behind no larger than LION_CONTAINER_MAX_SIZE.
 * Which members come out is unspecified.
 * ----------------------------------------------------------------
 */

typedef struct DamageIter
{
	uint32		n;
	uint32		bad;			/* lo values out of range */
	uint64		blocks;			/* the blocks of the lo values */
} DamageIter;

static bool
damage_iter_cb(uint16 lo, void *arg)
{
	DamageIter *st = (DamageIter *) arg;

	st->n++;
	if ((uint32) lo > LION_CONTAINER_RANGE - 1)
		st->bad++;
	st->blocks |= lo_block_bit(lo & (LION_CONTAINER_RANGE - 1));
	return true;
}

/*
 * Every member a container stores, read raw rather than through the library
 * (which masks what it reads): is it below LION_CONTAINER_RANGE?  And a
 * NARROW's width one it can have.  What a result of the set algebra has to
 * be, whatever its operands were, because a result is handed on - onto a
 * page, into another operation.
 */
static bool
raw_in_range(const LionContainer *c)
{
	uint32		i;

	if (c->type == LION_CT_NARROW)
		return c->flags >= 1 && c->flags <= TEST_KMAX;
	if (c->type == LION_CT_ARRAY)
	{
		const uint16 *arr = (const uint16 *) LION_CONTAINER_PAYLOAD(c);
		uint32		n = Min((uint32) c->cardinality, (uint32) LION_ARRAY_MAX_CARD);

		for (i = 0; i < n; i++)
			if ((uint32) arr[i] > LION_CONTAINER_RANGE - 1)
				return false;
	}
	else if (c->type == LION_CT_RUN)
	{
		const LionRun *runs = (const LionRun *) (LION_CONTAINER_PAYLOAD(c) + sizeof(uint16));
		uint32		n = Min((uint32) LION_RUN_NRUNS(c), (uint32) LION_RUN_MAX_NRUNS);

		for (i = 0; i < n; i++)
			if ((uint32) runs[i].start + runs[i].len_minus_1 > LION_CONTAINER_RANGE - 1)
				return false;
	}
	return true;
}

static uint64 damage_pred_seed;
static uint32 damage_pred_calls;

static bool
damage_pred(uint16 lo, void *arg)
{
	damage_pred_calls++;
	if ((uint32) lo > LION_CONTAINER_RANGE - 1)
		damage_pred_calls += 1000000;	/* flagged by the caller */
	return ((((uint32) lo * 2654435761U) ^ (uint32) damage_pred_seed) >> 29) == 0;
}

static LionContainer *dmg_a;
static LionContainer *dmg_b;
static LionContainer *dmg_dst;
static LionContainer *dmg_work;
static uint16 *dmg_out;
static uint64 *dmg_img;			/* LION_BITSET_BYTES */

/*
 * Run every reader over c (and c against other), and every mutator over a
 * copy of c, checking the promises above.  c and other are left unchanged.
 */
static void
damage_exercise(const LionContainer *c, const LionContainer *other)
{
	DamageIter	st;
	uint32		n;
	uint32		i;
	uint32		bad = 0;
	uint32		lo = rng_below(LION_CONTAINER_RANGE);
	uint32		s = rng_below(LION_CONTAINER_RANGE);
	uint32		e = s + rng_below(LION_CONTAINER_RANGE - s);
	uint64		mask;

	/* readers */
	(void) lion_container_contains(c, (uint16) lo);
	(void) lion_container_range_cardinality(c, (uint16) s, (uint16) e);

	st.n = 0;
	st.bad = 0;
	st.blocks = 0;
	lion_container_iterate(c, damage_iter_cb, &st);
	CHECK(st.n <= LION_CONTAINER_RANGE, "damaged: iterate() visits at most 32768 values");
	CHECK(st.bad == 0, "damaged: iterate() hands out only lo values in range");

	/*
	 * The count engine's readers.  An image of exactly LION_BITSET_BYTES, and
	 * a block mask that has every block iterate() hands out a member of - the
	 * members the engine queues for a heap recheck - and no bit past
	 * LION_BLOCKS_PER_CONTAINER, which the engine's per-block flags stop at.
	 */
	memset(dmg_img, 0, LION_BITSET_BYTES);
	lion_container_or_into_bitset(c, dmg_img);
	CHECK(guard_ok(dmg_img, LION_BITSET_BYTES),
		  "damaged: or_into_bitset() stays inside its 4096-byte image");
	mask = lion_container_block_mask(c);
	CHECK((mask & ~TEST_BLOCK_BITS) == 0,
		  "damaged: block_mask() sets no bit past LION_BLOCKS_PER_CONTAINER");
	CHECK((st.blocks & ~mask) == 0,
		  "damaged: block_mask() has every block iterate() hands out a member of");
	CHECK(mask == image_blocks(dmg_img),
		  "damaged: block_mask() names the blocks or_into_bitset() fills");

	n = lion_container_to_array(c, dmg_out);
	CHECK(n <= LION_CONTAINER_RANGE, "damaged: to_array() returns at most 32768 values");
	CHECK(n == st.n, "damaged: to_array() and iterate() agree on the count");
	for (i = 0; i < n && i < LION_CONTAINER_RANGE; i++)
		if ((uint32) dmg_out[i] > LION_CONTAINER_RANGE - 1)
			bad++;
	CHECK(bad == 0, "damaged: to_array() writes only lo values in range");
	CHECK(guard_ok(dmg_out, LION_CONTAINER_RANGE * sizeof(uint16)),
		  "damaged: to_array() stays inside its 32768-entry output");

	(void) lion_container_and_cardinality(c, other);
	(void) lion_container_and_cardinality(other, c);
	if (c->type == LION_CT_ARRAY)
	{
		uint64		blocks;

		n = lion_container_and_image_count(c, dmg_img, &blocks);
		CHECK((blocks & ~TEST_BLOCK_BITS) == 0 && n <= LION_ARRAY_MAX_CARD,
			  "damaged: and_image_count() counts at most the ARRAY's members, on its blocks");
	}

#define DAMAGE_SETOP(what, stmt) \
	do { \
		stmt; \
		CHECK(lion_container_size(dmg_dst) <= LION_CONTAINER_MAX_SIZE, \
			  "damaged: " what " result size"); \
		CHECK(raw_in_range(dmg_dst), \
			  "damaged: " what " result holds only members in range"); \
	} while (0)

	DAMAGE_SETOP("and()", (void) lion_container_and(c, other, dmg_dst));
	DAMAGE_SETOP("and()", (void) lion_container_and(other, c, dmg_dst));
	DAMAGE_SETOP("or()", (void) lion_container_or(c, other, dmg_dst));
	DAMAGE_SETOP("or()", (void) lion_container_or(other, c, dmg_dst));
	DAMAGE_SETOP("andnot()", (void) lion_container_andnot(c, other, dmg_dst));
	DAMAGE_SETOP("andnot()", (void) lion_container_andnot(other, c, dmg_dst));
	DAMAGE_SETOP("and_raw()", (void) lion_container_and_raw(c, other, dmg_dst));
	DAMAGE_SETOP("and_raw()", (void) lion_container_and_raw(other, c, dmg_dst));
	DAMAGE_SETOP("or_raw()", (void) lion_container_or_raw(c, other, dmg_dst));
	DAMAGE_SETOP("or_raw()", (void) lion_container_or_raw(other, c, dmg_dst));
	DAMAGE_SETOP("andnot_raw()", (void) lion_container_andnot_raw(c, other, dmg_dst));
	DAMAGE_SETOP("andnot_raw()", (void) lion_container_andnot_raw(other, c, dmg_dst));
	DAMAGE_SETOP("copy()", lion_container_copy(c, dmg_dst));
#undef DAMAGE_SETOP
	CHECK(guard_ok(dmg_dst, LION_CONTAINER_MAX_SIZE), "damaged: set algebra stays inside dest");

	/* mutators, each on a fresh copy */
#define DAMAGE_MUTATE(what, stmt) \
	do { \
		memcpy(dmg_work, c, LION_CONTAINER_MAX_SIZE); \
		stmt; \
		CHECK(lion_container_size(dmg_work) <= LION_CONTAINER_MAX_SIZE, \
			  "damaged: " what " leaves a container of at most 4104 bytes"); \
		CHECK(guard_ok(dmg_work, LION_CONTAINER_MAX_SIZE), \
			  "damaged: " what " stays inside its buffer"); \
		st.n = 0; \
		st.bad = 0; \
		st.blocks = 0; \
		lion_container_iterate(dmg_work, damage_iter_cb, &st); \
		CHECK(st.n <= LION_CONTAINER_RANGE && st.bad == 0, \
			  "damaged: " what " leaves a container that iterates in range"); \
	} while (0)

	DAMAGE_MUTATE("add()", (void) lion_container_add(dmg_work, (uint16) lo));
	DAMAGE_MUTATE("remove()", (void) lion_container_remove(dmg_work, (uint16) lo));
	DAMAGE_MUTATE("remove_range()",
				  (void) lion_container_remove_range(dmg_work, (uint16) s, (uint16) e));
	DAMAGE_MUTATE("optimize()", lion_container_optimize(dmg_work));
	DAMAGE_MUTATE("to_bitset()", lion_container_to_bitset(dmg_work));
	damage_pred_seed = rng_next();
	damage_pred_calls = 0;
	DAMAGE_MUTATE("remove_if()",
				  (void) lion_container_remove_if(dmg_work, damage_pred, NULL));
	CHECK(damage_pred_calls <= LION_CONTAINER_RANGE,
		  "damaged: remove_if() asks about at most 32768 lo values, all in range");
	DAMAGE_MUTATE("remove_if() then optimize()",
				  ((void) lion_container_remove_if(dmg_work, damage_pred, NULL),
				   lion_container_optimize(dmg_work)));
#undef DAMAGE_MUTATE
}

static void
test_damaged_reported(void)
{
	Ref		   *r = &ref_b;
	uint32		i;
	uint32		n;
	DamageIter	st;

	/*
	 * The three shapes a 2026-09 review found overflowing a buffer, with
	 * AddressSanitizer, in a standalone harness.
	 */
	phase("damaged: ARRAY member past 32767 against a BITSET");
	gen_typed(r, &buf_b, LION_CT_BITSET);
	memcpy(dmg_b, &buf_b, LION_CONTAINER_MAX_SIZE);
	lion_container_init(dmg_a, TEST_CKEY);
	dmg_a->cardinality = 1;
	LION_ARRAY_DATA(dmg_a)[0] = 32768;	/* bits_set() indexed word 512 */
	(void) lion_container_or(dmg_a, dmg_b, dmg_dst);
	CHECK(guard_ok(dmg_dst, LION_CONTAINER_MAX_SIZE), "or() stays inside dest");
	damage_exercise(dmg_a, dmg_b);
	LION_ARRAY_DATA(dmg_a)[0] = 65535;	/* ... and word 1023, 4 KB past it */
	(void) lion_container_or(dmg_a, dmg_b, dmg_dst);
	CHECK(guard_ok(dmg_dst, LION_CONTAINER_MAX_SIZE), "or() stays inside dest");
	damage_exercise(dmg_a, dmg_b);

	phase("damaged: RUN reaching past 32767, materialised");
	lion_container_init(dmg_a, TEST_CKEY);
	dmg_a->type = LION_CT_RUN;
	dmg_a->cardinality = 1;
	LION_RUN_NRUNS(dmg_a) = 1;
	LION_RUN_DATA(dmg_a)[0].start = 30000;
	LION_RUN_DATA(dmg_a)[0].len_minus_1 = 60000;
	n = lion_container_to_array(dmg_a, dmg_out);
	CHECK(n == LION_CONTAINER_RANGE - 30000, "a run clamped at 32767 yields 30000 .. 32767");
	CHECK(guard_ok(dmg_out, LION_CONTAINER_RANGE * sizeof(uint16)),
		  "to_array() stays inside its 32768-entry output");
	damage_exercise(dmg_a, dmg_b);

	phase("damaged: BITSET whose header understates its members, filtered");
	gen_random(r, 5000);
	build_by_append(&buf_a, r);
	lion_container_to_bitset(&buf_a.c);
	memcpy(dmg_a, &buf_a, LION_CONTAINER_MAX_SIZE);
	dmg_a->cardinality = 100;	/* container_shrink_bitset() trusted this */
	damage_pred_seed = 0;
	(void) lion_container_remove_if(dmg_a, damage_pred, NULL);
	CHECK(dmg_a->type == LION_CT_BITSET,
		  "a BITSET holding more than an ARRAY can is not made one");
	CHECK(guard_ok(dmg_a, LION_CONTAINER_MAX_SIZE), "remove_if() stays inside the container");
	memcpy(dmg_a, &buf_a, LION_CONTAINER_MAX_SIZE);
	dmg_a->cardinality = 100;
	lion_container_optimize(dmg_a);
	CHECK(dmg_a->type == LION_CT_BITSET, "nor does optimize() make it one");
	damage_exercise(dmg_a, dmg_b);

	/* overlapping runs: every value once, and never more than 32768 */
	phase("damaged: 1023 runs each covering the whole range");
	lion_container_init(dmg_a, TEST_CKEY);
	dmg_a->type = LION_CT_RUN;
	dmg_a->cardinality = 7;
	LION_RUN_NRUNS(dmg_a) = LION_RUN_MAX_NRUNS;
	for (i = 0; i < LION_RUN_MAX_NRUNS; i++)
	{
		LION_RUN_DATA(dmg_a)[i].start = 0;
		LION_RUN_DATA(dmg_a)[i].len_minus_1 = LION_CONTAINER_RANGE - 1;
	}
	n = lion_container_to_array(dmg_a, dmg_out);
	CHECK(n == LION_CONTAINER_RANGE, "overlapping runs yield each value once");
	st.n = 0;
	st.bad = 0;
	st.blocks = 0;
	lion_container_iterate(dmg_a, damage_iter_cb, &st);
	CHECK(st.n == LION_CONTAINER_RANGE, "and iterate() visits each value once");
	damage_exercise(dmg_a, dmg_b);

	/* a run that starts past the range is empty */
	phase("damaged: a run starting past 32767");
	lion_container_init(dmg_a, TEST_CKEY);
	dmg_a->type = LION_CT_RUN;
	dmg_a->cardinality = 3;
	LION_RUN_NRUNS(dmg_a) = 2;
	LION_RUN_DATA(dmg_a)[0].start = 10;
	LION_RUN_DATA(dmg_a)[0].len_minus_1 = 2;
	LION_RUN_DATA(dmg_a)[1].start = 40000;
	LION_RUN_DATA(dmg_a)[1].len_minus_1 = 5;
	CHECK(lion_container_to_array(dmg_a, dmg_out) == 3, "the run past the range is empty");
	damage_exercise(dmg_a, dmg_b);

	/* counts past the largest legal container, in an exact 4104-byte buffer */
	phase("damaged: ARRAY claiming 3000 members");
	for (i = 0; i < LION_CONTAINER_MAX_SIZE; i++)
		((char *) dmg_a)[i] = (char) rng_next();
	dmg_a->ckey = TEST_CKEY;
	dmg_a->type = LION_CT_ARRAY;
	dmg_a->flags = 0;
	dmg_a->cardinality = 3000;
	damage_exercise(dmg_a, dmg_b);
	damage_exercise(dmg_a, dmg_a);

	phase("damaged: RUN claiming 60000 runs");
	dmg_a->type = LION_CT_RUN;
	LION_RUN_NRUNS(dmg_a) = 60000;
	damage_exercise(dmg_a, dmg_b);
	damage_exercise(dmg_a, dmg_a);

	/* runs out of order, which remove_range()'s run arithmetic assumes */
	phase("damaged: RUN with runs out of order");
	lion_container_init(dmg_a, TEST_CKEY);
	dmg_a->type = LION_CT_RUN;
	dmg_a->cardinality = 100;
	LION_RUN_NRUNS(dmg_a) = LION_RUN_MAX_NRUNS;
	for (i = 0; i < LION_RUN_MAX_NRUNS; i++)
	{
		LION_RUN_DATA(dmg_a)[i].start = (uint16) (32000 - 31 * i);
		LION_RUN_DATA(dmg_a)[i].len_minus_1 = 3;
	}
	memcpy(dmg_work, dmg_a, LION_CONTAINER_MAX_SIZE);
	(void) lion_container_remove_range(dmg_work, 5000, 20000);
	CHECK(lion_container_size(dmg_work) <= LION_CONTAINER_MAX_SIZE &&
		  guard_ok(dmg_work, LION_CONTAINER_MAX_SIZE),
		  "remove_range() over unordered runs stays inside its buffer");
	damage_exercise(dmg_a, dmg_b);

	/* an ARRAY out of order, which remove_range()'s binary searches assume */
	phase("damaged: ARRAY with members out of order");
	lion_container_init(dmg_a, TEST_CKEY);
	dmg_a->cardinality = LION_ARRAY_MAX_CARD;
	for (i = 0; i < LION_ARRAY_MAX_CARD; i++)
		LION_ARRAY_DATA(dmg_a)[i] = (uint16) (65535 - 16 * i);
	memcpy(dmg_work, dmg_a, LION_CONTAINER_MAX_SIZE);
	(void) lion_container_remove_range(dmg_work, 100, 30000);
	CHECK(guard_ok(dmg_work, LION_CONTAINER_MAX_SIZE),
		  "remove_range() over an unordered array stays inside its buffer");
	damage_exercise(dmg_a, dmg_b);
}

/* Is bit lo of an image set? */
static bool
img_test(const uint64 *w, uint32 lo)
{
	return ((w[lo >> 6] >> (lo & 63)) & 1) != 0;
}

/* Number of bits set in an image. */
static uint32
img_card(const uint64 *w)
{
	uint32		n = 0;
	uint32		i;

	for (i = 0; i < LION_BITSET_WORDS; i++)
		n += (uint32) pg_popcount64(w[i]);
	return n;
}

/*
 * The count engine's union of k containers and its visibility-map mask
 * carried their own copies of or_into_bitset() and block_mask(), without the
 * library's masks, and a 2026-09-27 review found these shapes writing up to
 * 12 KiB past a 4 KiB image and shifting by 64 or more.  Both are the
 * library's now; this is what they must do with them.
 */
static void
test_damaged_count_readers(void)
{
	uint64		mask;
	uint64		expect;
	uint32		i;

	phase("damaged: ARRAY members past 32767, into an image and a block mask");
	lion_container_init(dmg_a, TEST_CKEY);
	dmg_a->cardinality = 3;
	LION_ARRAY_DATA(dmg_a)[0] = 7;
	LION_ARRAY_DATA(dmg_a)[1] = 32768 + 7000;
	LION_ARRAY_DATA(dmg_a)[2] = 65535;	/* word 1023 of a 512-word image */
	memset(dmg_img, 0, LION_BITSET_BYTES);
	lion_container_or_into_bitset(dmg_a, dmg_img);
	CHECK(guard_ok(dmg_img, LION_BITSET_BYTES),
		  "or_into_bitset() stays inside a 4096-byte image");
	CHECK(img_card(dmg_img) == 3 && img_test(dmg_img, 7) &&
		  img_test(dmg_img, 7000) && img_test(dmg_img, 32767),
		  "members past the range are masked into it, as iterate() takes them");
	mask = lion_container_block_mask(dmg_a);
	expect = lo_block_bit(7) | lo_block_bit(7000) | lo_block_bit(32767);
	CHECK(mask == expect, "block_mask() masks them the same way");
	damage_exercise(dmg_a, dmg_b);

	/* a full ARRAY of the largest uint16, which shifted by 127 at 8K */
	for (i = 0; i < LION_ARRAY_MAX_CARD; i++)
		LION_ARRAY_DATA(dmg_a)[i] = 65535;
	dmg_a->cardinality = LION_ARRAY_MAX_CARD;
	CHECK(lion_container_block_mask(dmg_a) == lo_block_bit(32767),
		  "block_mask() of 2048 members of 65535 is the last block");
	damage_exercise(dmg_a, dmg_b);

	phase("damaged: RUN past 32767, into an image and a block mask");
	lion_container_init(dmg_a, TEST_CKEY);
	dmg_a->type = LION_CT_RUN;
	dmg_a->cardinality = 9;
	LION_RUN_NRUNS(dmg_a) = 3;
	LION_RUN_DATA(dmg_a)[0].start = 100;
	LION_RUN_DATA(dmg_a)[0].len_minus_1 = 0;
	LION_RUN_DATA(dmg_a)[1].start = 30000;
	LION_RUN_DATA(dmg_a)[1].len_minus_1 = 65535;	/* to 95535 */
	LION_RUN_DATA(dmg_a)[2].start = 65535;			/* empty */
	LION_RUN_DATA(dmg_a)[2].len_minus_1 = 65535;
	memset(dmg_img, 0, LION_BITSET_BYTES);
	lion_container_or_into_bitset(dmg_a, dmg_img);
	CHECK(guard_ok(dmg_img, LION_BITSET_BYTES),
		  "or_into_bitset() of a run past the range stays inside the image");
	CHECK(img_card(dmg_img) == 1 + (LION_CONTAINER_RANGE - 30000) &&
		  img_test(dmg_img, 100) && img_test(dmg_img, 30000) &&
		  img_test(dmg_img, 32767),
		  "a run is clamped at 32767, and one starting past it is empty");
	expect = lo_block_bit(100);
	for (i = 30000 >> LION_OFFSET_BITS; i < LION_BLOCKS_PER_CONTAINER; i++)
		expect |= UINT64CONST(1) << i;
	CHECK(lion_container_block_mask(dmg_a) == expect,
		  "block_mask() clamps the runs the same way");
	damage_exercise(dmg_a, dmg_b);

	/* 1023 runs from 32767 to 98301 each, the most the clamp lets through */
	LION_RUN_NRUNS(dmg_a) = 65535;
	for (i = 0; i < LION_RUN_MAX_NRUNS; i++)
	{
		LION_RUN_DATA(dmg_a)[i].start = 32767;
		LION_RUN_DATA(dmg_a)[i].len_minus_1 = 65534;
	}
	memset(dmg_img, 0, LION_BITSET_BYTES);
	lion_container_or_into_bitset(dmg_a, dmg_img);
	CHECK(guard_ok(dmg_img, LION_BITSET_BYTES) && img_card(dmg_img) == 1,
		  "1023 runs past the range, claiming 65535, set lo 32767 alone");
	CHECK(lion_container_block_mask(dmg_a) == lo_block_bit(32767),
		  "and block_mask() the last block alone");
	damage_exercise(dmg_a, dmg_b);
}

/*
 * What the set algebra and the other mutators leave behind for operands the
 * 2026-09-27 review listed: a result never stores a member past the range,
 * even where an operand does; remove_range() leaves a container of at most
 * LION_CONTAINER_MAX_SIZE even when it has nothing to remove; and the bulk
 * builder, fed the pairs of a damaged sparse segment
 * (lion_cursor_emit_segment()), builds a well-formed container of what it
 * was fed, masked, instead of an unsorted one - or an assertion failure.
 */
static void
test_damaged_results(void)
{
	Ref		   *r = &ref_a;
	static const uint16 fed[] = {10, 5, 10, 40000, 3, 65535, 32767, 0};
	uint32		i;

	phase("damaged: set algebra of an ARRAY with members past 32767");
	lion_container_init(dmg_a, TEST_CKEY);
	dmg_a->cardinality = 4;
	LION_ARRAY_DATA(dmg_a)[0] = 5;
	LION_ARRAY_DATA(dmg_a)[1] = 40000;
	LION_ARRAY_DATA(dmg_a)[2] = 50000;
	LION_ARRAY_DATA(dmg_a)[3] = 65535;
	lion_container_init(dmg_b, TEST_CKEY);
	dmg_b->cardinality = 3;
	LION_ARRAY_DATA(dmg_b)[0] = 5;
	LION_ARRAY_DATA(dmg_b)[1] = 40000;
	LION_ARRAY_DATA(dmg_b)[2] = 60000;

	(void) lion_container_and(dmg_a, dmg_b, dmg_dst);
	CHECK(dmg_dst->type == LION_CT_ARRAY && dmg_dst->cardinality == 2 &&
		  LION_ARRAY_DATA(dmg_dst)[1] == (uint16) (40000 - LION_CONTAINER_RANGE),
		  "and() of two ARRAYs stores the common member past the range masked");
	(void) lion_container_or(dmg_a, dmg_b, dmg_dst);
	CHECK(raw_in_range(dmg_dst), "or() of two ARRAYs stores members in range");
	(void) lion_container_andnot(dmg_a, dmg_b, dmg_dst);
	CHECK(raw_in_range(dmg_dst), "andnot() of two ARRAYs stores members in range");
	damage_exercise(dmg_a, dmg_b);

	/* against a BITSET and a RUN: the ARRAY x other paths */
	gen_typed(r, &buf_b, LION_CT_BITSET);
	memcpy(dmg_b, &buf_b, LION_CONTAINER_MAX_SIZE);
	(void) lion_container_and(dmg_a, dmg_b, dmg_dst);
	CHECK(raw_in_range(dmg_dst), "and() with a BITSET stores members in range");
	(void) lion_container_andnot(dmg_a, dmg_b, dmg_dst);
	CHECK(raw_in_range(dmg_dst), "andnot() of a BITSET stores members in range");
	gen_typed(r, &buf_b, LION_CT_RUN);
	memcpy(dmg_b, &buf_b, LION_CONTAINER_MAX_SIZE);
	(void) lion_container_and(dmg_a, dmg_b, dmg_dst);
	CHECK(raw_in_range(dmg_dst), "and() with a RUN stores members in range");
	(void) lion_container_andnot(dmg_a, dmg_b, dmg_dst);
	CHECK(raw_in_range(dmg_dst), "andnot() of a RUN stores members in range");

	/*
	 * The shortcuts that copy an operand (container_copy()): an empty BITSET,
	 * because two ARRAYs that fit one take array_union() instead.
	 */
	lion_container_init(dmg_b, TEST_CKEY);
	dmg_b->type = LION_CT_BITSET;
	memset(LION_BITSET_DATA(dmg_b), 0, LION_BITSET_BYTES);
	(void) lion_container_or(dmg_a, dmg_b, dmg_dst);
	CHECK(raw_in_range(dmg_dst) && dmg_dst->cardinality == 4,
		  "or() with an empty operand copies the other one masked");
	(void) lion_container_or(dmg_b, dmg_a, dmg_dst);
	CHECK(raw_in_range(dmg_dst), "... either way round");
	(void) lion_container_andnot(dmg_a, dmg_b, dmg_dst);
	CHECK(raw_in_range(dmg_dst) && dmg_dst->cardinality == 4,
		  "andnot() of an empty operand copies the other one masked");
	damage_exercise(dmg_a, dmg_b);

	phase("damaged: set algebra of a RUN reaching past 32767");
	lion_container_init(dmg_a, TEST_CKEY);
	dmg_a->type = LION_CT_RUN;
	dmg_a->cardinality = 5;
	LION_RUN_NRUNS(dmg_a) = 3;
	LION_RUN_DATA(dmg_a)[0].start = 10;
	LION_RUN_DATA(dmg_a)[0].len_minus_1 = 4;
	LION_RUN_DATA(dmg_a)[1].start = 32000;
	LION_RUN_DATA(dmg_a)[1].len_minus_1 = 10000;
	LION_RUN_DATA(dmg_a)[2].start = 40000;
	LION_RUN_DATA(dmg_a)[2].len_minus_1 = 3;
	(void) lion_container_or(dmg_a, dmg_b, dmg_dst);
	CHECK(raw_in_range(dmg_dst) && lion_container_size(dmg_dst) <= LION_CONTAINER_MAX_SIZE,
		  "or() with an empty operand copies a RUN clamped");
	CHECK(lion_container_contains(dmg_dst, 12) && lion_container_contains(dmg_dst, 32767) &&
		  lion_container_range_cardinality(dmg_dst, 0, LION_CONTAINER_RANGE - 1) ==
		  5 + (LION_CONTAINER_RANGE - 32000),
		  "... keeping what iterate() reads of it");
	(void) lion_container_andnot(dmg_a, dmg_b, dmg_dst);
	CHECK(raw_in_range(dmg_dst), "andnot() of an empty operand copies a RUN clamped");
	(void) lion_container_and(dmg_a, dmg_a, dmg_dst);
	CHECK(raw_in_range(dmg_dst), "and() of the RUN with itself stays in range");
	damage_exercise(dmg_a, dmg_b);

	phase("damaged: remove_range() of an empty RUN claiming 60000 runs");
	for (i = 0; i < LION_CONTAINER_MAX_SIZE; i++)
		((char *) dmg_a)[i] = (char) rng_next();
	dmg_a->ckey = TEST_CKEY;
	dmg_a->type = LION_CT_RUN;
	dmg_a->flags = 0;
	dmg_a->cardinality = 0;
	LION_RUN_NRUNS(dmg_a) = 60000;
	memcpy(dmg_work, dmg_a, LION_CONTAINER_MAX_SIZE);
	CHECK(lion_container_remove_range(dmg_work, 100, 200) == 0,
		  "remove_range() of an empty container removes nothing");
	CHECK(lion_container_size(dmg_work) <= LION_CONTAINER_MAX_SIZE &&
		  guard_ok(dmg_work, LION_CONTAINER_MAX_SIZE),
		  "... and leaves it at most 4104 bytes, like every mutator");
	memcpy(dmg_work, dmg_a, LION_CONTAINER_MAX_SIZE);
	(void) lion_container_remove_range(dmg_work, 200, 100);
	CHECK(lion_container_size(dmg_work) <= LION_CONTAINER_MAX_SIZE,
		  "... with an empty range too");
	dmg_a->type = LION_CT_ARRAY;
	dmg_a->cardinality = 0;
	damage_exercise(dmg_a, dmg_b);

	phase("damaged: append_sorted() fed values out of order, repeated and past 32767");
	ref_init(r);
	lion_container_init(&buf_a.c, TEST_CKEY);
	for (i = 0; i < lengthof(fed); i++)
	{
		lion_container_append_sorted(&buf_a.c, fed[i]);
		(void) ref_add(r, fed[i] & (LION_CONTAINER_RANGE - 1));
	}
	CHECK(buf_a.c.type == LION_CT_ARRAY, "a few members stay an ARRAY");
	verify_full(&buf_a.c, r);

	/* past LION_ARRAY_MAX_CARD, descending, into the BITSET it becomes */
	for (i = 0; i < 3 * LION_ARRAY_MAX_CARD; i++)
	{
		uint32		v = 65535 - 10 * i;

		lion_container_append_sorted(&buf_a.c, (uint16) v);
		(void) ref_add(r, v & (LION_CONTAINER_RANGE - 1));
		if (i % 7 == 0)
		{
			/* and a repeat of one it already has */
			lion_container_append_sorted(&buf_a.c, (uint16) v);
		}
	}
	CHECK(buf_a.c.type == LION_CT_BITSET, "past 2048 members it is a BITSET");
	verify_full(&buf_a.c, r);
	lion_container_optimize(&buf_a.c);
	verify_full(&buf_a.c, r);
}

/*
 * Random payloads.  A third of them get a count field that is plausible, so
 * that the payload's contents rather than the clamps are what the functions
 * meet; the rest get anything.
 */
static void
damage_randomize(LionContainer *c)
{
	uint32		i;
	uint32		k = rng_below(3);

	for (i = 0; i < LION_CONTAINER_MAX_SIZE; i++)
		((char *) c)[i] = (char) rng_next();
	c->ckey = TEST_CKEY;
	c->type = (uint8) (rng_below(4) == 3 ? LION_CT_NARROW : LION_CT_ARRAY + rng_below(3));
	c->flags = (uint8) rng_below(2);
	/* a NARROW of any width, now and then one it cannot have (DESIGN.md §38) */
	if (c->type == LION_CT_NARROW)
		c->flags = (uint8) (rng_below(8) == 0 ? rng_next() : rng_below(TEST_KMAX + 2));
	c->cardinality = (uint16) (k == 0 ? rng_below(LION_ARRAY_MAX_CARD + 1) : rng_next());
	if (c->type == LION_CT_RUN)
	{
		LionRun    *runs = LION_RUN_DATA(c);
		uint32		nruns = (k == 0) ? rng_below(LION_RUN_MAX_NRUNS + 1) : (rng_next() & 0xFFFF);

		LION_RUN_NRUNS(c) = (uint16) nruns;
		if (k == 1)
		{
			/* plausible runs: mostly short, mostly in range */
			for (i = 0; i < LION_RUN_MAX_NRUNS; i++)
			{
				runs[i].start = (uint16) rng_below(LION_CONTAINER_RANGE + 2000);
				runs[i].len_minus_1 = (uint16) rng_below(64);
			}
		}
	}
	else if (c->type == LION_CT_ARRAY && k == 1)
	{
		/* ascending, with the odd member past the range */
		uint16	   *arr = LION_ARRAY_DATA(c);
		uint32		v = 0;

		for (i = 0; i < LION_ARRAY_MAX_CARD; i++)
		{
			v += 1 + rng_below(40);
			arr[i] = (uint16) v;
		}
	}
}

static void
test_damaged_random(uint32 iters, uint64 seed)
{
	uint32		it;

	phase("damaged: random payloads");
	rng_seed(seed);
	for (it = 0; it < iters; it++)
	{
		damage_randomize(dmg_a);
		if (rng_below(2) == 0)
		{
			gen_typed(&ref_b, &buf_b, (LionContainerType) (rng_below(4) == 3 ? LION_CT_NARROW :
														   LION_CT_ARRAY + rng_below(3)));
			memcpy(dmg_b, &buf_b, LION_CONTAINER_MAX_SIZE);
		}
		else
			damage_randomize(dmg_b);
		damage_exercise(dmg_a, dmg_b);
	}
}

/* ----------------------------------------------------------------
 *						growth in place
 *
 * lion_container.h's contract for the in-place insert and its WAL redo
 * (lion_insert_container_inplace(), LION_OP_CONTAINER_ADD): add() on an
 * ARRAY below LION_ARRAY_MAX_CARD members needs 2 bytes past its size, on a
 * RUN below LION_RUN_MAX_NRUNS runs 4, and a BITSET none.  Each add here
 * runs on an allocation of exactly that, and must also leave the same bytes
 * an add in a full-size buffer does - redo depends on the two agreeing.
 * ----------------------------------------------------------------
 */

static void
inplace_add_one(const LionContainer *c, Size alloc, uint16 lo)
{
	LionContainer *p = exact_alloc(alloc);
	Size		size = lion_container_size(c);
	bool		r1;
	bool		r2;

	CHECK(lion_container_inplace_need(c, lo) == alloc,
		  "inplace_need() is the allotment the growth contract names");
	memcpy(p, c, size);
	memcpy(&buf_e, c, size);
	r1 = lion_container_add(p, lo);
	r2 = lion_container_add(&buf_e.c, lo);
	CHECK(r1 == r2, "add() in place returns what it returns in a full buffer");
	CHECK(p->type == c->type, "add() in place keeps the representation");
	CHECK(lion_container_size(p) <= alloc, "add() in place stays inside the item");
	CHECK(guard_ok(p, alloc), "add() in place writes nothing past the item");
	CHECK(lion_container_size(p) == lion_container_size(&buf_e.c) &&
		  memcmp(p, &buf_e, lion_container_size(p)) == 0,
		  "add() in place leaves the bytes it leaves in a full buffer");
	free(p);
}

static void
test_inplace_growth(void)
{
	uint32		i;
	uint32		k;

	phase("add() within the in-place growth contract");
	rng_seed(UINT64CONST(0x5EED3001));

	/* ARRAYs from empty to one below the limit */
	for (k = 0; k < 60; k++)
	{
		uint32		n = (k < 3) ? k : ((k < 6) ? LION_ARRAY_MAX_CARD - 1 - (k - 3) :
									   rng_below(LION_ARRAY_MAX_CARD));

		gen_random(&ref_a, n);
		build_by_append(&buf_a, &ref_a);
		CHECK(buf_a.c.type == LION_CT_ARRAY, "an ARRAY below the limit");
		for (i = 0; i < 8; i++)
			inplace_add_one(&buf_a.c, lion_container_size(&buf_a.c) + sizeof(uint16),
							(uint16) (i < 2 ? (i == 0 ? 0 : LION_CONTAINER_RANGE - 1) :
									  rng_below(LION_CONTAINER_RANGE)));
	}

	/* RUNs up to one run below the limit, adding in gaps, at ends and next to runs */
	for (k = 0; k < 60; k++)
	{
		uint32		nruns = (k < 4) ? LION_RUN_MAX_NRUNS - 1 : 1 + rng_below(LION_RUN_MAX_NRUNS - 1);

		ref_init(&ref_a);
		for (i = 0; i < nruns; i++)
		{
			uint32		base = 32 * i;
			uint32		len = 1 + rng_below(8);
			uint32		j;

			for (j = 0; j < len; j++)
				(void) ref_add(&ref_a, base + 4 + j);
		}
		build_run_direct(&buf_a, &ref_a);
		CHECK(LION_RUN_NRUNS(&buf_a.c) == nruns, "a RUN below the run limit");
		for (i = 0; i < 16; i++)
		{
			uint32		lo;

			if (i < 4)
				lo = 32 * rng_below(nruns) + (i == 0 ? 3 : (i == 1 ? 4 : 20));
			else
				lo = rng_below(LION_CONTAINER_RANGE);
			inplace_add_one(&buf_a.c, lion_container_size(&buf_a.c) + sizeof(LionRun),
							(uint16) lo);
		}
	}

	/* BITSETs are always their full size */
	for (k = 0; k < 10; k++)
	{
		gen_typed(&ref_a, &buf_a, LION_CT_BITSET);
		for (i = 0; i < 8; i++)
			inplace_add_one(&buf_a.c, LION_CONTAINER_MAX_SIZE,
							(uint16) rng_below(LION_CONTAINER_RANGE));
	}

	/*
	 * A NARROW of width k (DESIGN.md §38) takes a member at an offset below
	 * 64 * k in its 8 + 512 * k bytes, and none at 64 * k or more, which
	 * widens it - the general path, which rewrites the item.
	 */
	for (k = 0; k < 40; k++)
	{
		uint32		w;

		gen_typed(&ref_a, &buf_a, LION_CT_NARROW);
		w = buf_a.c.flags;
		for (i = 0; i < 16; i++)
		{
			uint32		off = (i == 0) ? 0 : (i == 1) ? 64 * w - 1 :
				rng_below(64 * w);

			inplace_add_one(&buf_a.c, LION_NARROW_SIZE(w),
							(uint16) ((rng_below(LION_BLOCKS_PER_CONTAINER) << LION_OFFSET_BITS) | off));
		}
		for (i = 0; i < 8; i++)
		{
			uint32		off = (i == 0) ? 64 * w :
				64 * w + rng_below((1U << LION_OFFSET_BITS) - 64 * w);

			CHECK(lion_container_inplace_need(&buf_a.c,
											  (uint16) ((rng_below(LION_BLOCKS_PER_CONTAINER) << LION_OFFSET_BITS) | off)) == 0,
				  "a NARROW cannot take a member at offset 64 * k or more in place");
		}
	}

	/* and where every kind converts: no add() in place */
	gen_random(&ref_a, LION_ARRAY_MAX_CARD);
	build_by_append(&buf_a, &ref_a);
	CHECK(buf_a.c.type == LION_CT_ARRAY &&
		  lion_container_inplace_need(&buf_a.c, 7) == 0,
		  "an ARRAY of 2048 members cannot grow in place");
	ref_init(&ref_a);
	for (i = 0; i < LION_RUN_MAX_NRUNS; i++)
		(void) ref_add(&ref_a, 4 * i);
	build_run_direct(&buf_a, &ref_a);
	CHECK(lion_container_inplace_need(&buf_a.c, 7) == 0,
		  "a RUN of 1023 runs cannot grow in place");
	buf_a.c.type = LION_CT_SPARSE;
	CHECK(lion_container_inplace_need(&buf_a.c, 7) == 0,
		  "nor can anything that is no container");
}

/* ----------------------------------------------------------------
 *		dense accumulation (DESIGN.md §32, "dense and probed")
 *
 * or_inplace(), add_many() and mark_members() against the reference: the
 * union a range's sets are collected into, a few members at a time, and the
 * members of a probe's container a range holds.
 * ----------------------------------------------------------------
 */

/* A container of a random shape: small or large ARRAY, RUN or BITSET. */
static void
gen_any(Ref *r, CBuf *b)
{
	switch (rng_below(7))
	{
		case 0:
			gen_random(r, 1 + rng_below(3));	/* what a sparse segment emits */
			build_by_append(b, r);
			break;
		case 5:
			gen_typed(r, b, LION_CT_NARROW);
			break;
		case 1:
			gen_random(r, rng_below(LION_ARRAY_MAX_CARD));
			build_by_append(b, r);
			lion_container_optimize(&b->c);
			break;
		case 2:
			gen_typed(r, b, LION_CT_ARRAY);
			break;
		case 3:
			gen_typed(r, b, LION_CT_RUN);
			break;
		case 4:
			gen_typed(r, b, LION_CT_BITSET);
			break;
		default:
			gen_runs(r, 1 + rng_below(3), 1, 40);
			build_run_direct(b, r);
			break;
	}
}

/* ref_r |= r */
static void
ref_union_into(Ref *acc, const Ref *r)
{
	uint32		i;

	for (i = 0; i < LION_CONTAINER_RANGE; i++)
		if (r->m[i])
			(void) ref_add(acc, i);
}

static void
test_or_inplace(void)
{
	uint32		k;
	uint32		i;

	phase("or_inplace(): a BITSET accumulator, then optimized");
	rng_seed(UINT64CONST(0x5EED4001));
	for (k = 0; k < 40; k++)
	{
		uint32		nadd = 1 + rng_below(k < 20 ? 400 : 20);
		uint32		bad = 0;

		/* start empty, or from a random set, as a BITSET */
		if (k % 3 == 0)
			ref_init(&ref_r);
		else
			gen_random(&ref_r, rng_below(4000));
		build_by_append(&buf_d, &ref_r);
		lion_container_to_bitset(&buf_d.c);
		CHECK(buf_d.c.type == LION_CT_BITSET, "the accumulator is a BITSET");

		for (i = 0; i < nadd; i++)
		{
			uint32		before = ref_r.card;
			uint32		added;

			gen_any(&ref_a, &buf_a);
			added = lion_container_or_inplace(&buf_d.c, &buf_a.c);
			ref_union_into(&ref_r, &ref_a);
			if (added != ref_r.card - before)
				bad++;
			if (buf_d.c.type != LION_CT_BITSET ||
				buf_d.c.cardinality != ref_r.card)
				bad++;
		}
		CHECK(bad == 0, "or_inplace() counts exactly the new members and stays a BITSET");
		verify_light(&buf_d.c, &ref_r);
		lion_container_optimize(&buf_d.c);
		verify_full(&buf_d.c, &ref_r);
	}

	/* a run over the whole range, and one into a full bitset */
	ref_init(&ref_r);
	build_by_append(&buf_d, &ref_r);
	lion_container_to_bitset(&buf_d.c);
	ref_init(&ref_a);
	for (i = 0; i < LION_CONTAINER_RANGE; i++)
		(void) ref_add(&ref_a, i);
	build_run_direct(&buf_a, &ref_a);
	CHECK(lion_container_or_inplace(&buf_d.c, &buf_a.c) == LION_CONTAINER_RANGE,
		  "a full run adds 32768 members");
	CHECK(lion_container_or_inplace(&buf_d.c, &buf_a.c) == 0,
		  "... and nothing a second time");
	lion_container_optimize(&buf_d.c);
	verify_full(&buf_d.c, &ref_a);
}

static void
test_add_many(void)
{
	static uint16 vals[3 * LION_CONTAINER_RANGE];
	uint32		k;

	phase("add_many(): values in any order, repeated, past the range");
	rng_seed(UINT64CONST(0x5EED4002));
	for (k = 0; k < 60; k++)
	{
		uint32		n;
		uint32		i;
		uint32		card;

		gen_any(&ref_r, &buf_d);
		switch (k % 4)
		{
			case 0:
				n = rng_below(8);
				break;
			case 1:
				n = rng_below(LION_ARRAY_MAX_CARD);
				break;
			case 2:
				n = LION_ARRAY_MAX_CARD + rng_below(4000);
				break;
			default:
				n = rng_below(3 * LION_CONTAINER_RANGE);
				break;
		}
		for (i = 0; i < n; i++)
		{
			/* a repeat of the one before, or a value past the range */
			if (i > 0 && rng_below(8) == 0)
				vals[i] = vals[i - 1];
			else if (rng_below(16) == 0)
				vals[i] = (uint16) (LION_CONTAINER_RANGE + rng_below(LION_CONTAINER_RANGE));
			else
				vals[i] = (uint16) rng_below(LION_CONTAINER_RANGE);
			(void) ref_add(&ref_r, vals[i] & (LION_CONTAINER_RANGE - 1));
		}
		memset(scratch_img, 0xA5, sizeof(scratch_img));
		card = lion_container_add_many(&buf_d.c, vals, n, scratch_img);
		CHECK(card == ref_r.card, "add_many() returns the union's cardinality");
		verify_full(&buf_d.c, &ref_r);
	}
}

/* Bit j of marks, and the reference's answer for it. */
static bool
mark_test(const uint64 *marks, uint32 j)
{
	return ((marks[j >> 6] >> (j & 63)) & 1) != 0;
}

static void
test_mark_members(void)
{
	static uint16 sorted[LION_CONTAINER_RANGE];
	uint32		k;

	phase("mark_members(): a probe's members that a container holds");
	rng_seed(UINT64CONST(0x5EED4003));
	for (k = 0; k < 120; k++)
	{
		uint32		n;
		uint32		i;
		uint32		words;
		uint32		added;
		uint32		expect = 0;
		uint32		bad = 0;
		uint64	   *marks;

		/* the probe: from empty to every value, as an ascending array */
		switch (k % 5)
		{
			case 0:
				gen_random(&ref_b, rng_below(4));
				break;
			case 1:
				gen_random(&ref_b, rng_below(200));
				break;
			case 2:
				gen_random(&ref_b, rng_below(LION_ARRAY_MAX_CARD + 1));
				break;
			case 3:
				gen_runs(&ref_b, 1 + rng_below(30), 1, 300);
				break;
			default:
				gen_random(&ref_b, k == 4 ? LION_CONTAINER_RANGE :
						   rng_below(LION_CONTAINER_RANGE));
				break;
		}
		n = 0;
		for (i = 0; i < LION_CONTAINER_RANGE; i++)
			if (ref_b.m[i])
				sorted[n++] = (uint16) i;

		gen_any(&ref_a, &buf_a);
		/* ... and now and then a container that shares many of them */
		if (k % 7 == 0 && n > 0)
		{
			ref_init(&ref_a);
			for (i = 0; i < n; i += 1 + rng_below(3))
				(void) ref_add(&ref_a, sorted[i]);
			build_by_append(&buf_a, &ref_a);
			lion_container_optimize(&buf_a.c);
		}

		/* marks in an exact buffer; a few bits set beforehand */
		words = (n + 63) / 64;
		marks = exact_alloc(Max(words, 1) * sizeof(uint64));
		memset(marks, 0, Max(words, 1) * sizeof(uint64));
		for (i = 0; i < n; i += 97)
			marks[i >> 6] |= UINT64CONST(1) << (i & 63);

		for (i = 0; i < n; i++)
			if (ref_a.m[sorted[i]] && i % 97 != 0)
				expect++;
		added = lion_container_mark_members(&buf_a.c, sorted, n, marks);
		CHECK(added == expect, "mark_members() counts the bits it set");
		for (i = 0; i < n; i++)
			if (mark_test(marks, i) != (ref_a.m[sorted[i]] || i % 97 == 0))
				bad++;
		for (i = n; i < words * 64; i++)
			if (mark_test(marks, i))
				bad++;
		CHECK(bad == 0, "mark_members() marks exactly the members it holds");
		CHECK(guard_ok(marks, Max(words, 1) * sizeof(uint64)),
			  "mark_members() writes nothing past the marks");
		free(marks);
	}

	phase("mark_members(): damaged containers stay inside the marks");
	for (k = 0; k < 3; k++)
	{
		uint32		n = 5;
		uint64	   *marks = exact_alloc(sizeof(uint64));
		uint32		i;

		for (i = 0; i < n; i++)
			sorted[i] = (uint16) (i * 7000);
		lion_container_init(dmg_a, TEST_CKEY);
		if (k == 0)
		{
			/* members past the range and out of order */
			dmg_a->cardinality = 4;
			LION_ARRAY_DATA(dmg_a)[0] = 65535;
			LION_ARRAY_DATA(dmg_a)[1] = 7000;
			LION_ARRAY_DATA(dmg_a)[2] = 32768 + 14000;
			LION_ARRAY_DATA(dmg_a)[3] = 0;
		}
		else if (k == 1)
		{
			/* runs past the range and overlapping */
			dmg_a->type = LION_CT_RUN;
			dmg_a->cardinality = 65535;
			LION_RUN_NRUNS(dmg_a) = 3;
			LION_RUN_DATA(dmg_a)[0].start = 20000;
			LION_RUN_DATA(dmg_a)[0].len_minus_1 = 65535;
			LION_RUN_DATA(dmg_a)[1].start = 0;
			LION_RUN_DATA(dmg_a)[1].len_minus_1 = 7000;
			LION_RUN_DATA(dmg_a)[2].start = 65535;
			LION_RUN_DATA(dmg_a)[2].len_minus_1 = 65535;
		}
		else
		{
			/* a header claiming more members than an ARRAY may hold */
			dmg_a->cardinality = 60000;
			for (i = 0; i < LION_ARRAY_MAX_CARD; i++)
				LION_ARRAY_DATA(dmg_a)[i] = (uint16) (i * 16);
		}
		marks[0] = 0;
		(void) lion_container_mark_members(dmg_a, sorted, n, marks);
		CHECK((marks[0] >> n) == 0, "no bit at or past n");
		CHECK(guard_ok(marks, sizeof(uint64)), "nothing past the marks");
		free(marks);

		/* the same containers into a BITSET accumulator */
		lion_container_init(dmg_b, TEST_CKEY);
		lion_container_to_bitset(dmg_b);
		(void) lion_container_or_inplace(dmg_b, dmg_a);
		CHECK(guard_ok(dmg_b, LION_CONTAINER_MAX_SIZE) &&
			  dmg_b->cardinality == img_card(LION_BITSET_DATA(dmg_b)),
			  "or_inplace() of a damaged container stays inside the bitset, counted");
	}
}

/* ----------------------------------------------------------------
 *		the AND of a few members with a union (DESIGN.md §29.11)
 *
 * and_union_raw() against the reference: a AND (b[0] OR ... OR b[nb-1]),
 * for a of every representation up to LION_ARRAY_MAX_CARD members and lists
 * of every shape - disjoint members, overlapping ones, members that hold all
 * of a or none of it, empty lists - and damaged inputs on either side.
 * ----------------------------------------------------------------
 */

#define AU_MAXB 12

static void
test_and_union(void)
{
	static CBuf bbuf[AU_MAXB];
	static Ref	bref;
	const LionContainer *bs[AU_MAXB];
	uint32		k;
	uint32		v;

	phase("and_union_raw(): a running intersection against a union's members");
	rng_seed(UINT64CONST(0x5EED5001));
	for (k = 0; k < 600; k++)
	{
		uint32		nb = (k % 11 == 0) ? 0 : 1 + rng_below(k % 3 == 0 ? AU_MAXB : 5);
		uint32		i;
		uint32		card;

		/* a: from one member to an ARRAY's worth, in any representation */
		switch (k % 5)
		{
			case 0:
				gen_random(&ref_a, 1 + rng_below(4));
				build_by_append(&buf_a, &ref_a);
				break;
			case 1:
				gen_random(&ref_a, rng_below(300));
				build_by_append(&buf_a, &ref_a);
				break;
			case 2:
				gen_random(&ref_a, rng_below(LION_ARRAY_MAX_CARD + 1));
				build_by_append(&buf_a, &ref_a);
				lion_container_optimize(&buf_a.c);
				break;
			case 3:
				/* runs of consecutive members, as a RUN */
				gen_runs(&ref_a, 1 + rng_below(40), 1, 40);
				build_run_direct(&buf_a, &ref_a);
				break;
			default:
				/* a BITSET of few members, as a raw AND leaves one */
				gen_random(&ref_a, rng_below(LION_ARRAY_MAX_CARD + 1));
				build_by_append(&buf_a, &ref_a);
				lion_container_to_bitset(&buf_a.c);
				break;
		}
		if (ref_a.card > LION_ARRAY_MAX_CARD)
			continue;			/* a caller never hands in more */

		ref_init(&bref);
		for (i = 0; i < nb; i++)
		{
			Ref		   *r = &ref_b;

			switch (rng_below(4))
			{
				case 0:
					/* a share of a's own members: most of them found */
					ref_init(r);
					for (v = 0; v < LION_CONTAINER_RANGE; v++)
						if (ref_a.m[v] && rng_below(nb + 1) == 0)
							(void) ref_add(r, v);
					build_by_append(&bbuf[i], r);
					lion_container_optimize(&bbuf[i].c);
					break;
				case 1:
					/* disjoint members of one scalar list: stripes */
					ref_init(r);
					for (v = i; v < LION_CONTAINER_RANGE; v += nb)
						if (rng_below(3) == 0)
							(void) ref_add(r, v);
					build_by_append(&bbuf[i], r);
					lion_container_optimize(&bbuf[i].c);
					break;
				default:
					gen_any(r, &bbuf[i]);
					break;
			}
			bs[i] = &bbuf[i].c;
			ref_union_into(&bref, r);
		}
		ref_binop(&ref_a, &bref, &ref_r, OP_AND);

		memcpy(&buf_e, &buf_a, lion_container_size(&buf_a.c));
		memset(&buf_d, 0x5A, sizeof(buf_d));
		card = lion_container_and_union_raw(&buf_a.c, bs, nb, &buf_d.c);
		CHECK(card == ref_r.card, "and_union_raw() returns the AND's cardinality");
		CHECK(buf_d.c.type == LION_CT_ARRAY && buf_d.c.ckey == TEST_CKEY,
			  "and_union_raw() leaves an ARRAY of a's ckey");
		verify_full(&buf_d.c, &ref_r);
		CHECK(memcmp(&buf_e, &buf_a, lion_container_size(&buf_a.c)) == 0,
			  "and_union_raw() leaves a as it was");

		/* ... which is what the union, built and ANDed, holds */
		if (nb > 0)
		{
			lion_container_bitset_init(&buf_b.c, TEST_CKEY);
			for (i = 0; i < nb; i++)
				lion_container_or_into_bitset(bs[i], LION_BITSET_DATA(&buf_b.c));
			(void) lion_container_bitset_recount(&buf_b.c);
			CHECK(lion_container_and_raw(&buf_a.c, &buf_b.c, &buf_e.c) == card,
				  "and_union_raw() agrees with the union built and ANDed");
		}
	}

	phase("and_union_raw(): a lookup of every member, one at a time");
	/* every member of a full ARRAY, each in exactly one of the members */
	ref_init(&ref_a);
	for (v = 0; v < LION_ARRAY_MAX_CARD; v++)
		(void) ref_add(&ref_a, v * 16);
	build_by_append(&buf_a, &ref_a);
	for (k = 0; k < 4; k++)
	{
		ref_init(&ref_b);
		for (v = k; v < LION_ARRAY_MAX_CARD; v += 4)
			(void) ref_add(&ref_b, v * 16);
		build_by_append(&bbuf[k], &ref_b);
		if (k == 1)
			lion_container_to_bitset(&bbuf[k].c);
		else if (k == 2)
			build_run_direct(&bbuf[k], &ref_b);
		bs[k] = &bbuf[k].c;
	}
	CHECK(lion_container_and_union_raw(&buf_a.c, bs, 4, &buf_d.c) == LION_ARRAY_MAX_CARD,
		  "every member of a full ARRAY is found in exactly one of four");
	verify_full(&buf_d.c, &ref_a);

	phase("and_union_raw(): damaged inputs stay inside dest");
	for (k = 0; k < 400; k++)
	{
		uint32		nb = 1 + rng_below(3);
		uint32		i;

		damage_randomize(dmg_a);
		for (i = 0; i < nb; i++)
		{
			if (rng_below(2) == 0)
				gen_any(&ref_b, &bbuf[i]);
			else
				damage_randomize(&bbuf[i].c);
			bs[i] = &bbuf[i].c;
		}
		(void) lion_container_and_union_raw(dmg_a, bs, nb, dmg_dst);
		CHECK(dmg_dst->type == LION_CT_ARRAY &&
			  lion_container_size(dmg_dst) <= LION_CONTAINER_MAX_SIZE &&
			  raw_in_range(dmg_dst) && guard_ok(dmg_dst, LION_CONTAINER_MAX_SIZE),
			  "damaged: and_union_raw() leaves an ARRAY in range, inside dest");
		{
			const char *why = NULL;

			CHECK(lion_container_check(dmg_dst, LION_CONTAINER_MAX_SIZE, &why),
				  "damaged: and_union_raw() leaves a well-formed ARRAY whatever a was");
		}

		/* a sound a against damaged members */
		gen_random(&ref_a, rng_below(LION_ARRAY_MAX_CARD + 1));
		build_by_append(&buf_a, &ref_a);
		(void) lion_container_and_union_raw(&buf_a.c, bs, nb, dmg_dst);
		{
			const char *why = NULL;

			CHECK(lion_container_check(dmg_dst, LION_CONTAINER_MAX_SIZE, &why) &&
				  guard_ok(dmg_dst, LION_CONTAINER_MAX_SIZE),
				  "damaged members: and_union_raw() leaves a well-formed ARRAY");
		}
	}
}

/* ----------------------------------------------------------------
 *		a few members evaluated in a tree (DESIGN.md §29.11)
 *
 * extract_members(), probe_members() and array_from_marks() against the
 * reference, each on its own, and together as the count engine combines
 * them: a random tree of ANDs and ORs over containers of every
 * representation, evaluated for a's members position by position - a leaf
 * probed with the positions still wanted, an AND's children each with the
 * positions the one before kept, an OR's with those none before found - and
 * compared with a AND the tree built from the reference.  Damaged inputs on
 * every side stay inside what they are given.
 * ----------------------------------------------------------------
 */

#define TP_LEAVES	10

typedef struct TpNode
{
	int			kind;			/* 0 leaf, 1 AND, 2 OR */
	int			leaf;
	int			nargs;
	struct TpNode *args[4];
} TpNode;

static CBuf tp_buf[TP_LEAVES];
static Ref	tp_ref[TP_LEAVES];
static TpNode tp_nodes[64];
static int	tp_nnodes;
static int	tp_nleaves;

/*
 * A random tree of at most `budget` leaves: each child is given what its
 * siblings before it left, less a leaf for each sibling after it.
 */
static TpNode *
tp_gen(int depth, int budget)
{
	TpNode	   *n = &tp_nodes[tp_nnodes++];
	int			i;

	if (depth == 0 || budget < 2 || rng_below(3) == 0)
	{
		n->kind = 0;
		n->leaf = tp_nleaves;
		gen_any(&tp_ref[tp_nleaves], &tp_buf[tp_nleaves]);
		tp_nleaves++;
		return n;
	}
	n->kind = 1 + rng_below(2);
	n->nargs = Min(2 + (int) rng_below(2), budget);
	for (i = 0; i < n->nargs; i++)
	{
		int			before = tp_nleaves;

		n->args[i] = tp_gen(depth - 1, budget - (n->nargs - i - 1));
		budget -= tp_nleaves - before;
	}
	return n;
}

static void
tp_ref_eval(const TpNode *n, Ref *out)
{
	static Ref	stack[16];
	static int	sp = 0;
	Ref		   *sub;
	Ref		   *tmp;
	int			i;

	if (n->kind == 0)
	{
		memcpy(out, &tp_ref[n->leaf], sizeof(Ref));
		return;
	}
	sub = &stack[sp++];
	tmp = &stack[sp++];
	tp_ref_eval(n->args[0], out);
	for (i = 1; i < n->nargs; i++)
	{
		tp_ref_eval(n->args[i], sub);
		ref_binop(out, sub, tmp, n->kind == 1 ? OP_AND : OP_OR);
		memcpy(out, tmp, sizeof(Ref));
	}
	sp -= 2;
}

/* found |= the positions of pend[] whose values the tree holds */
static void
tp_eval(const TpNode *n, const uint16 *vals, const uint16 *pend, uint32 np,
		uint64 *found)
{
	uint16		rest[LION_ARRAY_MAX_CARD];
	uint64		f[LION_ARRAY_MAX_CARD / 64];
	uint32		nr = np;
	uint32		j;
	uint32		k;
	int			i;

	memcpy(rest, pend, sizeof(uint16) * np);
	if (n->kind == 0)
	{
		(void) lion_container_probe_members(&tp_buf[n->leaf].c, vals, rest,
											nr, found);
		return;
	}
	for (i = 0; i < n->nargs && nr > 0; i++)
	{
		memset(f, 0, sizeof(f));
		tp_eval(n->args[i], vals, rest, nr, f);
		for (j = k = 0; j < nr; j++)
		{
			uint32		p = rest[j];
			bool		hit = (f[p >> 6] >> (p & 63)) & 1;

			if (n->kind == 2 && hit)
				found[p >> 6] |= UINT64CONST(1) << (p & 63);
			if ((n->kind == 1) == hit)
				rest[k++] = (uint16) p;
		}
		nr = k;
	}
	if (n->kind == 1)
		for (j = 0; j < nr; j++)
			found[rest[j] >> 6] |= UINT64CONST(1) << (rest[j] & 63);
}

static void
test_tree_probe(void)
{
	uint16		vals[LION_CONTAINER_RANGE];
	uint16		pend[LION_ARRAY_MAX_CARD];
	uint16		keep[LION_ARRAY_MAX_CARD];
	uint64		found[LION_ARRAY_MAX_CARD / 64];
	uint64		want[LION_ARRAY_MAX_CARD / 64];
	uint32		k;
	uint32		v;

	phase("extract_members(): a container's members, bounded");
	rng_seed(UINT64CONST(0x5EED6001));
	for (k = 0; k < 400; k++)
	{
		uint32		cap = (k % 4 == 0) ? rng_below(LION_ARRAY_MAX_CARD + 1) :
			LION_CONTAINER_RANGE;
		uint16	   *out = exact_alloc(sizeof(uint16) * Max(cap, 1));
		uint32		n;
		uint32		i = 0;
		bool		same = true;

		gen_any(&ref_a, &buf_a);
		n = lion_container_extract_members(&buf_a.c, out, cap);
		if (ref_a.card > cap)
			CHECK(n == cap + 1, "extract_members() says there are more than cap");
		else
		{
			CHECK(n == ref_a.card, "extract_members() returns the cardinality");
			for (v = 0; v < LION_CONTAINER_RANGE && i < n; v++)
				if (ref_a.m[v])
					same = same && (out[i++] == v);
			CHECK(same, "extract_members() writes the members, ascending");
		}
		CHECK(guard_ok(out, sizeof(uint16) * Max(cap, 1)),
			  "extract_members() writes no more than cap");
		free(out);
	}

	phase("probe_members(): positions still wanted, looked up in one container");
	for (k = 0; k < 600; k++)
	{
		uint32		n = 0;
		uint32		np = 0;
		uint32		kept;
		uint32		i;
		uint32		j = 0;
		bool		ok = true;

		gen_random(&ref_a, 1 + rng_below(LION_ARRAY_MAX_CARD));
		for (v = 0; v < LION_CONTAINER_RANGE; v++)
			if (ref_a.m[v])
				vals[n++] = (uint16) v;
		for (i = 0; i < n; i++)
			if (rng_below(3) != 0)
				pend[np++] = (uint16) i;
		gen_any(&ref_b, &buf_b);

		/* bits already set elsewhere stay set */
		memset(found, 0, sizeof(found));
		memset(want, 0, sizeof(want));
		for (i = 0; i < n; i++)
			if (rng_below(7) == 0)
				found[i >> 6] |= UINT64CONST(1) << (i & 63);
		memcpy(want, found, sizeof(found));
		memcpy(keep, pend, sizeof(uint16) * np);

		kept = lion_container_probe_members(&buf_b.c, vals, pend, np, found);
		for (i = 0; i < np; i++)
		{
			uint32		p = keep[i];

			if (ref_b.m[vals[p]])
				want[p >> 6] |= UINT64CONST(1) << (p & 63);
			else
				ok = ok && j < kept && pend[j++] == p;
		}
		CHECK(ok && j == kept,
			  "probe_members() keeps the positions not found, in order");
		CHECK(memcmp(found, want, sizeof(found)) == 0,
			  "probe_members() marks exactly the positions found");
	}

	phase("array_from_marks(): the marked positions, as an ARRAY");
	for (k = 0; k < 400; k++)
	{
		uint32		n = 0;
		uint32		i;
		uint32		card;

		gen_random(&ref_a, rng_below(LION_ARRAY_MAX_CARD + 1));
		for (v = 0; v < LION_CONTAINER_RANGE; v++)
			if (ref_a.m[v])
				vals[n++] = (uint16) v;
		ref_init(&ref_r);
		memset(found, 0xFF, sizeof(found));	/* bits past n are ignored */
		for (i = 0; i < n; i++)
		{
			if (rng_below(2) == 0)
				found[i >> 6] &= ~(UINT64CONST(1) << (i & 63));
			else
				(void) ref_add(&ref_r, vals[i]);
		}
		memset(&buf_d, 0x5A, sizeof(buf_d));
		card = lion_container_array_from_marks(&buf_d.c, TEST_CKEY, vals, n,
											   found);
		CHECK(card == ref_r.card && buf_d.c.type == LION_CT_ARRAY &&
			  buf_d.c.ckey == TEST_CKEY,
			  "array_from_marks() leaves an ARRAY of the marked members");
		verify_full(&buf_d.c, &ref_r);
	}

	phase("a few members evaluated in a tree of ANDs and ORs");
	for (k = 0; k < 800; k++)
	{
		TpNode	   *root;
		uint32		n;
		uint32		i;
		uint32		card;

		tp_nnodes = 0;
		tp_nleaves = 0;
		root = tp_gen(1 + rng_below(3), TP_LEAVES);
		tp_ref_eval(root, &ref_b);

		/* a: a few members, or up to an ARRAY's worth, any representation */
		if (k % 3 == 0)
			gen_any(&ref_a, &buf_a);
		else
		{
			gen_random(&ref_a, (k % 3 == 1) ? 1 + rng_below(4) :
					   rng_below(LION_ARRAY_MAX_CARD + 1));
			build_by_append(&buf_a, &ref_a);
		}
		if (ref_a.card > LION_ARRAY_MAX_CARD)
			continue;			/* the engine never probes with more */
		ref_binop(&ref_a, &ref_b, &ref_r, OP_AND);

		n = lion_container_extract_members(&buf_a.c, vals, LION_ARRAY_MAX_CARD);
		CHECK(n == ref_a.card, "a's members, extracted");
		for (i = 0; i < n; i++)
			pend[i] = (uint16) i;
		memset(found, 0, sizeof(found));
		tp_eval(root, vals, pend, n, found);
		card = lion_container_array_from_marks(&buf_d.c, TEST_CKEY, vals, n,
											   found);
		CHECK(card == ref_r.card, "a AND the tree, probed: its cardinality");
		verify_full(&buf_d.c, &ref_r);
	}

	phase("the tree probe's steps: damaged inputs stay inside what they are given");
	for (k = 0; k < 400; k++)
	{
		uint32		n;
		uint32		np;
		uint32		i;
		uint16	   *out = exact_alloc(sizeof(uint16) * LION_ARRAY_MAX_CARD);
		const char *why = NULL;

		damage_randomize(dmg_a);
		n = lion_container_extract_members(dmg_a, out, LION_ARRAY_MAX_CARD);
		CHECK(guard_ok(out, sizeof(uint16) * LION_ARRAY_MAX_CARD),
			  "damaged: extract_members() writes no more than cap");
		if (n > LION_ARRAY_MAX_CARD)
			n = 0;

		/* damaged vals too: unsorted, repeated */
		for (i = 0; i < n; i++)
			if (rng_below(8) == 0)
				out[i] = (uint16) rng_next();
		np = n;
		for (i = 0; i < n; i++)
			pend[i] = (uint16) i;
		memset(found, 0, sizeof(found));
		if (rng_below(2) == 0)
			damage_randomize(dmg_b);
		else
		{
			gen_any(&ref_b, &buf_b);
			memcpy(dmg_b, &buf_b, lion_container_size(&buf_b.c));
		}
		np = lion_container_probe_members(dmg_b, out, pend, np, found);
		CHECK(np <= n, "damaged: probe_members() keeps no more than it is given");
		for (i = 0; i < np; i++)
			CHECK(pend[i] < n, "damaged: probe_members() keeps positions it was given");
		(void) lion_container_array_from_marks(dmg_dst, TEST_CKEY, out, n, found);
		CHECK(lion_container_check(dmg_dst, LION_CONTAINER_MAX_SIZE, &why) &&
			  guard_ok(dmg_dst, LION_CONTAINER_MAX_SIZE),
			  "damaged: array_from_marks() leaves a well-formed ARRAY");
		free(out);
	}
}

/* ----------------------------------------------------------------
 *				whole-bitset kernels, every implementation
 *
 * The library makes every pass over a whole bitset that counts - a
 * BITSET's cardinality and runs, the AND, OR and ANDNOT of two BITSETs
 * counted or written out, the in-place OR - with one of up to three
 * implementations (lion_container.c, "whole-bitset kernels"): AVX2 or
 * POPCNT on x86-64, picked by the CPU, and a portable one everywhere.
 * test_bitset_kernels() forces each one this build and CPU have in turn
 * (lion_container_simd_force()) and checks every pass
 * (lion_container_bits_pass()), and what the public functions make of
 * BITSETs, over pairs of many densities and shapes against a reference that
 * counts a bit at a time: the counts, the words written, and the
 * representation the optimizing forms then choose.  Every container sits in
 * an exact-size heap buffer at each 8-byte alignment modulo 32, the AVX2
 * kernel's vector width, so that a sanitized build sees any access past it.
 * ----------------------------------------------------------------
 */

static const char *const simd_names[] = {"auto", "portable", "popcnt", "avx2"};

#define KERN_NPATTERNS	22

/* A bit-by-bit count: no popcount instruction, builtin or table. */
static uint32
kern_ref_count(const uint64 *w)
{
	uint32		n = 0;
	uint32		i;

	for (i = 0; i < LION_CONTAINER_RANGE; i++)
		n += (uint32) ((w[i >> 6] >> (i & 63)) & 1);
	return n;
}

static bool
kern_bit(const uint64 *w, uint32 i)
{
	return ((w[i >> 6] >> (i & 63)) & 1) != 0;
}

static uint32
kern_ref_runs(const uint64 *w)
{
	uint32		n = 0;
	uint32		i;

	for (i = 0; i < LION_CONTAINER_RANGE; i++)
		if (kern_bit(w, i) && (i == 0 || !kern_bit(w, i - 1)))
			n++;
	return n;
}

/* The narrowest width that holds every member of the image w. */
static uint32
kern_ref_width(const uint64 *w)
{
	uint32		k = 1;
	uint32		i;

	for (i = 0; i < LION_CONTAINER_RANGE; i++)
		if (((w[i >> 6] >> (i & 63)) & 1) != 0)
			k = Max(k, test_lo_width(i));
	return k;
}

/*
 * The representation lion_container_optimize() chooses (DESIGN.md §3, §38)
 * for the members of the image w: is it c's, and a NARROW's width c's?
 */
static bool
kern_opt_ok(const LionContainer *c, const uint64 *w, uint32 card, uint32 nruns)
{
	uint32		k = kern_ref_width(w);
	Size		asz = (card <= LION_ARRAY_MAX_CARD) ?
		LION_CONTAINER_HDRSZ + (Size) card * sizeof(uint16) : SIZE_MAX;
	Size		rsz = (nruns <= LION_RUN_MAX_NRUNS) ?
		LION_CONTAINER_HDRSZ + sizeof(uint16) + (Size) nruns * sizeof(LionRun) : SIZE_MAX;
	Size		nsz = (k <= TEST_KMAX) ? LION_NARROW_SIZE(k) : SIZE_MAX;
	uint8		t;

	if (asz <= rsz && asz <= nsz && asz <= LION_CONTAINER_MAX_SIZE)
		t = LION_CT_ARRAY;
	else if (rsz <= nsz && rsz <= LION_CONTAINER_MAX_SIZE)
		t = LION_CT_RUN;
	else if (nsz <= LION_CONTAINER_MAX_SIZE)
		t = LION_CT_NARROW;
	else
		t = LION_CT_BITSET;
	return c->type == t && (t != LION_CT_NARROW || c->flags == k);
}

/* Bits set with probability ppm / 1e6. */
static void
kern_random(uint64 *w, uint32 ppm)
{
	uint32		i;

	memset(w, 0, LION_BITSET_BYTES);
	for (i = 0; i < LION_CONTAINER_RANGE; i++)
		if (rng_below(1000000) < ppm)
			w[i >> 6] |= UINT64CONST(1) << (i & 63);
}

/* Pattern p of KERN_NPATTERNS: edges of words, runs and blocks, densities. */
static void
kern_pattern(uint64 *w, int p)
{
	static const uint32 ppm[] = {500, 10000, 100000, 300000, 500000,
	700000, 900000, 990000, 999500};
	uint32		i;

	memset(w, 0, LION_BITSET_BYTES);
	switch (p)
	{
		case 0:					/* empty */
			break;
		case 1:					/* full */
			memset(w, 0xFF, LION_BITSET_BYTES);
			break;
		case 2:
			memset(w, 0x55, LION_BITSET_BYTES);
			break;
		case 3:
			memset(w, 0xAA, LION_BITSET_BYTES);
			break;
		case 4:					/* the first bit of every word */
			for (i = 0; i < LION_BITSET_WORDS; i++)
				w[i] = 1;
			break;
		case 5:					/* the last bit of every word */
			for (i = 0; i < LION_BITSET_WORDS; i++)
				w[i] = UINT64CONST(1) << 63;
			break;
		case 6:					/* runs of two across word boundaries */
			for (i = 0; i < LION_BITSET_WORDS; i++)
				w[i] = (i % 2 == 0) ? UINT64CONST(1) << 63 : 1;
			break;
		case 7:					/* the first member alone */
			w[0] = 1;
			break;
		case 8:					/* the last member alone */
			w[LION_BITSET_WORDS - 1] = UINT64CONST(1) << 63;
			break;
		case 9:					/* full but for the first and last */
			memset(w, 0xFF, LION_BITSET_BYTES);
			w[0] &= ~UINT64CONST(1);
			w[LION_BITSET_WORDS - 1] &= ~(UINT64CONST(1) << 63);
			break;
		case 10:				/* whole AVX2 blocks full and empty */
			for (i = 0; i < LION_BITSET_WORDS; i++)
				w[i] = ((i / 64) % 2 == 0) ? ~UINT64CONST(0) : 0;
			break;
		case 11:				/* random words */
			for (i = 0; i < LION_BITSET_WORDS; i++)
				w[i] = ((uint64) rng_next() << 32) | rng_next();
			break;
		case 12:				/* random runs */
			{
				uint32		lo = rng_below(200);

				while (lo < LION_CONTAINER_RANGE)
				{
					uint32		len = 1 + rng_below(300);

					for (i = lo; i < lo + len && i < LION_CONTAINER_RANGE; i++)
						w[i >> 6] |= UINT64CONST(1) << (i & 63);
					lo += len + 1 + rng_below(300);
				}
				break;
			}
		default:				/* 13 .. 21: densities */
			kern_random(w, ppm[p - 13]);
			break;
	}
}

/*
 * An exact-size buffer for a container at shift bytes past a 32-byte
 * boundary, which puts its payload at (shift + 8) % 32: exact_alloc()'s,
 * aligned.  *base is what to free().
 */
static LionContainer *
kern_alloc(Size shift, void **base)
{
	unsigned char *p;
	Size		i;

	if (posix_memalign((void **) &p, 32, shift + LION_CONTAINER_MAX_SIZE + guard_bytes) != 0)
	{
		printf("out of memory\n");
		exit(2);
	}
	for (i = 0; i < guard_bytes; i++)
		p[shift + LION_CONTAINER_MAX_SIZE + i] = GUARD_FILL;
	*base = p;
	return (LionContainer *) (p + shift);
}

/* c = the BITSET of w, its cardinality counted bit by bit */
static void
kern_bitset(LionContainer *c, const uint64 *w)
{
	c->ckey = TEST_CKEY;
	c->type = LION_CT_BITSET;
	c->flags = 0;
	memcpy(LION_BITSET_DATA(c), w, LION_BITSET_BYTES);
	c->cardinality = (uint16) kern_ref_count(w);
}

/* Is c's membership exactly w's, and is c well-formed? */
static bool
kern_holds(const LionContainer *c, const uint64 *w)
{
	uint64		img[LION_BITSET_WORDS];
	const char *why;

	memset(img, 0, sizeof(img));
	lion_container_or_into_bitset(c, img);
	return memcmp(img, w, LION_BITSET_BYTES) == 0 &&
		lion_container_cardinality(c) == kern_ref_count(w) &&
		lion_container_check(c, LION_CONTAINER_MAX_SIZE, &why);
}

/*
 * A raw result of two BITSETs: the reference's words in a BITSET, written by
 * the kernel pass - unless an operand was empty, which the set algebra
 * answers without one (an empty ARRAY, or a copy of the other operand).
 */
static bool
kern_raw_ok(const LionContainer *d, uint32 got, const uint64 *w, bool empty_operand)
{
	uint32		want = kern_ref_count(w);

	if (got != want || !kern_holds(d, w))
		return false;
	return empty_operand ||
		(d->type == LION_CT_BITSET &&
		 memcmp(LION_BITSET_DATA(d), w, LION_BITSET_BYTES) == 0);
}

static uint64 kern_wa[LION_BITSET_WORDS];
static uint64 kern_wb[LION_BITSET_WORDS];
static uint64 kern_want[LION_BITSET_WORDS];

/*
 * What a kernel pass should make of wa and wb, a bit at a time: its count
 * returned, and in want the words it writes (for the ops that write).
 */
static uint32
kern_ref_pass(LionBitsOp op, const uint64 *wa, const uint64 *wb, uint64 *want)
{
	uint64		counted[LION_BITSET_WORDS];
	uint32		i;

	for (i = 0; i < LION_BITSET_WORDS; i++)
	{
		switch (op)
		{
			case LION_BITS_COUNT:
			case LION_BITS_RUNS:
				counted[i] = wa[i];
				break;
			case LION_BITS_AND_COUNT:
			case LION_BITS_AND:
				want[i] = counted[i] = wa[i] & wb[i];
				break;
			case LION_BITS_OR:
				want[i] = counted[i] = wa[i] | wb[i];
				break;
			case LION_BITS_ANDNOT:
				want[i] = counted[i] = wa[i] & ~wb[i];
				break;
			case LION_BITS_OR_NEW:
				want[i] = wa[i] | wb[i];
				counted[i] = wb[i] & ~wa[i];
				break;
		}
	}
	return (op == LION_BITS_RUNS) ? kern_ref_runs(counted) : kern_ref_count(counted);
}

/*
 * Every op of the kernel pass in use against the reference, on the payloads
 * of a and b; d and e are work buffers whose payloads are written, e's in
 * place (the op's d being its a), as or_inplace() makes the pass.
 */
static uint32
kern_check_passes(const LionContainer *a, const LionContainer *b,
				  LionContainer *d, LionContainer *e)
{
	const uint64 *wa = LION_BITSET_DATA(a);
	const uint64 *wb = LION_BITSET_DATA(b);
	uint64	   *wd = LION_BITSET_DATA(d);
	uint64	   *we = LION_BITSET_DATA(e);
	uint32		bad = 0;
	int			op;

	for (op = LION_BITS_COUNT; op <= LION_BITS_OR_NEW; op++)
	{
		bool		writes = (op != LION_BITS_COUNT && op != LION_BITS_RUNS &&
							  op != LION_BITS_AND_COUNT);
		uint32		want = kern_ref_pass((LionBitsOp) op, wa, wb, kern_want);

		memset(wd, 0x5A, LION_BITSET_BYTES);
		if (lion_container_bits_pass((LionBitsOp) op, TEST_FULL, writes ? wd : NULL, wa, wb) != want ||
			(writes && memcmp(wd, kern_want, LION_BITSET_BYTES) != 0))
			bad++;
		if (!writes)
			continue;
		memcpy(we, wa, LION_BITSET_BYTES);
		if (lion_container_bits_pass((LionBitsOp) op, TEST_FULL, we, we, wb) != want ||
			memcmp(we, kern_want, LION_BITSET_BYTES) != 0)
			bad++;
	}
	return bad;
}

/*
 * One implementation over every pair of patterns: each function that runs
 * a kernel pass, against the reference.
 */
static void
test_kernels_one(void)
{
	void	   *base[4];
	LionContainer *a;
	LionContainer *b;
	LionContainer *d;
	LionContainer *e;
	int			pa;
	int			pb;
	uint32		i;
	uint32		bad_pass = 0;
	uint32		bad_count = 0;
	uint32		bad_raw = 0;
	uint32		bad_opt = 0;
	uint32		bad_mixed = 0;
	uint32		bad_inplace = 0;
	uint32		bad_guard = 0;

	for (pa = 0; pa < KERN_NPATTERNS; pa++)
	{
		for (pb = 0; pb < KERN_NPATTERNS; pb++)
		{
			const char *why;
			uint32		ca;
			uint32		want;
			uint32		got;
			bool		empty;

			/* every buffer at an alignment of its own, varying by pair */
			a = kern_alloc(8 * ((pa + pb) % 4), &base[0]);
			b = kern_alloc(8 * ((pa + 2 * pb + 1) % 4), &base[1]);
			d = kern_alloc(8 * ((pa + 3 * pb + 2) % 4), &base[2]);
			e = kern_alloc(8 * ((2 * pa + pb + 3) % 4), &base[3]);

			/* fixed seeds: the random patterns are the same for every run */
			rng_seed(UINT64CONST(0x5EED6000) + (uint64) pa);
			kern_pattern(kern_wa, pa);
			rng_seed(UINT64CONST(0x5EED7000) + (uint64) pb);
			kern_pattern(kern_wb, pb);
			kern_bitset(a, kern_wa);
			kern_bitset(b, kern_wb);
			ca = a->cardinality;
			empty = (ca == 0 || b->cardinality == 0);

			/* the passes themselves */
			bad_pass += kern_check_passes(a, b, d, e);

			/* COUNT: recount(), image_cardinality(), check() */
			if (pb == 0)
			{
				memcpy(e, a, LION_CONTAINER_MAX_SIZE);
				e->cardinality = 0;
				if (lion_container_bitset_recount(e) != ca || e->cardinality != ca ||
					lion_container_image_cardinality(LION_BITSET_DATA(a)) != ca ||
					!lion_container_check(a, LION_CONTAINER_MAX_SIZE, &why))
					bad_count++;
				e->cardinality = (uint16) (ca + 1);
				if (lion_container_check(e, LION_CONTAINER_MAX_SIZE, &why))
					bad_count++;	/* a cardinality one off must be found */
			}

			/* AND_COUNT, both ways round */
			for (i = 0; i < LION_BITSET_WORDS; i++)
				kern_want[i] = kern_wa[i] & kern_wb[i];
			want = kern_ref_count(kern_want);
			if (lion_container_and_cardinality(a, b) != want ||
				lion_container_and_cardinality(b, a) != want)
				bad_count++;

			/* AND written out: raw, both ways round, and optimized */
			got = lion_container_and_raw(a, b, d);
			if (!kern_raw_ok(d, got, kern_want, empty))
				bad_raw++;
			got = lion_container_and_raw(b, a, d);
			if (!kern_raw_ok(d, got, kern_want, empty))
				bad_raw++;
			got = lion_container_and(a, b, d);
			if (got != want || !kern_holds(d, kern_want) ||
				!kern_opt_ok(d, kern_want, want, kern_ref_runs(kern_want)))
				bad_opt++;

			/* OR */
			for (i = 0; i < LION_BITSET_WORDS; i++)
				kern_want[i] = kern_wa[i] | kern_wb[i];
			want = kern_ref_count(kern_want);
			got = lion_container_or_raw(a, b, d);
			if (!kern_raw_ok(d, got, kern_want, empty))
				bad_raw++;
			got = lion_container_or(a, b, d);
			if (got != want || !kern_holds(d, kern_want) ||
				!kern_opt_ok(d, kern_want, want, kern_ref_runs(kern_want)))
				bad_opt++;

			/* OR in place: the new members counted, the words ORed */
			memcpy(e, a, LION_CONTAINER_MAX_SIZE);
			for (i = 0; i < LION_BITSET_WORDS; i++)
				kern_want[i] = kern_wb[i] & ~kern_wa[i];
			got = lion_container_or_inplace(e, b);
			if (got != kern_ref_count(kern_want) || e->type != LION_CT_BITSET ||
				e->cardinality != ca + got)
				bad_inplace++;
			for (i = 0; i < LION_BITSET_WORDS; i++)
				kern_want[i] = kern_wa[i] | kern_wb[i];
			if (memcmp(LION_BITSET_DATA(e), kern_want, LION_BITSET_BYTES) != 0 ||
				lion_container_or_inplace(e, b) != 0)
				bad_inplace++;

			/* ANDNOT, both ways round */
			for (i = 0; i < LION_BITSET_WORDS; i++)
				kern_want[i] = kern_wa[i] & ~kern_wb[i];
			want = kern_ref_count(kern_want);
			got = lion_container_andnot_raw(a, b, d);
			if (!kern_raw_ok(d, got, kern_want, empty))
				bad_raw++;
			got = lion_container_andnot(a, b, d);
			if (got != want || !kern_holds(d, kern_want) ||
				!kern_opt_ok(d, kern_want, want, kern_ref_runs(kern_want)))
				bad_opt++;
			for (i = 0; i < LION_BITSET_WORDS; i++)
				kern_want[i] = kern_wb[i] & ~kern_wa[i];
			want = kern_ref_count(kern_want);
			got = lion_container_andnot_raw(b, a, d);
			if (!kern_raw_ok(d, got, kern_want, empty))
				bad_raw++;

			/*
			 * RUNS: a optimized as optimize() would choose, by the reference's
			 * count of its runs.  Then a in that form against the BITSET b,
			 * which fills a into the result and folds b into it in the kernel
			 * pass that counts it.
			 */
			memcpy(e, a, LION_CONTAINER_MAX_SIZE);
			lion_container_optimize(e);
			if (!kern_opt_ok(e, kern_wa, ca, kern_ref_runs(kern_wa)) ||
				!kern_holds(e, kern_wa))
				bad_opt++;
			if (e->type != LION_CT_BITSET && e->cardinality > 0)
			{
				for (i = 0; i < LION_BITSET_WORDS; i++)
					kern_want[i] = kern_wa[i] | kern_wb[i];
				want = kern_ref_count(kern_want);
				if (!kern_raw_ok(d, lion_container_or_raw(e, b, d), kern_want, empty))
					bad_mixed++;
				if (e->type == LION_CT_RUN)
				{
					for (i = 0; i < LION_BITSET_WORDS; i++)
						kern_want[i] = kern_wa[i] & ~kern_wb[i];
					want = kern_ref_count(kern_want);
					if (lion_container_andnot_raw(e, b, d) != want ||
						!kern_holds(d, kern_want))
						bad_mixed++;
					for (i = 0; i < LION_BITSET_WORDS; i++)
						kern_want[i] = kern_wa[i] & kern_wb[i];
					want = kern_ref_count(kern_want);
					if (lion_container_and(e, b, d) != want || !kern_holds(d, kern_want))
						bad_mixed++;
				}
			}

			/* the operands untouched, and nothing written past a buffer */
			if (memcmp(LION_BITSET_DATA(a), kern_wa, LION_BITSET_BYTES) != 0 ||
				memcmp(LION_BITSET_DATA(b), kern_wb, LION_BITSET_BYTES) != 0)
				bad_guard++;
			for (i = 0; i < 4; i++)
			{
				LionContainer *c = (i == 0) ? a : (i == 1) ? b : (i == 2) ? d : e;

				if (!guard_ok(c, LION_CONTAINER_MAX_SIZE))
					bad_guard++;
				free(base[i]);
			}
		}
	}
	CHECK(bad_pass == 0, "kernels: every pass counts, and writes, what the reference does");
	CHECK(bad_count == 0, "kernels: recount(), check() and and_cardinality() count as the reference");
	CHECK(bad_raw == 0, "kernels: and/or/andnot_raw() of two BITSETs write and count the reference's words");
	CHECK(bad_opt == 0, "kernels: and/or/andnot() and optimize() hold the reference's members, in its representation");
	CHECK(bad_mixed == 0, "kernels: a RUN or an ARRAY against a BITSET, folded in the counting pass");
	CHECK(bad_inplace == 0, "kernels: or_inplace() ORs the words and counts exactly the new members");
	CHECK(bad_guard == 0, "kernels: operands untouched, nothing written past a buffer");
}

/* ----------------------------------------------------------------
 *						NARROW (DESIGN.md §38)
 *
 * For each width k, 1 .. LION_NARROW_MAX_WIDTH: the transitions into and out
 * of a NARROW of that width - optimize() past 256 * k members, remove() down
 * to 256 * k, add() at offsets 64 * k - 1 and 64 * k and every offset that
 * widens it - then optimize()'s choice against the reference's sizes from
 * every representation and width a set can arrive in, the set algebra of
 * operands of mixed widths, damaged payloads of every width and of a BITSET
 * in buffers of exactly their size, and the kernel passes at every width.
 * (256 * k and 512 * k are 8K's numbers; the tests use the macros.)
 * ----------------------------------------------------------------
 */

/* Is c a bitmap of width k: a NARROW of it, or at the full width a BITSET? */
static bool
is_width(const LionContainer *c, uint32 k)
{
	if (k == TEST_FULL)
		return c->type == LION_CT_BITSET && c->flags == 0;
	return c->type == LION_CT_NARROW && c->flags == k;
}

/*
 * The width add() of lo leaves a bitmap of width k at: k if it holds lo's
 * offset, else the narrowest that does, and past LION_NARROW_MAX_WIDTH the
 * full width, a BITSET.
 */
static uint32
widened(uint32 k, uint32 lo)
{
	uint32		need = test_lo_width(lo);

	if (need <= k)
		return k;
	return (need <= TEST_KMAX) ? need : TEST_FULL;
}

static void
test_narrow_transitions(uint32 k)
{
	static CBuf saved;
	static char width_phase[128];
	uint32		cap = LION_WIDTH_ARRAY_CARD(k);
	uint32		n;
	uint32		lo;
	uint32		i;
	uint32		j;
	uint32		b;
	uint32		t;

	snprintf(width_phase, sizeof(width_phase), "NARROW of width %u: optimize() at 256 * k members", k);
	phase(width_phase);
	rng_seed(UINT64CONST(0x5EED3801) + k);
	CHECK(cap == (LION_NARROW_SIZE(k) - LION_CONTAINER_HDRSZ) / sizeof(uint16),
		  "a NARROW of width k is the size of an ARRAY of 256 * k members");
	for (n = cap - 1; n <= cap + 1; n++)
	{
		gen_narrow_sparse(&ref_a, n, k);
		CHECK(ref_width(&ref_a) == k, "the case needs width k");
		build_by_append(&buf_a, &ref_a);
		lion_container_optimize(&buf_a.c);
		if (n <= cap)
			CHECK(buf_a.c.type == LION_CT_ARRAY,
				  "256 * k members or fewer are an ARRAY (256 * k ties a NARROW of width k, and a tie goes to the ARRAY)");
		else
			CHECK(is_width(&buf_a.c, k) &&
				  lion_container_size(&buf_a.c) == LION_NARROW_SIZE(k),
				  "past 256 * k members at offsets below 64 * k are a NARROW of width k");
		verify_full(&buf_a.c, &ref_a);

		/* allow_narrow false: what optimize() made before §38 */
		lion_container_optimize_ext(&buf_a.c, false);
		CHECK(buf_a.c.type == LION_CT_ARRAY, "without NARROW the same set is an ARRAY");
		verify_full(&buf_a.c, &ref_a);
		lion_container_optimize(&buf_a.c);
		CHECK((n > cap) == is_width(&buf_a.c, k),
			  "and optimize() makes it a NARROW of width k again past 256 * k");
		verify_full(&buf_a.c, &ref_a);
	}

	snprintf(width_phase, sizeof(width_phase), "NARROW of width %u: remove() to 256 * k members", k);
	phase(width_phase);
	gen_narrow_sparse(&ref_a, cap + 4, k);
	build_narrow_direct(&buf_a, &ref_a, k);
	verify_full(&buf_a.c, &ref_a);
	while (ref_a.card > cap - 3)
	{
		lo = ref_pick_member(&ref_a);
		CHECK(lion_container_remove(&buf_a.c, (uint16) lo), "remove() of a member");
		(void) ref_remove(&ref_a, lo);
		CHECK((ref_a.card > cap) ? is_width(&buf_a.c, k) : buf_a.c.type == LION_CT_ARRAY,
			  "a NARROW of width k down to 256 * k + 1 members, an ARRAY from 256 * k");
		verify_full(&buf_a.c, &ref_a);
	}

	snprintf(width_phase, sizeof(width_phase), "NARROW of width %u: add() at 64 * k - 1, and past it", k);
	phase(width_phase);
	gen_narrow(&ref_a, cap + 200, k);
	build_narrow_direct(&buf_a, &ref_a, k);
	for (b = 0; b < LION_BLOCKS_PER_CONTAINER; b += 7)
	{
		bool		isnew;

		lo = (b << LION_OFFSET_BITS) | (64 * k - 1);
		CHECK(lion_container_inplace_need(&buf_a.c, (uint16) lo) == LION_NARROW_SIZE(k),
			  "offset 64 * k - 1: add() keeps a NARROW of width k in its size");
		isnew = ref_add(&ref_a, lo);
		CHECK(lion_container_add(&buf_a.c, (uint16) lo) == isnew, "add() at offset 64 * k - 1");
		CHECK(is_width(&buf_a.c, k), "... and the NARROW keeps its width");
		CHECK(!lion_container_add(&buf_a.c, (uint16) lo), "... and a second time adds nothing");
	}
	verify_full(&buf_a.c, &ref_a);
	memcpy(&saved, &buf_a, LION_NARROW_SIZE(k));
	memcpy(&ref_b, &ref_a, sizeof(Ref));

	/* the first and the last offset of each word past the NARROW's */
	for (j = k; j < TEST_FULL; j++)
		for (t = 0; t < 2; t++)
		{
			uint32		off = 64 * j + ((t == 0) ? 0 : 63);
			uint32		want;

			memcpy(&buf_a, &saved, LION_NARROW_SIZE(k));
			memcpy(&ref_a, &ref_b, sizeof(Ref));
			lo = (rng_below(LION_BLOCKS_PER_CONTAINER) << LION_OFFSET_BITS) | off;
			want = widened(k, lo);
			CHECK(want == ((j + 1 <= TEST_KMAX) ? j + 1 : TEST_FULL),
				  "the case widens to the next word a block's width");
			CHECK(!lion_container_contains(&buf_a.c, (uint16) lo),
				  "a NARROW of width k holds no member at offset 64 * k or more");
			CHECK(!lion_container_remove(&buf_a.c, (uint16) lo), "... removes none");
			CHECK(lion_container_inplace_need(&buf_a.c, (uint16) lo) == 0,
				  "... and cannot take one in place");
			CHECK(lion_container_add(&buf_a.c, (uint16) lo), "add() past its offsets");
			(void) ref_add(&ref_a, lo);
			CHECK(is_width(&buf_a.c, want),
				  "... widens it to the narrowest width that holds the member, past LION_NARROW_MAX_WIDTH a BITSET");
			verify_full(&buf_a.c, &ref_a);
			lion_container_optimize(&buf_a.c);
			CHECK(ref_opt_ok(&buf_a.c, &ref_a, true),
				  "... which optimize() makes the reference's representation");
			verify_full(&buf_a.c, &ref_a);
			CHECK(lion_container_remove(&buf_a.c, (uint16) lo), "remove() of the member");
			(void) ref_remove(&ref_a, lo);
			lion_container_optimize(&buf_a.c);
			CHECK(is_width(&buf_a.c, k) &&
				  memcmp(&buf_a, &saved, LION_NARROW_SIZE(k)) == 0,
				  "... and optimize() without it gives back the NARROW of width k, byte for byte");
		}

	/* the last lo of the container, at offset 511 at 8K */
	memcpy(&buf_a, &saved, LION_NARROW_SIZE(k));
	memcpy(&ref_a, &ref_b, sizeof(Ref));
	CHECK(lion_container_add(&buf_a.c, LION_CONTAINER_RANGE - 1), "add() of the last lo");
	(void) ref_add(&ref_a, LION_CONTAINER_RANGE - 1);
	CHECK(is_width(&buf_a.c, TEST_FULL), "... makes a BITSET");
	verify_full(&buf_a.c, &ref_a);

	/* append_sorted() on a NARROW, which no builder makes: as add() */
	memcpy(&buf_a, &saved, LION_NARROW_SIZE(k));
	memcpy(&ref_a, &ref_b, sizeof(Ref));
	lion_container_append_sorted(&buf_a.c, 3);
	(void) ref_add(&ref_a, 3);
	lo = ((LION_BLOCKS_PER_CONTAINER - 1) << LION_OFFSET_BITS) | (64 * k - 1);
	lion_container_append_sorted(&buf_a.c, (uint16) lo);
	(void) ref_add(&ref_a, lo);
	CHECK(is_width(&buf_a.c, k), "append_sorted() of offsets below 64 * k keeps the NARROW");
	verify_full(&buf_a.c, &ref_a);
	lion_container_append_sorted(&buf_a.c, (uint16) (lo + 1));
	(void) ref_add(&ref_a, lo + 1);
	CHECK(is_width(&buf_a.c, widened(k, lo + 1)),
		  "append_sorted() of offset 64 * k widens it, as add() does");
	verify_full(&buf_a.c, &ref_a);

	snprintf(width_phase, sizeof(width_phase), "NARROW of width %u: runs of members are not runs of bits", k);
	phase(width_phase);
	ref_init(&ref_a);
	for (i = 0; i < LION_CONTAINER_RANGE; i++)
		if (test_lo_fits(i, k))
			(void) ref_add(&ref_a, i);
	build_narrow_direct(&buf_a, &ref_a, k);
	verify_full(&buf_a.c, &ref_a);
	lion_container_optimize(&buf_a.c);
	CHECK(buf_a.c.type == LION_CT_RUN &&
		  LION_RUN_NRUNS(&buf_a.c) == LION_BLOCKS_PER_CONTAINER,
		  "every offset below 64 * k of every block: one run a block");
	verify_full(&buf_a.c, &ref_a);
	lion_container_optimize_ext(&buf_a.c, false);
	CHECK(buf_a.c.type == LION_CT_RUN, "... with or without NARROW");

	/*
	 * 2 * k runs a block, the last ending at offset 64 * k - 1 and the first
	 * starting at 0: a RUN of 10 + 512 * k bytes, two more than the NARROW of
	 * width k, which it therefore is; as runs of the payload's bits they
	 * would be a block fewer less one, a RUN smaller than the NARROW.
	 */
	ref_init(&ref_a);
	for (b = 0; b < LION_BLOCKS_PER_CONTAINER; b++)
		for (j = 0; j < 2 * k; j++)
		{
			uint32		start = (j == 2 * k - 1) ? 64 * k - 10 : 32 * j;

			for (i = start; i < start + 10; i++)
				(void) ref_add(&ref_a, (b << LION_OFFSET_BITS) | i);
		}
	CHECK(ref_nruns(&ref_a) == LION_BLOCKS_PER_CONTAINER * 2 * k &&
		  ref_opt_type(&ref_a, true) == LION_CT_NARROW && ref_width(&ref_a) == k,
		  "the case has 2 * k runs a block, and is a NARROW of width k");
	build_by_append(&buf_a, &ref_a);
	lion_container_optimize(&buf_a.c);
	CHECK(is_width(&buf_a.c, k), "a BITSET of them optimizes to a NARROW of width k");
	build_narrow_direct(&buf_a, &ref_a, k);
	lion_container_optimize(&buf_a.c);
	CHECK(is_width(&buf_a.c, k),
		  "a NARROW's runs that meet across a block boundary are counted as two");
	verify_full(&buf_a.c, &ref_a);
	if (k < TEST_KMAX)
	{
		build_narrow_direct(&buf_a, &ref_a, k + 1);
		verify_full(&buf_a.c, &ref_a);
		lion_container_optimize(&buf_a.c);
		CHECK(is_width(&buf_a.c, k), "... and one a word a block wider optimizes to width k");
	}
	lion_container_optimize_ext(&buf_a.c, false);
	CHECK(buf_a.c.type == LION_CT_RUN &&
		  LION_RUN_NRUNS(&buf_a.c) == LION_BLOCKS_PER_CONTAINER * 2 * k,
		  "... and without NARROW it is a RUN of them");
	verify_full(&buf_a.c, &ref_a);

	snprintf(width_phase, sizeof(width_phase), "NARROW of width %u: remove_if() and remove_range()", k);
	phase(width_phase);
	for (t = 0; t < 12; t++)
	{
		uint32		s;
		uint32		e;
		uint32		want;

		gen_narrow(&ref_a, cap + 8 + rng_below(t < 6 ? 200 : 3000 * k), k);
		build_narrow_direct(&buf_a, &ref_a, k);
		want = ref_remove_if(&ref_a, (t % 2) ? pred_mod3 : pred_even);
		CHECK(lion_container_remove_if(&buf_a.c, (t % 2) ? pred_mod3 : pred_even, NULL) == want,
			  "remove_if() of a NARROW removes what the reference does");
		CHECK((ref_a.card > cap) ? is_width(&buf_a.c, k) : buf_a.c.type == LION_CT_ARRAY,
			  "remove_if() leaves a NARROW of width k past 256 * k members, an ARRAY at 256 * k or fewer");
		verify_full(&buf_a.c, &ref_a);

		build_narrow_direct(&buf_a, &ref_a, k);
		for (i = 0; i < 20; i++)
		{
			/* inside a block, across offset 64 * k, across blocks */
			s = rng_below(LION_CONTAINER_RANGE);
			e = s + ((i % 3 == 0) ? rng_below(64) : (i % 3 == 1) ? rng_below(600) :
					 rng_below(8000));
			e = Min(e, LION_CONTAINER_RANGE - 1);
			CHECK(lion_container_range_cardinality(&buf_a.c, (uint16) s, (uint16) e) ==
				  ref_range_card(&ref_a, s, e),
				  "range_cardinality() of a NARROW");
		}
		s = rng_below(LION_CONTAINER_RANGE);
		e = s + rng_below(3000);
		e = Min(e, LION_CONTAINER_RANGE - 1);
		want = ref_range_card(&ref_a, s, e);
		CHECK(lion_container_remove_range(&buf_a.c, (uint16) s, (uint16) e) == want,
			  "remove_range() of a NARROW removes what the reference does");
		(void) ref_remove_range(&ref_a, s, e);
		CHECK((ref_a.card > cap || want == 0) ? is_width(&buf_a.c, k) :
			  buf_a.c.type == LION_CT_ARRAY,
			  "remove_range() leaves a NARROW of width k past 256 * k members, an ARRAY at 256 * k or fewer, and one it removes nothing from as it was");
		verify_full(&buf_a.c, &ref_a);
	}
}

/*
 * optimize() from every representation and width a set can be in: the
 * reference's choice, and the same bytes whichever it started from - its
 * result is a function of the members (and allow_narrow) alone, which
 * VACUUM's rewrite and the set algebra's results rely on.
 */
static void
test_narrow_optimize(void)
{
	static CBuf first;
	uint32		trial;

	phase("NARROW: optimize() from every representation and width, against the sizes");
	rng_seed(UINT64CONST(0x5EED3802));
	for (trial = 0; trial < 600; trial++)
	{
		uint32		k = rand_width();
		uint32		cap = LION_WIDTH_ARRAY_CARD(k);
		int			allow;

		switch (trial % 9)
		{
			case 0:
				/* around the ARRAY / NARROW boundary */
				gen_narrow(&ref_a, cap - cap / 4 + rng_below(cap / 2), k);
				break;
			case 1:
				gen_narrow(&ref_a, 1 + rng_below(LION_BLOCKS_PER_CONTAINER * 58 * k), k);
				break;
			case 2:
				gen_narrow_sparse(&ref_a, cap - 32 + rng_below(64), k);
				break;
			case 3:
				/* around the RUN / NARROW boundary, at 128 * k - 1 runs at 8K */
				gen_narrow_runs(&ref_a, cap / 2 - 40 * k + rng_below(80 * k), 2, 12, false, k);
				break;
			case 4:
				/* runs ending at offset 64 * k - 1 and starting at offset 0 */
				gen_narrow_runs(&ref_a, cap / 2 - 40 * k + rng_below(80 * k), 2, 30, true, k);
				break;
			case 5:
				gen_narrow_runs(&ref_a, 1 + rng_below(80), 40, 64 * k, trial % 18 == 5, k);
				break;
			case 6:
				/* one member no NARROW holds */
				gen_narrow(&ref_a, 600 + rng_below(3000), k);
				(void) ref_add(&ref_a, (rng_below(LION_BLOCKS_PER_CONTAINER) << LION_OFFSET_BITS) |
							   (64 * TEST_KMAX + rng_below(64 * (TEST_FULL - TEST_KMAX))));
				break;
			case 7:
				/* one member a wider NARROW holds */
				gen_narrow(&ref_a, cap + rng_below(2000), k);
				if (k < TEST_KMAX)
					(void) ref_add(&ref_a, (rng_below(LION_BLOCKS_PER_CONTAINER) << LION_OFFSET_BITS) |
								   (64 * k + rng_below(64 * (TEST_KMAX - k))));
				break;
			default:
				gen_random(&ref_a, 1 + rng_below(3000));
				break;
		}

		for (allow = 0; allow < 2; allow++)
		{
			uint32		rw = ref_width(&ref_a);
			int			from;
			bool		have = false;

			for (from = 0; from < 5; from++)
			{
				switch (from)
				{
					case 0:		/* what the builder makes: ARRAY, or BITSET */
						build_by_append(&buf_a, &ref_a);
						break;
					case 1:
						build_by_append(&buf_a, &ref_a);
						lion_container_to_bitset(&buf_a.c);
						break;
					case 2:
						if (ref_nruns(&ref_a) > LION_RUN_MAX_NRUNS)
							continue;
						build_run_direct(&buf_a, &ref_a);
						break;
					case 3:
						if (rw > TEST_KMAX)
							continue;
						build_narrow_direct(&buf_a, &ref_a, rw);
						break;
					default:
						/* a NARROW wider than its members need */
						if (rw >= TEST_KMAX)
							continue;
						build_narrow_direct(&buf_a, &ref_a, rw + 1 + rng_below(TEST_KMAX - rw));
						break;
				}
				lion_container_optimize_ext(&buf_a.c, allow != 0);
				CHECK(ref_opt_ok(&buf_a.c, &ref_a, allow != 0),
					  "optimize_ext() picks the reference's representation, and a NARROW's width");
				if (!have)
				{
					verify_full(&buf_a.c, &ref_a);
					memcpy(&first, &buf_a, lion_container_size(&buf_a.c));
					have = true;
				}
				else
					CHECK(lion_container_size(&buf_a.c) == lion_container_size(&first.c) &&
						  memcmp(&buf_a, &first, lion_container_size(&first.c)) == 0,
						  "optimize_ext() makes the same bytes from every representation and width");
			}
		}
	}
}

/* r of members a NARROW of width k holds, in representation t. */
static void
gen_typed_narrow(Ref *r, CBuf *b, LionContainerType t, uint32 k)
{
	uint32		cap = LION_WIDTH_ARRAY_CARD(k);

	switch (t)
	{
		case LION_CT_ARRAY:
			gen_narrow_sparse(r, 1 + rng_below(cap), k);
			build_by_append(b, r);
			lion_container_optimize(&b->c);
			break;
		case LION_CT_RUN:
			gen_narrow_runs(r, 1 + rng_below(60), 20, 64 * k, rng_below(2) == 0, k);
			build_by_append(b, r);
			lion_container_optimize(&b->c);
			break;
		case LION_CT_BITSET:
			gen_narrow(r, 1 + rng_below(LION_BLOCKS_PER_CONTAINER * 45 * k), k);
			build_by_append(b, r);
			lion_container_to_bitset(&b->c);
			break;
		default:
			gen_narrow(r, cap + 50 + rng_below(6 * cap), k);
			build_by_append(b, r);
			lion_container_optimize(&b->c);
			CHECK(is_width(&b->c, k), "generator produced a NARROW of the width it asked for");
			break;
	}
	CHECK(b->c.type == t, "generator produced the requested representation");
}

/*
 * The representation the unoptimized forms build a bitmap result in
 * (lion_container.h): an AND with no ARRAY at the narrower bitmap operand's
 * width (two RUNs aside), an OR of two bitmaps at the wider's and of a
 * bitmap and anything else at the full width, an ANDNOT of a bitmap at its
 * width and of a RUN at the full width.  Operands not empty.
 */
static void
check_raw_widths(void)
{
	const LionContainer *a = &buf_a.c;
	const LionContainer *b = &buf_b.c;
	uint32		wa = test_width(a);
	uint32		wb = test_width(b);
	bool		aa = (a->type == LION_CT_ARRAY);
	bool		ba = (b->type == LION_CT_ARRAY);

	if (a->cardinality == 0 || b->cardinality == 0)
		return;
	(void) lion_container_and_raw(a, b, &buf_d.c);
	if ((wa || wb) && !aa && !ba)
		CHECK(is_width(&buf_d.c, (wa && wb) ? Min(wa, wb) : Max(wa, wb)),
			  "and_raw() of a bitmap and no ARRAY builds a bitmap of the narrower bitmap's width");
	(void) lion_container_or_raw(a, b, &buf_d.c);
	if (wa && wb)
		CHECK(is_width(&buf_d.c, Max(wa, wb)),
			  "or_raw() of two bitmaps builds a bitmap of the wider one's width");
	else if ((wa || wb) && !(aa && ba))
		CHECK(is_width(&buf_d.c, TEST_FULL),
			  "or_raw() of a bitmap and another kind builds a BITSET");
	(void) lion_container_andnot_raw(a, b, &buf_d.c);
	if (wa)
		CHECK(is_width(&buf_d.c, wa), "andnot_raw() of a bitmap builds one of its width");
	else if (!aa)
		CHECK(is_width(&buf_d.c, TEST_FULL), "andnot_raw() of a RUN builds a BITSET");
}

static void
test_setops_narrow(void)
{
	static const LionContainerType types[] = {LION_CT_ARRAY, LION_CT_BITSET, LION_CT_RUN, LION_CT_NARROW};
	uint32		ta;
	uint32		tb;
	uint32		trial;

	phase("set algebra of members NARROWs of mixed widths hold, all 16 type combinations");
	rng_seed(UINT64CONST(0x5EED3803));
	for (ta = 0; ta < lengthof(types); ta++)
		for (tb = 0; tb < lengthof(types); tb++)
			for (trial = 0; trial < 16; trial++)
			{
				uint32		ka = rand_width();
				uint32		kb = (trial % 4 == 1) ? ka : rand_width();

				gen_typed_narrow(&ref_a, &buf_a, types[ta], ka);
				if (trial % 4 == 3)
					gen_typed(&ref_b, &buf_b, types[tb]);	/* and anywhere */
				else
					gen_typed_narrow(&ref_b, &buf_b, types[tb], kb);
				run_binops();
				check_raw_widths();
			}
}

/*
 * Damaged bitmaps of every width, a BITSET's among them, each in a heap
 * buffer of exactly its size: every reader on it where it lies, every
 * mutator on a copy in a LION_CONTAINER_MAX_SIZE buffer, and add() of a
 * member it holds in place, in an exact buffer again.  Every bit of the
 * payload is a legal member, so what a damaged bitmap can get wrong is its
 * cardinality, which check() has to find - and a NARROW its width, which
 * check() has to find too, and which the library reads as the nearest
 * width it can have, 1 for 0 and LION_NARROW_MAX_WIDTH for anything past
 * it, so that it never reads past the size lion_container_size() gives.
 */
static void
test_damaged_widths(uint32 iters, uint64 seed)
{
	LionContainer *nc[LION_BITSET_WIDTH + 1];
	LionContainer *ip[LION_BITSET_WIDTH + 1];
	uint32		it;
	uint32		k;

	for (k = 1; k <= TEST_FULL; k++)
	{
		Size		sz = (k == TEST_FULL) ? LION_CONTAINER_MAX_SIZE : LION_NARROW_SIZE(k);

		nc[k] = (k <= TEST_KMAX || k == TEST_FULL) ? exact_alloc(sz) : NULL;
		ip[k] = (k <= TEST_KMAX || k == TEST_FULL) ? exact_alloc(sz) : NULL;
	}

	phase("damaged: bitmaps of every width in exact buffers");
	rng_seed(seed);
	for (it = 0; it < iters; it++)
	{
		DamageIter	st;
		const char *why;
		LionContainer *c;
		uint64	   *w;
		Size		sz;
		uint32		nwords;
		uint32		truth = 0;
		uint32		i;
		uint32		n;
		uint32		lo = rng_below(LION_CONTAINER_RANGE);
		uint32		s = rng_below(LION_CONTAINER_RANGE);
		uint32		e = s + rng_below(LION_CONTAINER_RANGE - s);
		uint32		how = rng_below(4);
		bool		badwidth = false;
		uint64		mask;

		k = 1 + rng_below(TEST_KMAX + 1);
		if (k > TEST_KMAX)
			k = TEST_FULL;
		c = nc[k];
		w = TEST_NARROW_DATA(c);
		sz = (k == TEST_FULL) ? LION_CONTAINER_MAX_SIZE : LION_NARROW_SIZE(k);
		nwords = LION_WIDTH_WORDS(k);

		for (i = 0; i < sz; i++)
			((char *) c)[i] = (char) rng_next();
		if (how == 1)
			for (i = 0; i < nwords; i++)
				w[i] &= ((uint64) rng_next() << 32 | rng_next()) &
					((uint64) rng_next() << 32 | rng_next());
		for (i = 0; i < nwords; i++)
			truth += (uint32) __builtin_popcountll(w[i]);
		c->ckey = TEST_CKEY;
		c->type = (k == TEST_FULL) ? LION_CT_BITSET : LION_CT_NARROW;
		c->flags = (k == TEST_FULL) ? 0 : (uint8) k;
		c->cardinality = (uint16) ((how <= 1) ? truth :
								   (how == 2) ? rng_below(LION_CONTAINER_RANGE + 1) :
								   rng_next());

		/* a width this build does not have, read as the nearest it has */
		if (k == 1 && rng_below(4) == 0)
		{
			c->flags = 0;
			badwidth = true;
		}
		else if (k == TEST_KMAX && rng_below(4) == 0)
		{
			c->flags = (uint8) (TEST_KMAX + 1 + rng_below(255 - TEST_KMAX));
			badwidth = true;
		}
		CHECK(lion_container_size(c) == sz,
			  "damaged: a bitmap's size is its width's, a bad width read as the nearest");

		if (rng_below(2) == 0)
		{
			gen_any(&ref_b, &buf_b);
			memcpy(dmg_b, &buf_b, LION_CONTAINER_MAX_SIZE);
		}
		else
			damage_randomize(dmg_b);

		/* readers, on the bitmap where it lies */
		CHECK(lion_container_check(c, sz, &why) == (c->cardinality == truth && !badwidth),
			  "damaged: check() of a bitmap passes exactly when its width is one it can have and its cardinality is its payload's");
		(void) lion_container_check_offsets(c, TEST_MAXOFF, &why);
		CHECK(lion_container_min_width(c) <= k,
			  "damaged: min_width() of a bitmap is at most its width");
		(void) lion_container_inplace_need(c, (uint16) lo);
		(void) lion_container_contains(c, (uint16) lo);
		CHECK(lion_container_range_cardinality(c, 0, LION_CONTAINER_RANGE - 1) == truth ||
			  c->cardinality == 0,
			  "damaged: range_cardinality() counts the payload, not the header");
		(void) lion_container_range_cardinality(c, (uint16) s, (uint16) e);

		st.n = 0;
		st.bad = 0;
		st.blocks = 0;
		lion_container_iterate(c, damage_iter_cb, &st);
		CHECK(st.n == truth && st.bad == 0,
			  "damaged: iterate() of a bitmap hands out its payload's members, in range");
		memset(dmg_img, 0, LION_BITSET_BYTES);
		lion_container_or_into_bitset(c, dmg_img);
		CHECK(guard_ok(dmg_img, LION_BITSET_BYTES) && img_card(dmg_img) == truth,
			  "damaged: or_into_bitset() of a bitmap sets its payload's members");
		mask = lion_container_block_mask(c);
		CHECK(mask == st.blocks && mask == image_blocks(dmg_img),
			  "damaged: block_mask() of a bitmap names the blocks its members are on");
		n = lion_container_to_array(c, dmg_out);
		CHECK(n == truth, "damaged: to_array() of a bitmap writes its payload's members");
		(void) lion_container_and_cardinality(c, dmg_b);
		(void) lion_container_and_cardinality(dmg_b, c);
		(void) lion_container_mark_members(c, dmg_out, Min(n, 100), (uint64 *) scratch_img);

#define DAMAGE_SETOP(what, stmt) \
	do { \
		stmt; \
		CHECK(lion_container_size(dmg_dst) <= LION_CONTAINER_MAX_SIZE && \
			  raw_in_range(dmg_dst), \
			  "damaged: " what " of a bitmap leaves a result in range"); \
	} while (0)

		DAMAGE_SETOP("and()", (void) lion_container_and(c, dmg_b, dmg_dst));
		DAMAGE_SETOP("and()", (void) lion_container_and(dmg_b, c, dmg_dst));
		DAMAGE_SETOP("or()", (void) lion_container_or(c, dmg_b, dmg_dst));
		DAMAGE_SETOP("or()", (void) lion_container_or(dmg_b, c, dmg_dst));
		DAMAGE_SETOP("andnot()", (void) lion_container_andnot(c, dmg_b, dmg_dst));
		DAMAGE_SETOP("andnot()", (void) lion_container_andnot(dmg_b, c, dmg_dst));
		DAMAGE_SETOP("and_raw()", (void) lion_container_and_raw(c, dmg_b, dmg_dst));
		DAMAGE_SETOP("and_raw()", (void) lion_container_and_raw(dmg_b, c, dmg_dst));
		DAMAGE_SETOP("or_raw()", (void) lion_container_or_raw(c, dmg_b, dmg_dst));
		DAMAGE_SETOP("or_raw()", (void) lion_container_or_raw(dmg_b, c, dmg_dst));
		DAMAGE_SETOP("andnot_raw()", (void) lion_container_andnot_raw(c, dmg_b, dmg_dst));
		DAMAGE_SETOP("andnot_raw()", (void) lion_container_andnot_raw(dmg_b, c, dmg_dst));
		DAMAGE_SETOP("and_raw() with itself", (void) lion_container_and_raw(c, c, dmg_dst));
		DAMAGE_SETOP("or_raw() with itself", (void) lion_container_or_raw(c, c, dmg_dst));
		{
			uint32		k2 = rand_width();
			LionContainer *o = nc[k2];

			/* against another bitmap of a width of its own, undamaged */
			if (o != c)
			{
				gen_narrow(&ref_a, 1 + rng_below(LION_BLOCKS_PER_CONTAINER * 30 * k2), k2);
				build_narrow_direct(&buf_a, &ref_a, k2);
				memcpy(o, &buf_a, LION_NARROW_SIZE(k2));
				DAMAGE_SETOP("and_raw() with another width", (void) lion_container_and_raw(c, o, dmg_dst));
				DAMAGE_SETOP("or_raw() with another width", (void) lion_container_or_raw(o, c, dmg_dst));
				DAMAGE_SETOP("andnot_raw() with another width", (void) lion_container_andnot_raw(c, o, dmg_dst));
				DAMAGE_SETOP("andnot_raw() with another width", (void) lion_container_andnot_raw(o, c, dmg_dst));
				(void) lion_container_and_cardinality(c, o);
				CHECK(guard_ok(o, LION_NARROW_SIZE(k2)),
					  "damaged: nothing written past the other bitmap");
			}
		}
#undef DAMAGE_SETOP
		{
			const LionContainer *one = c;

			(void) lion_container_and_union_raw(dmg_b, &one, 1, dmg_dst);
			CHECK(raw_in_range(dmg_dst), "damaged: and_union_raw() probing a bitmap");
		}
		CHECK(guard_ok(c, sz) && guard_ok(dmg_dst, LION_CONTAINER_MAX_SIZE),
			  "damaged: nothing written past a bitmap or a result");

		/* mutators, on a copy of its bytes in a full buffer */
#define DAMAGE_MUTATE(what, stmt) \
	do { \
		memcpy(dmg_work, c, sz); \
		stmt; \
		st.n = 0; \
		st.bad = 0; \
		lion_container_iterate(dmg_work, damage_iter_cb, &st); \
		CHECK(lion_container_size(dmg_work) <= LION_CONTAINER_MAX_SIZE && \
			  guard_ok(dmg_work, LION_CONTAINER_MAX_SIZE) && \
			  st.n <= LION_CONTAINER_RANGE && st.bad == 0, \
			  "damaged: " what " of a bitmap leaves a container in range"); \
	} while (0)

		DAMAGE_MUTATE("add()", (void) lion_container_add(dmg_work, (uint16) lo));
		DAMAGE_MUTATE("remove()", (void) lion_container_remove(dmg_work, (uint16) lo));
		DAMAGE_MUTATE("remove_range()",
					  (void) lion_container_remove_range(dmg_work, (uint16) s, (uint16) e));
		DAMAGE_MUTATE("optimize()", lion_container_optimize(dmg_work));
		DAMAGE_MUTATE("optimize_ext(false)", lion_container_optimize_ext(dmg_work, false));
		DAMAGE_MUTATE("to_bitset()", lion_container_to_bitset(dmg_work));
		damage_pred_seed = rng_next();
		DAMAGE_MUTATE("remove_if()",
					  (void) lion_container_remove_if(dmg_work, damage_pred, NULL));
		DAMAGE_MUTATE("add_many()",
					  (void) lion_container_add_many(dmg_work, dmg_out, Min(n, 50), dmg_img));
		memcpy(dmg_work, dmg_b, LION_CONTAINER_MAX_SIZE);
		if (dmg_work->type == LION_CT_BITSET)
		{
			(void) lion_container_or_inplace(dmg_work, c);
			CHECK(guard_ok(dmg_work, LION_CONTAINER_MAX_SIZE),
				  "damaged: or_inplace() of a bitmap stays inside the accumulator");
		}
#undef DAMAGE_MUTATE

		/* add() of a member it holds, in place in exactly its size */
		lo = (lo & ~TEST_OFFSET_MASK) | ((lo & TEST_OFFSET_MASK) % (64 * k));
		memcpy(ip[k], c, sz);
		CHECK(lion_container_inplace_need(ip[k], (uint16) lo) == sz,
			  "damaged: a bitmap takes a member at an offset it holds in its size");
		(void) lion_container_add(ip[k], (uint16) lo);
		CHECK(ip[k]->type == c->type && lion_container_size(ip[k]) == sz &&
			  guard_ok(ip[k], sz),
			  "damaged: add() of such a member stays the bitmap it was, inside its size");
	}
	for (k = 1; k <= TEST_FULL; k++)
	{
		if (nc[k])
			free(nc[k]);
		if (ip[k])
			free(ip[k]);
	}
}

/*
 * The kernels at every NARROW width, each implementation: every pass
 * (lion_container_bits_pass()) over a payload of width k against a
 * reference a bit at a time - RUNS counting the runs of the words as they
 * lie - and the public functions on NARROWs of those words, against NARROWs
 * of the same width and of another, and the BITSET of a whole pattern, in
 * exact-size buffers at each 8-byte alignment modulo 32.  The payloads are
 * windows of the BITSET patterns.
 */
static uint32
narrow_ref_pass(LionBitsOp op, uint32 k, const uint64 *a, const uint64 *b, uint64 *want)
{
	static uint64 counted[LION_BITSET_WORDS];
	uint32		nwords = LION_WIDTH_WORDS(k);
	uint32		n = 0;
	uint32		i;

	for (i = 0; i < nwords; i++)
	{
		switch (op)
		{
			case LION_BITS_COUNT:
			case LION_BITS_RUNS:
				counted[i] = a[i];
				break;
			case LION_BITS_AND_COUNT:
			case LION_BITS_AND:
				want[i] = counted[i] = a[i] & b[i];
				break;
			case LION_BITS_OR:
				want[i] = counted[i] = a[i] | b[i];
				break;
			case LION_BITS_ANDNOT:
				want[i] = counted[i] = a[i] & ~b[i];
				break;
			case LION_BITS_OR_NEW:
				want[i] = a[i] | b[i];
				counted[i] = b[i] & ~a[i];
				break;
		}
	}
	for (i = 0; i < nwords * 64; i++)
	{
		bool		bit = kern_bit(counted, i);

		if (op == LION_BITS_RUNS)
			n += (bit && (i == 0 || !kern_bit(counted, i - 1))) ? 1 : 0;
		else
			n += bit ? 1 : 0;
	}
	return n;
}

/* w (LION_BITSET_WORDS) = the members of the payload nw of width k */
static void
narrow_widen(const uint64 *nw, uint32 k, uint64 *w)
{
	uint32		i;

	memset(w, 0, LION_BITSET_BYTES);
	for (i = 0; i < LION_WIDTH_WORDS(k) * 64; i++)
		if (kern_bit(nw, i))
		{
			uint32		lo = test_narrow_lo(i, k);

			w[lo >> 6] |= UINT64CONST(1) << (lo & 63);
		}
}

static LionContainer *
narrow_alloc(Size size, Size shift, void **base)
{
	unsigned char *p;
	Size		i;

	if (posix_memalign((void **) &p, 32, shift + size + guard_bytes) != 0)
	{
		printf("out of memory\n");
		exit(2);
	}
	for (i = 0; i < guard_bytes; i++)
		p[shift + size + i] = GUARD_FILL;
	*base = p;
	return (LionContainer *) (p + shift);
}

/* c = the NARROW of width k of payload nw, its count counted bit by bit */
static void
narrow_fill(LionContainer *c, uint32 k, const uint64 *nw)
{
	c->ckey = TEST_CKEY;
	c->type = LION_CT_NARROW;
	c->flags = (uint8) k;
	c->cardinality = (uint16) narrow_ref_pass(LION_BITS_COUNT, k, nw, NULL, NULL);
	memcpy(TEST_NARROW_DATA(c), nw, LION_WIDTH_BYTES(k));
}

/* r = the members of the image w */
static void
ref_of_image(Ref *r, const uint64 *w)
{
	uint32		i;

	ref_init(r);
	for (i = 0; i < LION_CONTAINER_RANGE; i++)
		if (kern_bit(w, i))
			(void) ref_add(r, i);
}

static void
test_width_kernels_one(void)
{
	static uint64 na[LION_BITSET_WORDS];
	static uint64 nb[LION_BITSET_WORDS];
	static uint64 want[LION_BITSET_WORDS];
	static uint64 dw[LION_BITSET_WORDS];
	static uint64 img[LION_BITSET_WORDS];
	static uint64 img2[LION_BITSET_WORDS];
	uint32		bad_pass = 0;
	uint32		bad_fn = 0;
	uint32		bad_mixed = 0;
	uint32		bad_guard = 0;
	uint32		k;

	for (k = 1; k <= TEST_KMAX; k++)
	{
		uint32		nwords = LION_WIDTH_WORDS(k);
		Size		nbytes = LION_WIDTH_BYTES(k);
		Size		sz = LION_NARROW_SIZE(k);
		uint32		k2 = (k % TEST_KMAX) + 1;	/* another width, when there is one */
		int			pa;
		int			pb;

		for (pa = 0; pa < KERN_NPATTERNS; pa++)
		{
			for (pb = 0; pb < KERN_NPATTERNS; pb++)
			{
				void	   *base[3];
				LionContainer *a;
				LionContainer *b;
				LionContainer *b2;
				uint32		offa = Min((uint32) (pa % 4) * 16, LION_BITSET_WORDS - nwords);
				uint32		offb = Min((uint32) (pb % 4) * 16, LION_BITSET_WORDS - nwords);
				uint32		i;
				int			op;
				uint32		ca;
				uint32		cb;
				uint32		got;

				rng_seed(UINT64CONST(0x5EED6800) + (uint64) pa);
				kern_pattern(kern_wa, pa);
				memcpy(na, kern_wa + offa, nbytes);
				rng_seed(UINT64CONST(0x5EED6900) + (uint64) pb);
				kern_pattern(kern_wb, pb);
				memcpy(nb, kern_wb + offb, nbytes);

				/* the passes, out of place and in place */
				for (op = LION_BITS_COUNT; op <= LION_BITS_OR_NEW; op++)
				{
					bool		writes = (op != LION_BITS_COUNT && op != LION_BITS_RUNS &&
										  op != LION_BITS_AND_COUNT);
					uint32		wn = narrow_ref_pass((LionBitsOp) op, k, na, nb, want);

					memset(dw, 0x5A, nbytes);
					if (lion_container_bits_pass((LionBitsOp) op, k, writes ? dw : NULL, na, nb) != wn ||
						(writes && memcmp(dw, want, nbytes) != 0))
						bad_pass++;
					if (!writes)
						continue;
					memcpy(dw, na, nbytes);
					if (lion_container_bits_pass((LionBitsOp) op, k, dw, dw, nb) != wn ||
						memcmp(dw, want, nbytes) != 0)
						bad_pass++;
				}

				/* NARROWs of those words, in exact buffers */
				a = narrow_alloc(sz, 8 * ((pa + pb) % 4), &base[0]);
				b = narrow_alloc(sz, 8 * ((pa + 2 * pb + 1) % 4), &base[1]);
				narrow_fill(a, k, na);
				narrow_fill(b, k, nb);
				ca = a->cardinality;
				cb = b->cardinality;

				{
					const char *why;

					if (!lion_container_check(a, sz, &why))
						bad_fn++;
					a->cardinality = (uint16) (ca + 1);
					if (lion_container_check(a, sz, &why))
						bad_fn++;	/* a count one off must be found */
					a->cardinality = (uint16) ca;
				}

				/* AND counted, both ways round; AND, OR, ANDNOT written */
				got = narrow_ref_pass(LION_BITS_AND_COUNT, k, na, nb, want);
				if (lion_container_and_cardinality(a, b) != got ||
					lion_container_and_cardinality(b, a) != got)
					bad_fn++;
				for (op = 0; op < 3; op++)
				{
					LionBitsOp	bop = (op == 0) ? LION_BITS_AND : (op == 1) ? LION_BITS_OR :
						LION_BITS_ANDNOT;
					uint32		wn = narrow_ref_pass(bop, k, na, nb, want);
					uint32		card;
					bool		empty = (ca == 0 || cb == 0);

					card = (op == 0) ? lion_container_and_raw(a, b, &buf_d.c) :
						(op == 1) ? lion_container_or_raw(a, b, &buf_d.c) :
						lion_container_andnot_raw(a, b, &buf_d.c);
					narrow_widen(want, k, img);
					memset(scratch_img, 0, LION_BITSET_BYTES);
					lion_container_or_into_bitset(&buf_d.c, scratch_img);
					if (card != wn || buf_d.c.cardinality != wn ||
						memcmp(scratch_img, img, LION_BITSET_BYTES) != 0)
						bad_fn++;
					if (!empty && (!is_width(&buf_d.c, k) ||
								   memcmp(TEST_NARROW_DATA(&buf_d.c), want, nbytes) != 0))
						bad_fn++;

					/* the optimizing form: the members, in the reference's representation */
					card = (op == 0) ? lion_container_and(a, b, &buf_d.c) :
						(op == 1) ? lion_container_or(a, b, &buf_d.c) :
						lion_container_andnot(a, b, &buf_d.c);
					ref_of_image(&ref_r, img);
					memset(scratch_img, 0, LION_BITSET_BYTES);
					lion_container_or_into_bitset(&buf_d.c, scratch_img);
					if (card != wn || memcmp(scratch_img, img, LION_BITSET_BYTES) != 0 ||
						!ref_opt_ok(&buf_d.c, &ref_r, true))
						bad_fn++;
				}

				/*
				 * Against a NARROW of another width, the same window of b's
				 * pattern read at it: the per-block passes of mixed widths.
				 */
				b2 = narrow_alloc(LION_NARROW_SIZE(k2), 8 * ((pa + 3 * pb + 2) % 4), &base[2]);
				memcpy(dw, kern_wb + Min(offb, LION_BITSET_WORDS - LION_WIDTH_WORDS(k2)),
					   LION_WIDTH_BYTES(k2));
				narrow_fill(b2, k2, dw);
				narrow_widen(na, k, img);
				narrow_widen(dw, k2, img2);
				for (op = 0; op < 4; op++)
				{
					uint32		card;
					uint32		wn;
					uint32		wk;

					for (i = 0; i < LION_BITSET_WORDS; i++)
						want[i] = (op == 0) ? (img[i] & img2[i]) :
							(op == 1) ? (img[i] | img2[i]) :
							(op == 2) ? (img[i] & ~img2[i]) : (img2[i] & ~img[i]);
					wn = kern_ref_count(want);
					card = (op == 0) ? lion_container_and_raw(a, b2, &buf_d.c) :
						(op == 1) ? lion_container_or_raw(a, b2, &buf_d.c) :
						(op == 2) ? lion_container_andnot_raw(a, b2, &buf_d.c) :
						lion_container_andnot_raw(b2, a, &buf_d.c);
					wk = (op == 0) ? Min(k, k2) : (op == 1) ? Max(k, k2) :
						(op == 2) ? k : k2;
					memset(scratch_img, 0, LION_BITSET_BYTES);
					lion_container_or_into_bitset(&buf_d.c, scratch_img);
					if (card != wn || buf_d.c.cardinality != wn ||
						memcmp(scratch_img, want, LION_BITSET_BYTES) != 0)
						bad_mixed++;
					if (ca > 0 && b2->cardinality > 0 && !is_width(&buf_d.c, wk))
						bad_mixed++;
					if (op == 0 && (lion_container_and_cardinality(a, b2) != wn ||
									lion_container_and_cardinality(b2, a) != wn))
						bad_mixed++;
					card = (op == 0) ? lion_container_and(b2, a, &buf_d.c) :
						(op == 1) ? lion_container_or(b2, a, &buf_d.c) :
						(op == 2) ? lion_container_andnot(a, b2, &buf_d.c) :
						lion_container_andnot(b2, a, &buf_d.c);
					ref_of_image(&ref_r, want);
					if (card != wn || !ref_opt_ok(&buf_d.c, &ref_r, true))
						bad_mixed++;
				}

				/* a NARROW against the BITSET of the whole pattern b */
				kern_bitset(&buf_b.c, kern_wb);
				narrow_widen(na, k, img);
				for (i = 0; i < LION_BITSET_WORDS; i++)
					img[i] &= kern_wb[i];
				got = kern_ref_count(img);
				if (lion_container_and_cardinality(a, &buf_b.c) != got ||
					lion_container_and_cardinality(&buf_b.c, a) != got ||
					lion_container_and_raw(a, &buf_b.c, &buf_d.c) != got ||
					(ca > 0 && buf_b.c.cardinality > 0 && !is_width(&buf_d.c, k)) ||
					lion_container_and_raw(&buf_b.c, a, &buf_d.c) != got)
					bad_fn++;

				/* or_inplace() of the NARROW into the BITSET */
				memcpy(&buf_e, &buf_b, LION_CONTAINER_MAX_SIZE);
				narrow_widen(na, k, img);
				{
					static uint64 newbits[LION_BITSET_WORDS];

					for (i = 0; i < LION_BITSET_WORDS; i++)
						newbits[i] = img[i] & ~kern_wb[i];
					got = lion_container_or_inplace(&buf_e.c, a);
					for (i = 0; i < LION_BITSET_WORDS; i++)
						img[i] |= kern_wb[i];
					if (got != kern_ref_count(newbits) ||
						memcmp(LION_BITSET_DATA(&buf_e.c), img, LION_BITSET_BYTES) != 0 ||
						buf_e.c.cardinality != kern_ref_count(img))
						bad_fn++;
				}

				/* optimize() of the NARROW: the reference's representation */
				narrow_widen(na, k, img);
				ref_of_image(&ref_a, img);
				memcpy(&buf_e, a, sz);
				lion_container_optimize(&buf_e.c);
				if (!ref_opt_ok(&buf_e.c, &ref_a, true) ||
					lion_container_cardinality(&buf_e.c) != ca)
					bad_fn++;

				if (memcmp(TEST_NARROW_DATA(a), na, nbytes) != 0 ||
					memcmp(TEST_NARROW_DATA(b), nb, nbytes) != 0 ||
					!guard_ok(a, sz) || !guard_ok(b, sz) ||
					!guard_ok(b2, LION_NARROW_SIZE(k2)))
					bad_guard++;
				free(base[0]);
				free(base[1]);
				free(base[2]);
			}
		}
	}
	CHECK(bad_pass == 0, "narrow kernels: every pass at every width counts, and writes, what the reference does");
	CHECK(bad_fn == 0, "narrow kernels: and/or/andnot, and_cardinality(), or_inplace(), optimize() and check() of NARROWs");
	CHECK(bad_mixed == 0, "narrow kernels: the set algebra of NARROWs of two widths, at the width it builds in");
	CHECK(bad_guard == 0, "narrow kernels: operands untouched, nothing read or written past a NARROW's buffer");
}

static void
test_bitset_kernels(void)
{
	static const LionSimdImpl impls[] = {LION_SIMD_PORTABLE, LION_SIMD_POPCNT, LION_SIMD_AVX2};
	LionSimdImpl best;
	uint32		k;

	phase("whole-bitset kernels");
	best = lion_container_simd_current();
	printf("  whole-bitset kernels: %s picked for this CPU; checking", simd_names[best]);
	for (k = 0; k < lengthof(impls); k++)
	{
		char		name[64];

		if (!lion_container_simd_force(impls[k]))
		{
			printf(" [%s: not in this build or CPU]", simd_names[impls[k]]);
#ifdef LION_NO_SIMD
			CHECK(impls[k] != LION_SIMD_PORTABLE,
				  "LION_NO_SIMD keeps the portable kernel");
#else
			CHECK(impls[k] != LION_SIMD_PORTABLE,
				  "every build has the portable kernel");
#endif
			continue;
		}
#ifdef LION_NO_SIMD
		CHECK(impls[k] == LION_SIMD_PORTABLE,
			  "LION_NO_SIMD builds no x86 kernel");
#endif
		CHECK(lion_container_simd_current() == impls[k], "simd_force() takes effect");
		printf(" %s", simd_names[impls[k]]);
		fflush(stdout);

		snprintf(name, sizeof(name), "%s kernel forced", simd_names[impls[k]]);
		phase_suffix = name;
		phase("whole-bitset kernels");
		test_kernels_one();
		phase("narrow kernels");
		test_width_kernels_one();

		/*
		 * And the set algebra over every type pair and the in-place OR, as
		 * the rest of this program runs them under the kernel the CPU picks.
		 */
		test_setops();
		test_setops_narrow();
		test_or_inplace();
		phase_suffix = NULL;
	}
	printf("\n");
	phase("whole-bitset kernels");
	CHECK(lion_container_simd_force(LION_SIMD_AUTO) &&
		  lion_container_simd_current() == best,
		  "simd_force(LION_SIMD_AUTO) goes back to the CPU's pick");
}

/* ----------------------------------------------------------------
 *					representative sizes (informational)
 * ----------------------------------------------------------------
 */

static const char *
type_name(const LionContainer *c)
{
	switch (c->type)
	{
		case LION_CT_ARRAY:
			return "ARRAY";
		case LION_CT_BITSET:
			return "BITSET";
		case LION_CT_RUN:
			return "RUN";
		case LION_CT_NARROW:
			{
				static char name[16];

				snprintf(name, sizeof(name), "NARROW%u", (unsigned) c->flags);
				return name;
			}
	}
	return "?";
}

static void
show_size(const char *what, const LionContainer *c)
{
	if (c->type == LION_CT_RUN)
		printf("  %-34s %5u members  %-6s %4u runs  %5zu bytes\n",
			   what, (unsigned) c->cardinality, type_name(c),
			   (unsigned) LION_RUN_NRUNS((LionContainer *) c),
			   (size_t) lion_container_size(c));
	else
		printf("  %-34s %5u members  %-6s %9s  %5zu bytes\n",
			   what, (unsigned) c->cardinality, type_name(c), "",
			   (size_t) lion_container_size(c));
}

static void
report_sizes(void)
{
	uint32		i;
	uint32		j;

	printf("\nrepresentative container sizes (after optimize):\n");

	lion_container_init(&buf_a.c, TEST_CKEY);
	show_size("empty", &buf_a.c);

	(void) lion_container_add(&buf_a.c, 123);
	lion_container_optimize(&buf_a.c);
	show_size("1 member", &buf_a.c);

	lion_container_init(&buf_a.c, TEST_CKEY);
	for (i = 0; i < 2048; i++)
		lion_container_append_sorted(&buf_a.c, (uint16) (i * 3));
	lion_container_optimize(&buf_a.c);
	show_size("2048 scattered members", &buf_a.c);

	lion_container_init(&buf_a.c, TEST_CKEY);
	for (i = 0; i < 2049; i++)
		lion_container_append_sorted(&buf_a.c, (uint16) (i * 3));
	lion_container_optimize(&buf_a.c);
	show_size("2049 scattered members", &buf_a.c);

	lion_container_init(&buf_a.c, TEST_CKEY);
	for (i = 0; i < 100; i++)
		for (j = 0; j < 10; j++)
			lion_container_append_sorted(&buf_a.c, (uint16) (i * 100 + j));
	lion_container_optimize(&buf_a.c);
	show_size("100 runs of 10", &buf_a.c);

	lion_container_init(&buf_a.c, TEST_CKEY);
	for (i = 0; i < LION_RUN_MAX_NRUNS; i++)
		for (j = 0; j < 3; j++)
			lion_container_append_sorted(&buf_a.c, (uint16) (i * 6 + j));
	lion_container_optimize(&buf_a.c);
	show_size("1023 runs of 3 (largest RUN)", &buf_a.c);

	lion_container_init(&buf_a.c, TEST_CKEY);
	for (i = 0; i < LION_CONTAINER_RANGE; i++)
		lion_container_append_sorted(&buf_a.c, (uint16) i);
	lion_container_optimize(&buf_a.c);
	show_size("32768 members (full)", &buf_a.c);

	/* every other row of tables of so many rows a page (DESIGN.md §38) */
	{
		static const uint32 rows[] = {35, 61, 97, 136, 185, 226, 291};
		uint32		r;

		for (r = 0; r < lengthof(rows); r++)
		{
			char		what[64];

			if (rows[r] > TEST_MAXOFF)
				continue;
			lion_container_init(&buf_a.c, TEST_CKEY);
			for (i = 0; i < LION_BLOCKS_PER_CONTAINER; i++)
				for (j = 1; j <= rows[r]; j += 2)
					lion_container_append_sorted(&buf_a.c,
												 (uint16) ((i << LION_OFFSET_BITS) | j));
			lion_container_optimize(&buf_a.c);
			snprintf(what, sizeof(what), "half of %u rows a page", rows[r]);
			show_size(what, &buf_a.c);
			lion_container_optimize_ext(&buf_a.c, false);
			show_size("the same, without NARROW", &buf_a.c);
		}
	}
}

/* ----------------------------------------------------------------
 *								main
 * ----------------------------------------------------------------
 */

int
main(void)
{
	printf("pg_lion container unit tests\n");
	printf("  LION_CONTAINER_HDRSZ=%zu LION_ARRAY_MAX_CARD=%zu "
		   "LION_RUN_MAX_NRUNS=%zu LION_CONTAINER_MAX_SIZE=%zu\n",
		   (size_t) LION_CONTAINER_HDRSZ, (size_t) LION_ARRAY_MAX_CARD,
		   (size_t) LION_RUN_MAX_NRUNS, (size_t) LION_CONTAINER_MAX_SIZE);

	CHECK(sizeof(LionContainer) == 8, "LionContainer header is 8 bytes");
	CHECK(sizeof(LionRun) == 4, "LionRun is 4 bytes");
	CHECK(LION_CONTAINER_MAX_SIZE == 4104, "LION_CONTAINER_MAX_SIZE is 4104");
	printf("  LION_NARROW_MAX_WIDTH=%u LION_BITSET_WIDTH=%u LION_HEAP_MAX_OFFSET=%u\n",
		   (unsigned) LION_NARROW_MAX_WIDTH, (unsigned) LION_BITSET_WIDTH,
		   (unsigned) LION_HEAP_MAX_OFFSET);
	CHECK(LION_HEAP_MAX_OFFSET == TEST_MAXOFF,
		  "LION_HEAP_MAX_OFFSET is MaxHeapTuplesPerPage's formula");
	CHECK(64 * TEST_KMAX > TEST_MAXOFF && 64 * (TEST_KMAX - 1) <= TEST_MAXOFF,
		  "LION_NARROW_MAX_WIDTH is the narrowest width whose bits hold every heap offset");
	CHECK(TEST_KMAX < TEST_FULL && TEST_FULL * 64 == (1U << LION_OFFSET_BITS) &&
		  LION_WIDTH_WORDS(TEST_FULL) == LION_BITSET_WORDS,
		  "a BITSET is the full width, every offset's bits");
	if (BLCKSZ == 8192)
	{
		uint32		k;
		bool		ok = (TEST_KMAX == 5);

		for (k = 1; k <= TEST_KMAX; k++)
			ok = ok && LION_NARROW_SIZE(k) == 8 + 512 * k &&
				LION_WIDTH_ARRAY_CARD(k) == 256 * k;
		CHECK(ok, "at 8K: widths 1 .. 5, a NARROW of width k 8 + 512 * k bytes, an ARRAY of 256 * k members");
	}

	test_empty();
	test_boundaries();
	test_array_bitset_transition();
	test_run_merge_and_split();
	test_run_overflow();
	test_full_container();
	test_optimize_choices();

	rng_seed(UINT64CONST(0x5EED0000));
	test_append_matches_add();

	test_remove_if();
	test_ranges();
	test_setops();
	test_setops_special();
	test_or_array_boundary();
	test_run_edge_cases();
	test_iterate_early_stop();
	test_gallop_intersection();
	test_and_shapes();
	test_and_probe();
	test_copy();
	test_run_intersection_overflow();
	test_remove_if_rebuild();
	test_check_rejects();
	test_check_offsets();

	dmg_a = exact_alloc(LION_CONTAINER_MAX_SIZE);
	dmg_b = exact_alloc(LION_CONTAINER_MAX_SIZE);
	dmg_dst = exact_alloc(LION_CONTAINER_MAX_SIZE);
	dmg_work = exact_alloc(LION_CONTAINER_MAX_SIZE);
	dmg_out = exact_alloc(LION_CONTAINER_RANGE * sizeof(uint16));
	dmg_img = exact_alloc(LION_BITSET_BYTES);
	rng_seed(UINT64CONST(0x5EED3000));
	test_damaged_reported();
	test_damaged_count_readers();
	test_damaged_results();
	test_damaged_random(3000, UINT64CONST(0x5EED3002));
	test_inplace_growth();
	{
		uint32		k;

		for (k = 1; k <= TEST_KMAX; k++)
			test_narrow_transitions(k);
	}
	test_narrow_optimize();
	test_setops_narrow();
	test_damaged_widths(4000, UINT64CONST(0x5EED3804));
	test_or_inplace();
	test_add_many();
	test_mark_members();
	test_and_union();
	test_tree_probe();
	test_bitset_kernels();

	test_random_ops(3, "random ops @ 0.01% density", 20000, UINT64CONST(0x5EED1001), 0);
	test_random_ops(328, "random ops @ 1% density", 20000, UINT64CONST(0x5EED1002), 0);
	test_random_ops(3277, "random ops @ 10% density", 20000, UINT64CONST(0x5EED1003), 0);
	test_random_ops(16384, "random ops @ 50% density", 20000, UINT64CONST(0x5EED1004), 0);
	test_random_ops(32440, "random ops @ 99% density", 20000, UINT64CONST(0x5EED1005), 0);
	{
		static const uint32 pct[] = {6, 25, 85};
		static char labels[TEST_KMAX_LABELS][64];
		uint32		k;
		uint32		d;
		uint32		nl = 0;

		/* at offsets below 64 * k, now and then past them, for every width */
		for (k = 1; k <= TEST_KMAX; k++)
			for (d = 0; d < lengthof(pct); d++)
			{
				uint32		bits = LION_BLOCKS_PER_CONTAINER * 64 * k;

				if (d != 1 && k != 1 && k != TEST_KMAX)
					continue;	/* every width at 25%, the ends at all three */
				snprintf(labels[nl], sizeof(labels[nl]),
						 "random ops at offsets below 64 * %u @ %u%%", k, pct[d]);
				test_random_ops(bits * pct[d] / 100, labels[nl], 20000,
								UINT64CONST(0x5EED1006) + 16 * k + d, k);
				nl++;
			}
	}

	report_sizes();

	printf("\n%ld checks, %ld member comparisons against the reference, %ld failures\n",
		   nchecks, nmember_cmps, nfail);
	if (nfail == 0)
		printf("ALL TESTS PASSED\n");
	else
		printf("*** %ld TESTS FAILED ***\n", nfail);
	return nfail == 0 ? 0 : 1;
}
