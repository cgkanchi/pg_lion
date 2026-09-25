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

#include "lion_container.h"

#define TEST_CKEY	0x0BADF00D

/* ----------------------------------------------------------------
 *							bookkeeping
 * ----------------------------------------------------------------
 */

static const char *cur_phase = "startup";
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
	cur_phase = name;
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

/* ----------------------------------------------------------------
 *							verification
 * ----------------------------------------------------------------
 */

static uint16 scratch_array[LION_CONTAINER_RANGE];
static uint16 scratch_iter[LION_CONTAINER_RANGE];

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
		default:
			expected = LION_CONTAINER_HDRSZ + sizeof(uint16) +
				(Size) LION_RUN_NRUNS(c) * sizeof(LionRun);
			CHECK(LION_RUN_NRUNS(c) == ref_nruns(r), "run count vs reference");
			break;
	}
	CHECK(lion_container_size(c) == expected, "lion_container_size()");
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
	static const LionContainerType types[] = {LION_CT_ARRAY, LION_CT_BITSET, LION_CT_RUN};
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
	static const LionContainerType types[] = {LION_CT_ARRAY, LION_CT_BITSET, LION_CT_RUN};
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
	verify_full(&buf_d.c, &ref_r);

	/* ANDNOT both ways round */
	ref_binop(&ref_a, &ref_b, &ref_r, OP_ANDNOT);
	card = lion_container_andnot(&buf_a.c, &buf_b.c, &buf_d.c);
	CHECK(card == ref_r.card, "andnot() returned the wrong cardinality");
	CHECK(buf_d.c.ckey == buf_a.c.ckey, "andnot() dest ckey");
	verify_full(&buf_d.c, &ref_r);

	ref_binop(&ref_b, &ref_a, &ref_r, OP_ANDNOT);
	card = lion_container_andnot(&buf_b.c, &buf_a.c, &buf_d.c);
	CHECK(card == ref_r.card, "reverse andnot() cardinality");
	verify_full(&buf_d.c, &ref_r);

	/* the operands must not have been touched */
	CHECK(lion_container_size(&buf_a.c) == asz && memcmp(&buf_e, &buf_a, asz) == 0,
		  "a set operation modified its left operand");
}

static void
test_setops(void)
{
	static const LionContainerType types[] = {LION_CT_ARRAY, LION_CT_BITSET, LION_CT_RUN};
	uint32		ta;
	uint32		tb;
	uint32		trial;

	phase("set algebra over all 9 type combinations");
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
	static const LionContainerType types[] = {LION_CT_ARRAY, LION_CT_BITSET, LION_CT_RUN};
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

static void
test_random_ops(uint32 target, const char *label, uint32 nops, uint64 seed)
{
	uint32		i;

	phase(label);
	rng_seed(seed);
	gen_random(&ref_a, target);
	build_by_add(&buf_a, &ref_a);
	lion_container_optimize(&buf_a.c);
	verify_full(&buf_a.c, &ref_a);

	for (i = 0; i < nops; i++)
	{
		uint32		op = rng_below(100);

		if (op < 30)
		{
			uint32		lo = rng_below(LION_CONTAINER_RANGE);
			bool		expected = ref_add(&ref_a, lo);
			bool		got = lion_container_add(&buf_a.c, (uint16) lo);

			CHECK(expected == got, "add() return value");
			CHECK(lion_container_contains(&buf_a.c, (uint16) lo),
				  "the member is present after add()");
		}
		else if (op < 52)
		{
			uint32		lo = rng_below(LION_CONTAINER_RANGE);
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
			lion_container_optimize(&buf_a.c);
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
	static const LionContainerType types[] = {LION_CT_ARRAY, LION_CT_BITSET, LION_CT_RUN};
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

	/* lion_container_size_for() agrees with lion_container_size() */
	CHECK(lion_container_size_for(LION_CT_ARRAY, 100, 0) == 8 + 200,
		  "size_for(ARRAY, 100)");
	CHECK(lion_container_size_for(LION_CT_BITSET, 9999, 0) == LION_CONTAINER_MAX_SIZE,
		  "size_for(BITSET)");
	CHECK(lion_container_size_for(LION_CT_RUN, 9999, 7) == 8 + 2 + 28,
		  "size_for(RUN, 7 runs)");
}

/* ----------------------------------------------------------------
 *				exact-size buffers (damaged containers)
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
} DamageIter;

static bool
damage_iter_cb(uint16 lo, void *arg)
{
	DamageIter *st = (DamageIter *) arg;

	st->n++;
	if ((uint32) lo > LION_CONTAINER_RANGE - 1)
		st->bad++;
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

	/* readers */
	(void) lion_container_contains(c, (uint16) lo);
	(void) lion_container_range_cardinality(c, (uint16) s, (uint16) e);

	st.n = 0;
	st.bad = 0;
	lion_container_iterate(c, damage_iter_cb, &st);
	CHECK(st.n <= LION_CONTAINER_RANGE, "damaged: iterate() visits at most 32768 values");
	CHECK(st.bad == 0, "damaged: iterate() hands out only lo values in range");

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
	(void) lion_container_and(c, other, dmg_dst);
	CHECK(lion_container_size(dmg_dst) <= LION_CONTAINER_MAX_SIZE, "damaged: and() result size");
	(void) lion_container_and(other, c, dmg_dst);
	CHECK(lion_container_size(dmg_dst) <= LION_CONTAINER_MAX_SIZE, "damaged: and() result size");
	(void) lion_container_or(c, other, dmg_dst);
	CHECK(lion_container_size(dmg_dst) <= LION_CONTAINER_MAX_SIZE, "damaged: or() result size");
	(void) lion_container_or(other, c, dmg_dst);
	CHECK(lion_container_size(dmg_dst) <= LION_CONTAINER_MAX_SIZE, "damaged: or() result size");
	(void) lion_container_andnot(c, other, dmg_dst);
	CHECK(lion_container_size(dmg_dst) <= LION_CONTAINER_MAX_SIZE, "damaged: andnot() result size");
	(void) lion_container_andnot(other, c, dmg_dst);
	CHECK(lion_container_size(dmg_dst) <= LION_CONTAINER_MAX_SIZE, "damaged: andnot() result size");
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
	c->type = (uint8) (LION_CT_ARRAY + rng_below(3));
	c->flags = (uint8) rng_below(2);
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
			gen_typed(&ref_b, &buf_b, (LionContainerType) (LION_CT_ARRAY + rng_below(3)));
			memcpy(dmg_b, &buf_b, LION_CONTAINER_MAX_SIZE);
		}
		else
			damage_randomize(dmg_b);
		damage_exercise(dmg_a, dmg_b);
	}
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
	test_run_intersection_overflow();
	test_remove_if_rebuild();
	test_check_rejects();

	dmg_a = exact_alloc(LION_CONTAINER_MAX_SIZE);
	dmg_b = exact_alloc(LION_CONTAINER_MAX_SIZE);
	dmg_dst = exact_alloc(LION_CONTAINER_MAX_SIZE);
	dmg_work = exact_alloc(LION_CONTAINER_MAX_SIZE);
	dmg_out = exact_alloc(LION_CONTAINER_RANGE * sizeof(uint16));
	rng_seed(UINT64CONST(0x5EED3000));
	test_damaged_reported();
	test_damaged_random(3000, UINT64CONST(0x5EED3002));

	test_random_ops(3, "random ops @ 0.01% density", 20000, UINT64CONST(0x5EED1001));
	test_random_ops(328, "random ops @ 1% density", 20000, UINT64CONST(0x5EED1002));
	test_random_ops(3277, "random ops @ 10% density", 20000, UINT64CONST(0x5EED1003));
	test_random_ops(16384, "random ops @ 50% density", 20000, UINT64CONST(0x5EED1004));
	test_random_ops(32440, "random ops @ 99% density", 20000, UINT64CONST(0x5EED1005));

	report_sizes();

	printf("\n%ld checks, %ld member comparisons against the reference, %ld failures\n",
		   nchecks, nmember_cmps, nfail);
	if (nfail == 0)
		printf("ALL TESTS PASSED\n");
	else
		printf("*** %ld TESTS FAILED ***\n", nfail);
	return nfail == 0 ? 0 : 1;
}
