/*-------------------------------------------------------------------------
 * sparse_test.c
 *	  Standalone unit tests for src/rbi_sparse.c (DESIGN.md §13).
 *
 *	  Built by "make unit PG_CONFIG=..." with -DFRONTEND, so this program
 *	  links against nothing but libc, the container library and the segment
 *	  library.
 *
 *	  Every segment under test is shadowed by a brute-force reference: a
 *	  plain sorted array of (ckey, lo) pairs maintained by insertion sort.
 *	  After each operation the segment is compared against the reference pair
 *	  by pair, for cardinality, for the header ckey, for iteration order and
 *	  for the answers of find/count/contains, and rbi_sparse_check() must
 *	  accept it.
 *
 *	  Exit status is 0 only if every check passed.
 *-------------------------------------------------------------------------
 */
#include "c.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rbi_sparse.h"

/* ----------------------------------------------------------------
 *							bookkeeping
 * ----------------------------------------------------------------
 */

static const char *cur_phase = "startup";
static long nchecks = 0;
static long nfail = 0;
static long npair_cmps = 0;

static void
check_impl(bool ok, int line, const char *msg)
{
	nchecks++;
	if (!ok)
	{
		nfail++;
		if (nfail <= 40)
			printf("FAIL [%s] sparse_test.c:%d: %s\n", cur_phase, line, msg);
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
 *						work buffers and reference
 * ----------------------------------------------------------------
 */

typedef union SBuf
{
	RBIContainer c;
	uint64		force_align;
	char		data[RBI_CONTAINER_MAX_SIZE];
} SBuf;

typedef struct Pair
{
	uint32		ckey;
	uint16		lo;
} Pair;

typedef struct Ref
{
	Pair		p[RBI_SPARSE_MAX_PAIRS + 8];
	uint32		n;
} Ref;

static void
ref_init(Ref *r)
{
	r->n = 0;
}

static int
pair_cmp(uint32 ak, uint16 al, uint32 bk, uint16 bl)
{
	if (ak != bk)
		return ak < bk ? -1 : 1;
	if (al != bl)
		return al < bl ? -1 : 1;
	return 0;
}

/* Insertion position of (ckey, lo) in the reference. */
static uint32
ref_pos(const Ref *r, uint32 ckey, uint16 lo)
{
	uint32		i;

	for (i = 0; i < r->n; i++)
	{
		if (pair_cmp(r->p[i].ckey, r->p[i].lo, ckey, lo) >= 0)
			break;
	}
	return i;
}

static bool
ref_contains(const Ref *r, uint32 ckey, uint16 lo)
{
	uint32		i = ref_pos(r, ckey, lo);

	return i < r->n && r->p[i].ckey == ckey && r->p[i].lo == lo;
}

static bool
ref_insert(Ref *r, uint32 ckey, uint16 lo)
{
	uint32		i = ref_pos(r, ckey, lo);
	uint32		j;

	if (i < r->n && r->p[i].ckey == ckey && r->p[i].lo == lo)
		return false;			/* duplicate */
	if (r->n >= RBI_SPARSE_MAX_PAIRS)
		return false;			/* full */

	for (j = r->n; j > i; j--)
		r->p[j] = r->p[j - 1];
	r->p[i].ckey = ckey;
	r->p[i].lo = lo;
	r->n++;
	return true;
}

static bool
ref_remove(Ref *r, uint32 ckey, uint16 lo)
{
	uint32		i = ref_pos(r, ckey, lo);
	uint32		j;

	if (i >= r->n || r->p[i].ckey != ckey || r->p[i].lo != lo)
		return false;

	for (j = i; j + 1 < r->n; j++)
		r->p[j] = r->p[j + 1];
	r->n--;
	return true;
}

static uint32
ref_count(const Ref *r, uint32 ckey)
{
	uint32		i;
	uint32		n = 0;

	for (i = 0; i < r->n; i++)
	{
		if (r->p[i].ckey == ckey)
			n++;
	}
	return n;
}

/* ----------------------------------------------------------------
 *						comparison against the reference
 * ----------------------------------------------------------------
 */

typedef struct IterState
{
	const Ref  *ref;
	uint32		i;
	bool		ok;
	uint32		stop_after;		/* stop the iteration after this many pairs */
} IterState;

static bool
iter_cb(uint32 ckey, uint16 lo, void *arg)
{
	IterState  *st = (IterState *) arg;

	if (st->i >= st->ref->n ||
		st->ref->p[st->i].ckey != ckey || st->ref->p[st->i].lo != lo)
		st->ok = false;
	st->i++;

	return st->i < st->stop_after;
}

static void
compare(const RBIContainer *s, const Ref *r, int line)
{
	const char *err = NULL;
	IterState	it;
	uint32		i;

	check_impl(rbi_sparse_check(s, rbi_sparse_size_for(r->n), &err), line,
			   err ? err : "check() rejected a good segment");
	check_impl(s->cardinality == r->n, line, "cardinality matches");
	check_impl(rbi_sparse_size(s) == rbi_sparse_size_for(r->n), line,
			   "size matches the pair count");
	check_impl(s->type == RBI_CT_SPARSE, line, "type is RBI_CT_SPARSE");
	check_impl(rbi_item_size(s) == rbi_sparse_size(s), line,
			   "rbi_item_size() dispatches to the segment size");

	if (r->n > 0)
	{
		check_impl(rbi_item_first_ckey(s) == r->p[0].ckey, line,
				   "first ckey matches");
		check_impl(rbi_item_last_ckey(s) == r->p[r->n - 1].ckey, line,
				   "last ckey matches");
		check_impl(rbi_item_covers(s, r->p[0].ckey), line,
				   "the segment covers its first ckey");
		check_impl(rbi_item_covers(s, r->p[r->n - 1].ckey), line,
				   "the segment covers its last ckey");
	}

	if (s->cardinality != r->n)
		return;					/* the payload comparison would be nonsense */

	for (i = 0; i < r->n; i++)
	{
		npair_cmps++;
		if (RBI_SPARSE_CKEYS_CONST(s)[i] != r->p[i].ckey ||
			RBI_SPARSE_LOS_CONST(s)[i] != r->p[i].lo)
		{
			check_impl(false, line, "pair matches the reference");
			break;
		}
	}

	/* contains() and count() for every ckey present, and for its neighbours */
	for (i = 0; i < r->n; i++)
	{
		uint32		first;
		uint32		count;

		npair_cmps++;
		if (!rbi_sparse_contains(s, r->p[i].ckey, r->p[i].lo))
		{
			check_impl(false, line, "contains() finds a member");
			break;
		}
		if (rbi_sparse_contains(s, r->p[i].ckey, (uint16) (r->p[i].lo ^ 0x4000)) !=
			ref_contains(r, r->p[i].ckey, (uint16) (r->p[i].lo ^ 0x4000)))
		{
			check_impl(false, line, "contains() agrees about a non-member");
			break;
		}
		rbi_sparse_find(s, r->p[i].ckey, &first, &count);
		if (count != ref_count(r, r->p[i].ckey) ||
			RBI_SPARSE_CKEYS_CONST(s)[first] != r->p[i].ckey)
		{
			check_impl(false, line, "find() returns the right range");
			break;
		}
		if (rbi_sparse_count(s, r->p[i].ckey + 1) != ref_count(r, r->p[i].ckey + 1))
		{
			check_impl(false, line, "count() agrees for the next ckey");
			break;
		}
	}

	it.ref = r;
	it.i = 0;
	it.ok = true;
	it.stop_after = UINT32_MAX;
	rbi_sparse_iterate(s, iter_cb, &it);
	check_impl(it.ok && it.i == r->n, line, "iterate() visits every pair in order");
}

#define COMPARE(s, r)	compare(&(s)->c, (r), __LINE__)

/* ----------------------------------------------------------------
 *								tests
 * ----------------------------------------------------------------
 */

static void
test_empty(void)
{
	SBuf		b;
	Ref			r;
	const char *err = NULL;

	phase("empty segment");
	ref_init(&r);
	rbi_sparse_init(&b.c, 0);

	CHECK(b.c.cardinality == 0, "a fresh segment is empty");
	CHECK(b.c.type == RBI_CT_SPARSE, "a fresh segment is a segment");
	CHECK(b.c.flags == 0, "a fresh segment has no flags");
	CHECK(rbi_sparse_size(&b.c) == RBI_CONTAINER_HDRSZ, "an empty segment is a header");
	CHECK(rbi_sparse_check(&b.c, RBI_CONTAINER_HDRSZ, &err), "check() accepts an empty segment");
	CHECK(!rbi_sparse_contains(&b.c, 0, 0), "an empty segment contains nothing");
	CHECK(rbi_sparse_count(&b.c, 17) == 0, "count() of an empty segment is 0");
	CHECK(!rbi_sparse_remove(&b.c, 1, 1), "remove() from an empty segment says no");
	COMPARE(&b, &r);
}

static void
test_one_pair(void)
{
	SBuf		b;
	Ref			r;
	bool		dup = true;

	phase("one pair");
	ref_init(&r);
	rbi_sparse_init(&b.c, 0);

	CHECK(rbi_sparse_insert(&b.c, 12345, 777, &dup), "insert succeeds");
	CHECK(!dup, "the first insert is not a duplicate");
	ref_insert(&r, 12345, 777);
	COMPARE(&b, &r);

	CHECK(b.c.ckey == 12345, "the header carries the first ckey");
	CHECK(rbi_sparse_size(&b.c) == RBI_CONTAINER_HDRSZ + 6, "one pair costs 6 bytes");

	CHECK(rbi_sparse_insert(&b.c, 12345, 777, &dup), "re-inserting is not an error");
	CHECK(dup, "re-inserting reports a duplicate");
	COMPARE(&b, &r);

	CHECK(rbi_sparse_remove(&b.c, 12345, 777), "remove finds the pair");
	ref_remove(&r, 12345, 777);
	COMPARE(&b, &r);
	CHECK(b.c.cardinality == 0, "the segment is empty again");
}

/*
 * n = 1, n = RBI_SPARSE_MAX_PAIRS and the 683rd pair that must be refused.
 */
static void
test_boundaries(void)
{
	SBuf		b;
	Ref			r;
	uint32		i;

	phase("boundaries: 1, 682, 683");
	ref_init(&r);
	rbi_sparse_init(&b.c, 0);

	for (i = 0; i < RBI_SPARSE_MAX_PAIRS; i++)
	{
		bool		ok = rbi_sparse_insert(&b.c, 1000 + i, (uint16) (i % 32768), NULL);

		if (!ok)
		{
			CHECK(false, "insert below the limit succeeds");
			break;
		}
		ref_insert(&r, 1000 + i, (uint16) (i % 32768));
	}
	COMPARE(&b, &r);
	CHECK(b.c.cardinality == RBI_SPARSE_MAX_PAIRS, "the segment holds 682 pairs");
	CHECK(rbi_sparse_size(&b.c) == 4100, "682 pairs occupy 4100 bytes");
	CHECK(rbi_sparse_size(&b.c) <= RBI_CONTAINER_MAX_SIZE, "a full segment fits the item bound");
	CHECK(rbi_sparse_size_for(RBI_SPARSE_MAX_PAIRS + 1) > RBI_CONTAINER_MAX_SIZE,
		  "683 pairs would not fit");

	CHECK(!rbi_sparse_insert(&b.c, 999, 1, NULL), "the 683rd pair is refused (before)");
	CHECK(!rbi_sparse_insert(&b.c, 500000, 1, NULL), "the 683rd pair is refused (after)");
	CHECK(!rbi_sparse_insert(&b.c, 1000, 9999, NULL), "the 683rd pair is refused (inside)");
	COMPARE(&b, &r);

	/* a duplicate is still reported as such, and does not count as full */
	{
		bool		dup = false;

		CHECK(rbi_sparse_insert(&b.c, 1000, 0, &dup), "a duplicate in a full segment is fine");
		CHECK(dup, "... and is reported as a duplicate");
	}

	/* one out, one in */
	CHECK(rbi_sparse_remove(&b.c, 1000, 0), "removing from a full segment");
	ref_remove(&r, 1000, 0);
	CHECK(rbi_sparse_insert(&b.c, 77, 3, NULL), "and there is room again");
	ref_insert(&r, 77, 3);
	COMPARE(&b, &r);
	CHECK(b.c.ckey == 77, "the header follows the new first pair");
}

/* Inserting at the front, in the middle and at the back, in every order. */
static void
test_insert_positions(void)
{
	static const uint32 ckeys[] = {50, 10, 90, 30, 70, 20, 80, 40, 60, 10, 90};
	SBuf		b;
	Ref			r;
	uint32		i;

	phase("insert positions");
	ref_init(&r);
	rbi_sparse_init(&b.c, 0);

	for (i = 0; i < lengthof(ckeys); i++)
	{
		uint16		lo = (uint16) (i * 111);

		if (rbi_sparse_insert(&b.c, ckeys[i], lo, NULL))
			ref_insert(&r, ckeys[i], lo);
		COMPARE(&b, &r);
	}

	/* same ckey, descending lo: the pairs still come out ascending */
	for (i = 0; i < 3; i++)
	{
		uint16		lo = (uint16) (3000 - i * 100);

		rbi_sparse_insert(&b.c, 55, lo, NULL);
		ref_insert(&r, 55, lo);
		COMPARE(&b, &r);
	}
	CHECK(rbi_sparse_count(&b.c, 55) == 3, "three members of one ckey");
}

static void
test_remove(void)
{
	SBuf		b;
	Ref			r;
	uint32		i;

	phase("remove");
	ref_init(&r);
	rbi_sparse_init(&b.c, 0);

	for (i = 0; i < 100; i++)
	{
		rbi_sparse_insert(&b.c, 10 + i / 3, (uint16) (i * 7), NULL);
		ref_insert(&r, 10 + i / 3, (uint16) (i * 7));
	}
	COMPARE(&b, &r);

	CHECK(!rbi_sparse_remove(&b.c, 9, 0), "removing a ckey that is not there");
	CHECK(!rbi_sparse_remove(&b.c, 10, 12345), "removing a lo that is not there");
	COMPARE(&b, &r);

	/* remove from the front, the middle and the back */
	while (r.n > 0)
	{
		uint32		idx = (r.n > 2) ? (rng_below(3) == 0 ? 0 :
									   (rng_below(2) ? r.n / 2 : r.n - 1)) : 0;
		uint32		ckey = r.p[idx].ckey;
		uint16		lo = r.p[idx].lo;

		CHECK(rbi_sparse_remove(&b.c, ckey, lo), "remove finds the pair");
		ref_remove(&r, ckey, lo);
		if ((r.n % 7) == 0)
			COMPARE(&b, &r);
	}
	COMPARE(&b, &r);
	CHECK(b.c.cardinality == 0, "everything was removed");
}

/* rbi_sparse_remove_if() against the same predicate applied to the reference. */
typedef struct PredArg
{
	uint32		mod;
	uint32		rem;
	long		ncalls;
} PredArg;

static bool
pred_cb(uint32 ckey, uint16 lo, void *arg)
{
	PredArg    *pa = (PredArg *) arg;

	pa->ncalls++;
	return ((ckey + lo) % pa->mod) == pa->rem;
}

static void
test_remove_if(void)
{
	uint32		mod;

	phase("remove_if");

	for (mod = 1; mod <= 5; mod++)
	{
		SBuf		b;
		Ref			r;
		Ref			expect;
		PredArg		pa;
		uint32		i;
		uint32		removed;
		uint32		expected = 0;

		ref_init(&r);
		rbi_sparse_init(&b.c, 0);
		for (i = 0; i < 400; i++)
		{
			uint32		ckey = 5 + i / 2;
			uint16		lo = (uint16) (i * 13 + mod);

			rbi_sparse_insert(&b.c, ckey, lo, NULL);
			ref_insert(&r, ckey, lo);
		}

		pa.mod = mod;
		pa.rem = 0;
		pa.ncalls = 0;

		expect = r;
		expect.n = 0;
		for (i = 0; i < r.n; i++)
		{
			if (((r.p[i].ckey + r.p[i].lo) % mod) == 0)
				expected++;
			else
				expect.p[expect.n++] = r.p[i];
		}

		removed = rbi_sparse_remove_if(&b.c, pred_cb, &pa);
		CHECK(pa.ncalls == (long) r.n, "the predicate is called once per pair");
		CHECK(removed == expected, "remove_if removes what the predicate says");
		COMPARE(&b, &expect);
	}

	/* removing nothing must not touch the segment */
	{
		SBuf		b;
		Ref			r;
		PredArg		pa;
		uint32		i;

		ref_init(&r);
		rbi_sparse_init(&b.c, 0);
		for (i = 0; i < 50; i++)
		{
			rbi_sparse_insert(&b.c, 100 + i, (uint16) i, NULL);
			ref_insert(&r, 100 + i, (uint16) i);
		}
		pa.mod = 1000000;
		pa.rem = 999999;
		pa.ncalls = 0;
		CHECK(rbi_sparse_remove_if(&b.c, pred_cb, &pa) == 0, "nothing matches");
		COMPARE(&b, &r);

		pa.mod = 1;
		pa.rem = 0;
		CHECK(rbi_sparse_remove_if(&b.c, pred_cb, &pa) == 50, "everything matches");
		ref_init(&r);
		COMPARE(&b, &r);
		CHECK(b.c.cardinality == 0, "the segment is empty");
	}
}

static void
test_extract(void)
{
	SBuf		b;
	SBuf		cont;
	Ref			r;
	Ref			expect;
	uint32		i;
	uint32		n;

	phase("extract");
	ref_init(&r);
	rbi_sparse_init(&b.c, 0);

	/* ckey 20 has three members, its neighbours one each */
	rbi_sparse_insert(&b.c, 10, 5, NULL);
	ref_insert(&r, 10, 5);
	rbi_sparse_insert(&b.c, 20, 300, NULL);
	ref_insert(&r, 20, 300);
	rbi_sparse_insert(&b.c, 20, 100, NULL);
	ref_insert(&r, 20, 100);
	rbi_sparse_insert(&b.c, 20, 200, NULL);
	ref_insert(&r, 20, 200);
	rbi_sparse_insert(&b.c, 30, 7, NULL);
	ref_insert(&r, 30, 7);
	COMPARE(&b, &r);

	n = rbi_sparse_extract(&b.c, 20, &cont.c);
	CHECK(n == 3, "extract moves every member of the ckey");
	CHECK(cont.c.type == RBI_CT_ARRAY, "the extracted container is an array");
	CHECK(cont.c.ckey == 20, "the extracted container has the right ckey");
	CHECK(cont.c.cardinality == 3, "the extracted container has three members");
	CHECK(rbi_container_contains(&cont.c, 100) &&
		  rbi_container_contains(&cont.c, 200) &&
		  rbi_container_contains(&cont.c, 300), "and holds the right members");

	expect = r;
	expect.n = 0;
	for (i = 0; i < r.n; i++)
	{
		if (r.p[i].ckey != 20)
			expect.p[expect.n++] = r.p[i];
	}
	COMPARE(&b, &expect);
	CHECK(rbi_sparse_count(&b.c, 20) == 0, "the ckey is gone from the segment");

	/* extracting a ckey that is not there gives an empty container */
	n = rbi_sparse_extract(&b.c, 25, &cont.c);
	CHECK(n == 0, "extracting an absent ckey moves nothing");
	CHECK(cont.c.cardinality == 0, "and produces an empty container");
	COMPARE(&b, &expect);

	/* extracting the first and the last ckey keeps the header honest */
	n = rbi_sparse_extract(&b.c, 10, &cont.c);
	CHECK(n == 1, "extracting the first ckey");
	expect.n = 0;
	expect.p[expect.n].ckey = 30;
	expect.p[expect.n].lo = 7;
	expect.n++;
	COMPARE(&b, &expect);
	CHECK(b.c.ckey == 30, "the header moved to the new first ckey");

	n = rbi_sparse_extract(&b.c, 30, &cont.c);
	CHECK(n == 1, "extracting the last ckey");
	ref_init(&expect);
	COMPARE(&b, &expect);
}

static void
test_split_half(void)
{
	SBuf		b;
	SBuf		l;
	SBuf		rr;
	Ref			r;
	uint32		i;

	phase("split_half");

	/* an empty or single-pair segment cannot be split */
	ref_init(&r);
	rbi_sparse_init(&b.c, 0);
	CHECK(!rbi_sparse_split_half(&b.c, &l.c, &rr.c), "an empty segment does not split");
	rbi_sparse_insert(&b.c, 1, 1, NULL);
	CHECK(!rbi_sparse_split_half(&b.c, &l.c, &rr.c), "a one-pair segment does not split");

	/* every pair of one ckey: no boundary to split at */
	rbi_sparse_init(&b.c, 0);
	for (i = 0; i < 10; i++)
		rbi_sparse_insert(&b.c, 42, (uint16) i, NULL);
	CHECK(!rbi_sparse_split_half(&b.c, &l.c, &rr.c),
		  "a segment of one ckey does not split");

	/* the normal case: a full segment with three members per ckey */
	ref_init(&r);
	rbi_sparse_init(&b.c, 0);
	for (i = 0; i < RBI_SPARSE_MAX_PAIRS; i++)
	{
		uint32		ckey = 1000 + i / 3;
		uint16		lo = (uint16) (i % 3 + 1);

		rbi_sparse_insert(&b.c, ckey, lo, NULL);
		ref_insert(&r, ckey, lo);
	}
	COMPARE(&b, &r);

	CHECK(rbi_sparse_split_half(&b.c, &l.c, &rr.c), "a full segment splits");
	CHECK(l.c.cardinality + rr.c.cardinality == r.n, "the halves hold every pair");
	CHECK(l.c.cardinality > 0 && rr.c.cardinality > 0, "neither half is empty");
	CHECK(rbi_item_last_ckey(&l.c) < rbi_item_first_ckey(&rr.c),
		  "the halves do not overlap");
	CHECK(rbi_sparse_count(&l.c, rbi_item_last_ckey(&l.c)) +
		  rbi_sparse_count(&rr.c, rbi_item_last_ckey(&l.c)) ==
		  rbi_sparse_count(&l.c, rbi_item_last_ckey(&l.c)),
		  "no ckey ends up in both halves");
	CHECK(l.c.cardinality < RBI_SPARSE_MAX_PAIRS &&
		  rr.c.cardinality < RBI_SPARSE_MAX_PAIRS, "both halves have room");

	{
		Ref			lref;
		Ref			rref;
		uint32		m = l.c.cardinality;

		ref_init(&lref);
		ref_init(&rref);
		for (i = 0; i < r.n; i++)
		{
			if (i < m)
				lref.p[lref.n++] = r.p[i];
			else
				rref.p[rref.n++] = r.p[i];
		}
		COMPARE(&l, &lref);
		COMPARE(&rr, &rref);
	}

	/* the pairs of the boundary ckey are all on one side */
	{
		uint32		bck = rbi_item_last_ckey(&l.c);

		CHECK(rbi_sparse_count(&rr.c, bck) == 0,
			  "the boundary ckey is not in the right half");
	}
}

static void
test_split_at(void)
{
	SBuf		b;
	SBuf		l;
	SBuf		rr;
	Ref			r;
	Ref			lref;
	Ref			rref;
	uint32		i;

	phase("split_at");
	ref_init(&r);
	rbi_sparse_init(&b.c, 0);
	for (i = 0; i < 20; i++)
	{
		rbi_sparse_insert(&b.c, 100 + i * 2, (uint16) (i + 1), NULL);
		ref_insert(&r, 100 + i * 2, (uint16) (i + 1));
	}

	/* split in the middle, at a ckey the segment does not hold */
	rbi_sparse_split_at(&b.c, 121, &l.c, &rr.c);
	ref_init(&lref);
	ref_init(&rref);
	for (i = 0; i < r.n; i++)
	{
		if (r.p[i].ckey < 121)
			lref.p[lref.n++] = r.p[i];
		else
			rref.p[rref.n++] = r.p[i];
	}
	COMPARE(&l, &lref);
	COMPARE(&rr, &rref);
	CHECK(l.c.cardinality + rr.c.cardinality == r.n, "split_at keeps every pair");

	/* split before the first ckey: the left part is empty */
	rbi_sparse_split_at(&b.c, 1, &l.c, &rr.c);
	CHECK(l.c.cardinality == 0, "nothing below the first ckey");
	COMPARE(&rr, &r);

	/* split after the last ckey: the right part is empty */
	rbi_sparse_split_at(&b.c, 100000, &l.c, &rr.c);
	CHECK(rr.c.cardinality == 0, "nothing above the last ckey");
	COMPARE(&l, &r);
}

static void
test_merge(void)
{
	SBuf		a;
	SBuf		b;
	SBuf		out;
	Ref			ra;
	Ref			rb;
	Ref			rout;
	uint32		i;

	phase("merge");
	ref_init(&ra);
	ref_init(&rb);
	rbi_sparse_init(&a.c, 0);
	rbi_sparse_init(&b.c, 0);

	for (i = 0; i < 10; i++)
	{
		rbi_sparse_insert(&a.c, 10 + i, (uint16) i, NULL);
		ref_insert(&ra, 10 + i, (uint16) i);
		rbi_sparse_insert(&b.c, 100 + i, (uint16) (i * 3), NULL);
		ref_insert(&rb, 100 + i, (uint16) (i * 3));
	}

	CHECK(rbi_sparse_merge(&a.c, &b.c, &out.c), "adjacent segments merge");
	rout = ra;
	for (i = 0; i < rb.n; i++)
		rout.p[rout.n++] = rb.p[i];
	COMPARE(&out, &rout);

	CHECK(!rbi_sparse_merge(&b.c, &a.c, &out.c), "the wrong order does not merge");

	/* touching ckeys do not merge: that would put one ckey in two items */
	{
		SBuf		c;

		rbi_sparse_init(&c.c, 0);
		rbi_sparse_insert(&c.c, 19, 1, NULL);
		rbi_sparse_insert(&c.c, 20, 1, NULL);
		CHECK(!rbi_sparse_merge(&a.c, &c.c, &out.c),
			  "segments sharing a ckey do not merge");
	}

	/* too big to merge */
	{
		SBuf		x;
		SBuf		y;

		rbi_sparse_init(&x.c, 0);
		rbi_sparse_init(&y.c, 0);
		for (i = 0; i < 400; i++)
			rbi_sparse_insert(&x.c, 1 + i, (uint16) i, NULL);
		for (i = 0; i < 400; i++)
			rbi_sparse_insert(&y.c, 1000 + i, (uint16) i, NULL);
		CHECK(!rbi_sparse_merge(&x.c, &y.c, &out.c), "800 pairs do not fit one segment");
		for (i = 0; i < 118; i++)
			rbi_sparse_remove(&y.c, 1000 + i, (uint16) i);
		CHECK(rbi_sparse_merge(&x.c, &y.c, &out.c), "682 pairs do");
		CHECK(out.c.cardinality == RBI_SPARSE_MAX_PAIRS, "and fill the segment exactly");
	}

	/* merging with an empty segment */
	{
		SBuf		e;

		rbi_sparse_init(&e.c, 0);
		CHECK(rbi_sparse_merge(&a.c, &e.c, &out.c), "merging an empty tail");
		COMPARE(&out, &ra);
		CHECK(rbi_sparse_merge(&e.c, &a.c, &out.c), "merging an empty head");
		COMPARE(&out, &ra);
	}
}

static void
test_iterate_early_stop(void)
{
	SBuf		b;
	Ref			r;
	IterState	it;
	uint32		i;

	phase("iterate early stop");
	ref_init(&r);
	rbi_sparse_init(&b.c, 0);
	for (i = 0; i < 30; i++)
	{
		rbi_sparse_insert(&b.c, 7 + i, (uint16) (i * 5), NULL);
		ref_insert(&r, 7 + i, (uint16) (i * 5));
	}

	it.ref = &r;
	it.i = 0;
	it.ok = true;
	it.stop_after = 10;
	rbi_sparse_iterate(&b.c, iter_cb, &it);
	CHECK(it.ok, "the pairs seen before the stop were right");
	CHECK(it.i == 10, "iterate() stops when the callback says so");
}

/*
 * check() has to reject corrupted images, not crash on them.
 */
static void
test_check_rejects(void)
{
	SBuf		b;
	const char *err;
	uint32		i;

	phase("check rejects");
	rbi_sparse_init(&b.c, 0);
	for (i = 0; i < 10; i++)
		rbi_sparse_insert(&b.c, 100 + i / 2, (uint16) (i * 3), NULL);

	err = NULL;
	CHECK(rbi_sparse_check(&b.c, rbi_sparse_size(&b.c), &err), "the good image passes");

	{
		SBuf		x = b;

		x.c.type = RBI_CT_ARRAY;
		err = NULL;
		CHECK(!rbi_sparse_check(&x.c, rbi_sparse_size(&b.c), &err) && err != NULL,
			  "a container is not a segment");
	}
	{
		SBuf		x = b;

		x.c.flags = 3;
		err = NULL;
		CHECK(!rbi_sparse_check(&x.c, rbi_sparse_size(&b.c), &err) && err != NULL,
			  "non-zero flags are rejected");
	}
	{
		SBuf		x = b;

		x.c.cardinality = (uint16) (RBI_SPARSE_MAX_PAIRS + 1);
		err = NULL;
		CHECK(!rbi_sparse_check(&x.c, RBI_CONTAINER_MAX_SIZE, &err) && err != NULL,
			  "too many pairs are rejected");
	}
	{
		SBuf		x = b;

		err = NULL;
		CHECK(!rbi_sparse_check(&x.c, rbi_sparse_size(&b.c) - 1, &err) && err != NULL,
			  "a segment that does not fit its slot is rejected");
		err = NULL;
		CHECK(!rbi_sparse_check(&x.c, 4, &err) && err != NULL,
			  "a segment whose header does not fit is rejected");
	}
	{
		SBuf		x = b;

		x.c.ckey = 0;
		err = NULL;
		CHECK(!rbi_sparse_check(&x.c, rbi_sparse_size(&b.c), &err) && err != NULL,
			  "a header key that is not the first ckey is rejected");
	}
	{
		SBuf		x = b;
		uint32	   *ckeys = RBI_SPARSE_CKEYS(&x.c);
		uint32		t = ckeys[0];

		ckeys[0] = ckeys[3];
		ckeys[3] = t;
		x.c.ckey = ckeys[0];
		err = NULL;
		CHECK(!rbi_sparse_check(&x.c, rbi_sparse_size(&b.c), &err) && err != NULL,
			  "unsorted ckeys are rejected");
	}
	{
		SBuf		x = b;
		uint16	   *los = RBI_SPARSE_LOS(&x.c);

		los[1] = los[0];
		err = NULL;
		CHECK(!rbi_sparse_check(&x.c, rbi_sparse_size(&b.c), &err) && err != NULL,
			  "a duplicate pair is rejected");
	}
	{
		SBuf		x = b;
		uint16	   *los = RBI_SPARSE_LOS(&x.c);
		uint16		t;

		/* pairs 0 and 1 share a ckey, so swapping their los is a violation */
		CHECK(RBI_SPARSE_CKEYS_CONST(&x.c)[0] == RBI_SPARSE_CKEYS_CONST(&x.c)[1],
			  "the fixture really has two members of one ckey");
		t = los[0];
		los[0] = los[1];
		los[1] = t;
		err = NULL;
		CHECK(!rbi_sparse_check(&x.c, rbi_sparse_size(&b.c), &err) && err != NULL,
			  "descending los within a ckey are rejected");
	}

	/* the container check must refuse a segment, and say so */
	{
		err = NULL;
		CHECK(!rbi_container_check(&b.c, RBI_CONTAINER_MAX_SIZE, &err) && err != NULL,
			  "rbi_container_check() rejects a sparse segment");
		if (err != NULL)
			CHECK(strstr(err, "sparse") != NULL,
				  "... with a message that names the segment");
	}
}

/*
 * Randomized insert/remove/extract/split/merge sequences against the
 * reference.
 */
static void
test_random_ops(const char *name, uint32 nckeys, uint32 nops, uint64 seed)
{
	SBuf		b;
	Ref			r;
	uint32		op;

	phase(name);
	rng_seed(seed);
	ref_init(&r);
	rbi_sparse_init(&b.c, 0);

	for (op = 0; op < nops; op++)
	{
		uint32		ckey = 1000 + rng_below(nckeys);
		uint16		lo = (uint16) rng_below(RBI_CONTAINER_RANGE);
		uint32		what = rng_below(100);

		if (what < 55)
		{
			bool		dup = false;
			bool		ok = rbi_sparse_insert(&b.c, ckey, lo, &dup);
			bool		refok = ref_insert(&r, ckey, lo);

			if (ok && !dup)
				CHECK(refok, "insert agrees with the reference");
			else if (dup)
				CHECK(!refok && ref_contains(&r, ckey, lo),
					  "a duplicate is a duplicate for the reference too");
			else
				CHECK(r.n == RBI_SPARSE_MAX_PAIRS, "insert only fails when full");
		}
		else if (what < 80)
		{
			bool		ok = rbi_sparse_remove(&b.c, ckey, lo);
			bool		refok = ref_remove(&r, ckey, lo);

			CHECK(ok == refok, "remove agrees with the reference");
		}
		else if (what < 88)
		{
			SBuf		cont;
			uint32		expect = ref_count(&r, ckey);
			uint32		got = rbi_sparse_extract(&b.c, ckey, &cont.c);
			Ref			after;
			uint32		i;

			CHECK(got == expect, "extract moves as many members as the reference has");
			CHECK(cont.c.cardinality == expect, "the container holds them all");

			ref_init(&after);
			for (i = 0; i < r.n; i++)
			{
				if (r.p[i].ckey == ckey)
					CHECK(rbi_container_contains(&cont.c, r.p[i].lo),
						  "the container holds the right members");
				else
					after.p[after.n++] = r.p[i];
			}
			r = after;
		}
		else if (what < 94)
		{
			SBuf		l;
			SBuf		rr;

			if (rbi_sparse_split_half(&b.c, &l.c, &rr.c))
			{
				Ref			lref;
				Ref			rref;
				uint32		i;
				uint32		m = l.c.cardinality;

				CHECK(l.c.cardinality + rr.c.cardinality == r.n,
					  "the halves hold every pair");
				CHECK(rbi_item_last_ckey(&l.c) < rbi_item_first_ckey(&rr.c),
					  "the halves do not overlap");

				ref_init(&lref);
				ref_init(&rref);
				for (i = 0; i < r.n; i++)
				{
					if (i < m)
						lref.p[lref.n++] = r.p[i];
					else
						rref.p[rref.n++] = r.p[i];
				}
				COMPARE(&l, &lref);
				COMPARE(&rr, &rref);

				/* ... and merging them again gives back the original */
				{
					SBuf		out;

					CHECK(rbi_sparse_merge(&l.c, &rr.c, &out.c),
						  "the halves merge back together");
					COMPARE(&out, &r);
				}
			}
			else
				CHECK(r.n < 2 || r.p[0].ckey == r.p[r.n - 1].ckey,
					  "split only fails on one ckey or too few pairs");
		}
		else
		{
			SBuf		l;
			SBuf		rr;
			Ref			lref;
			Ref			rref;
			uint32		i;
			uint32		cut = 1000 + rng_below(nckeys);

			/* split_at needs the ckey gone from the segment */
			{
				SBuf		cont;

				rbi_sparse_extract(&b.c, cut, &cont.c);
				{
					Ref			after;

					ref_init(&after);
					for (i = 0; i < r.n; i++)
					{
						if (r.p[i].ckey != cut)
							after.p[after.n++] = r.p[i];
					}
					r = after;
				}
			}

			rbi_sparse_split_at(&b.c, cut, &l.c, &rr.c);
			ref_init(&lref);
			ref_init(&rref);
			for (i = 0; i < r.n; i++)
			{
				if (r.p[i].ckey < cut)
					lref.p[lref.n++] = r.p[i];
				else
					rref.p[rref.n++] = r.p[i];
			}
			COMPARE(&l, &lref);
			COMPARE(&rr, &rref);
		}

		if ((op % 32) == 0)
			COMPARE(&b, &r);
	}

	COMPARE(&b, &r);
}

/*
 * What the format buys, printed for the record.
 */
static void
report_sizes(void)
{
	printf("\nsizes (bytes):\n");
	printf("  %-44s %6zu\n", "segment header", (size_t) RBI_CONTAINER_HDRSZ);
	printf("  %-44s %6zu\n", "one pair", (size_t) RBI_SPARSE_PAIR_SIZE);
	printf("  %-44s %6zu\n", "1 pair", (size_t) rbi_sparse_size_for(1));
	printf("  %-44s %6zu\n", "3 pairs (one ckey at the threshold - 1)",
		   (size_t) rbi_sparse_size_for(3));
	printf("  %-44s %6zu\n", "682 pairs (full)",
		   (size_t) rbi_sparse_size_for(RBI_SPARSE_MAX_PAIRS));
	printf("  %-44s %6zu\n", "3 one-member ARRAY containers instead",
		   (size_t) (3 * MAXALIGN(RBI_CONTAINER_HDRSZ + sizeof(uint16)) + 3 * 4));
	printf("  %-44s %6zu\n", "682 one-member ARRAY containers instead",
		   (size_t) (682 * MAXALIGN(RBI_CONTAINER_HDRSZ + sizeof(uint16)) + 682 * 4));
}

int
main(void)
{
	printf("roaring_index sparse segment unit tests\n");
	printf("  RBI_SPARSE_MAX_PAIRS=%u RBI_SPARSE_THRESHOLD=%d "
		   "RBI_SPARSE_PAIR_SIZE=%zu\n",
		   RBI_SPARSE_MAX_PAIRS, RBI_SPARSE_THRESHOLD,
		   (size_t) RBI_SPARSE_PAIR_SIZE);

	CHECK(sizeof(RBIContainer) == 8, "RBIContainer header is 8 bytes");
	CHECK(RBI_SPARSE_MAX_PAIRS == 682, "RBI_SPARSE_MAX_PAIRS is 682");
	CHECK(rbi_sparse_size_for(RBI_SPARSE_MAX_PAIRS) <= RBI_CONTAINER_MAX_SIZE,
		  "a full segment fits the common item bound");

	test_empty();
	test_one_pair();
	test_boundaries();
	test_insert_positions();

	rng_seed(UINT64CONST(0x5EED2000));
	test_remove();
	test_remove_if();
	test_extract();
	test_split_half();
	test_split_at();
	test_merge();
	test_iterate_early_stop();
	test_check_rejects();

	test_random_ops("random ops, 8 ckeys (dense)", 8, 20000,
					UINT64CONST(0x5EED2001));
	test_random_ops("random ops, 200 ckeys", 200, 20000,
					UINT64CONST(0x5EED2002));
	test_random_ops("random ops, 5000 ckeys (sparse)", 5000, 20000,
					UINT64CONST(0x5EED2003));
	test_random_ops("random ops, 1 ckey", 1, 5000,
					UINT64CONST(0x5EED2004));

	report_sizes();

	printf("\n%ld checks, %ld pair comparisons against the reference, %ld failures\n",
		   nchecks, npair_cmps, nfail);
	if (nfail == 0)
		printf("ALL TESTS PASSED\n");
	else
		printf("*** %ld TESTS FAILED ***\n", nfail);
	return nfail == 0 ? 0 : 1;
}
