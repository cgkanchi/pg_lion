/*-------------------------------------------------------------------------
 * positions_test.c
 *	  Standalone unit tests for src/lion_positions.c, the position chunks of
 *	  tsvector_pos_ops.
 *
 *	  Built by "make unit PG_CONFIG=..." with -DFRONTEND, so this program
 *	  links against nothing but libc and the chunk library.
 *
 *	  Every chunk under test is shadowed by a brute-force reference: a plain
 *	  sorted array of members.  After each operation the chunk is compared
 *	  against it member by member (code, positions, weights), for its
 *	  cardinality and for find/last_code, and lion_poschunk_check() must
 *	  accept it.  Damaged chunks - random bytes behind a valid header, and
 *	  valid chunks with bytes flipped - are fed to every function, in buffers
 *	  of exactly LION_CONTAINER_MAX_SIZE bytes so that the sanitizer build
 *	  catches a read past them, and what comes out must keep the module's
 *	  promises: codes ascending, at most LION_POS_MAX_NPOS positions, each
 *	  below LION_POS_LIMIT and ascending.
 *
 *	  Exit status is 0 only if every check passed.
 *-------------------------------------------------------------------------
 */
#include "c.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lion_positions.h"

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
			printf("FAIL [%s] positions_test.c:%d: %s\n", cur_phase, line, msg);
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
	return (uint32) ((x * UINT64CONST(0x2545F4914F6CDD1D)) >> 32);
}

static uint32
rng_below(uint32 n)
{
	return n ? rng_next() % n : 0;
}

/* ----------------------------------------------------------------
 *							the reference
 * ----------------------------------------------------------------
 */

#define REF_MAX 4096

typedef struct Ref
{
	uint32		n;
	LionPosMember *m;			/* REF_MAX of them, sorted by code */
} Ref;

static void
ref_init(Ref *r)
{
	r->n = 0;
	r->m = malloc(sizeof(LionPosMember) * REF_MAX);
}

static void
ref_free(Ref *r)
{
	free(r->m);
}

static int
ref_find(const Ref *r, uint64 code)
{
	uint32		i;

	for (i = 0; i < r->n; i++)
		if (r->m[i].code == code)
			return (int) i;
	return -1;
}

/* insert or replace */
static void
ref_put(Ref *r, const LionPosMember *m)
{
	uint32		i = 0;

	while (i < r->n && r->m[i].code < m->code)
		i++;
	if (i < r->n && r->m[i].code == m->code)
	{
		r->m[i] = *m;
		return;
	}
	memmove(&r->m[i + 1], &r->m[i], sizeof(LionPosMember) * (r->n - i));
	r->m[i] = *m;
	r->n++;
}

static bool
member_eq(const LionPosMember *a, const LionPosMember *b)
{
	return a->code == b->code && a->npos == b->npos &&
		memcmp(a->pos, b->pos, sizeof(uint16) * a->npos) == 0;
}

/* A random member at code: positions ascending, random weights. */
static void
rand_member(LionPosMember *m, uint64 code)
{
	uint32		shape = rng_below(20);
	uint32		npos;
	uint32		pos = 0;
	uint32		i;

	m->code = code;
	if (shape == 0)
		npos = 0;				/* stripped */
	else if (shape == 1)
		npos = LION_POS_MAX_NPOS;
	else if (shape < 5)
		npos = 1 + rng_below(20);
	else
		npos = 1 + rng_below(3);
	for (i = 0; i < npos; i++)
	{
		uint32		gap = (shape == 2) ? 1 + rng_below(2000) : 1 + rng_below(12);

		if (i == 0)
			gap = rng_below(shape == 2 ? 3000 : 60);
		if (pos + gap >= LION_POS_LIMIT)
		{
			npos = i;
			break;
		}
		pos += gap;
		m->pos[i] = LION_POS_MAKE(pos, rng_below(4));
	}
	m->npos = (uint16) npos;
}

/* Compare the chunk to the reference, fully. */
static void
compare(const LionContainer *c, const Ref *r, Size itemsz)
{
	LionPosIter it;
	LionPosMember m;
	uint32		i = 0;
	bool		same = true;
	char		err[256];
	uint64		last;

	if (!lion_poschunk_check(c, itemsz, err, sizeof(err)))
	{
		CHECK(false, err);
		return;
	}
	CHECK(c->cardinality == r->n, "cardinality matches the reference");
	lion_poschunk_iter_init(&it, c);
	while (lion_poschunk_iter_next(&it, &m))
	{
		if (i >= r->n || !member_eq(&m, &r->m[i]))
			same = false;
		i++;
	}
	CHECK(same && i == r->n, "iteration matches the reference");
	if (r->n > 0)
	{
		CHECK(c->ckey <= lion_code_ckey(r->m[0].code), "header ckey is a lower bound");
		CHECK(lion_poschunk_last_code(c, &last) && last == r->m[r->n - 1].code,
			  "last_code is the reference's last");
		/* find: a few members and a few misses */
		for (i = 0; i < 4; i++)
		{
			uint32		k = rng_below(r->n);
			uint64		miss = r->m[k].code + 1;

			CHECK(lion_poschunk_find(c, r->m[k].code, &m) && member_eq(&m, &r->m[k]),
				  "find returns the member");
			if (ref_find(r, miss) < 0)
				CHECK(!lion_poschunk_find(c, miss, &m), "find misses an absent code");
		}
	}
	else
		CHECK(!lion_poschunk_last_code(c, &last), "an empty chunk has no last code");
}

typedef struct RemoveArg
{
	uint32		mod;
	uint32		hit;
	uint32		calls;
} RemoveArg;

static bool
remove_cb_pure(const RemoveArg *a, uint64 code)
{
	return (uint32) ((code * UINT64CONST(0x9E3779B97F4A7C15)) >> 40) % a->mod == a->hit;
}

static bool
remove_cb(uint64 code, void *arg)
{
	RemoveArg  *a = (RemoveArg *) arg;

	a->calls++;
	return remove_cb_pure(a, code);
}

static LionContainer *
new_buf(void)
{
	LionContainer *c = malloc(LION_CONTAINER_MAX_SIZE);

	memset(c, 0, LION_CONTAINER_MAX_SIZE);
	return c;
}

/* ----------------------------------------------------------------
 *								tests
 * ----------------------------------------------------------------
 */

static void
test_encoding(void)
{
	LionContainer *c = new_buf();
	LionPosMember m;
	LionPosMember out;
	uint64		base = lion_make_code(7, 0);

	phase("encoding");
	lion_poschunk_init(c, 7);
	CHECK(lion_poschunk_size(c) == LION_POS_HDRSZ, "empty chunk is its header");

	/* lo 5, one position 3 at weight A: 1 + 1 + 1 bytes */
	m.code = base + 5;
	m.npos = 1;
	m.pos[0] = LION_POS_MAKE(3, 3);
	CHECK(lion_posmember_size(base, &m) == 3, "a small member is three bytes");
	CHECK(lion_poschunk_append_need(c, &m) == 3, "append_need agrees");
	CHECK(lion_poschunk_append(c, LION_CONTAINER_MAX_SIZE, &m), "append");
	CHECK(lion_poschunk_size(c) == LION_POS_HDRSZ + 3, "size after append");
	CHECK(lion_poschunk_find(c, base + 5, &out) && member_eq(&out, &m), "round trip");
	CHECK(LION_POS_WEIGHT(out.pos[0]) == 3 && LION_POS_POS(out.pos[0]) == 3, "weight and pos");

	/* not ascending: refused */
	CHECK(!lion_poschunk_append(c, LION_CONTAINER_MAX_SIZE, &m), "append of the same code refused");
	m.code = base + 4;
	CHECK(lion_poschunk_append_need(c, &m) == 0, "append below the last refused");

	/* malformed member: positions out of order */
	m.code = base + 9;
	m.npos = 2;
	m.pos[0] = LION_POS_MAKE(10, 0);
	m.pos[1] = LION_POS_MAKE(10, 1);
	CHECK(!lion_poschunk_append(c, LION_CONTAINER_MAX_SIZE, &m), "duplicate position refused");
	m.npos = LION_POS_MAX_NPOS + 1;
	CHECK(!lion_poschunk_append(c, LION_CONTAINER_MAX_SIZE, &m), "too many positions refused");

	/* extremes: the last position, a stripped member, a far code */
	m.code = base + 10;
	m.npos = 1;
	m.pos[0] = LION_POS_MAKE(LION_POS_LIMIT - 1, 2);
	CHECK(lion_poschunk_append(c, LION_CONTAINER_MAX_SIZE, &m), "position 16383");
	m.code = base + 11;
	m.npos = 0;
	CHECK(lion_poschunk_append(c, LION_CONTAINER_MAX_SIZE, &m), "stripped member");
	m.code = lion_make_code(0xFFFFFFFF, LION_CONTAINER_RANGE - 1);
	m.npos = 1;
	m.pos[0] = LION_POS_MAKE(1, 0);
	CHECK(lion_poschunk_append(c, LION_CONTAINER_MAX_SIZE, &m), "the largest code");
	CHECK(lion_poschunk_check(c, lion_poschunk_size(c), NULL, 0), "check accepts");
	CHECK(c->cardinality == 4, "four members");
	CHECK(lion_poschunk_find(c, base + 11, &out) && out.npos == 0, "stripped round trip");
	CHECK(lion_poschunk_find(c, base + 10, &out) &&
		  LION_POS_POS(out.pos[0]) == LION_POS_LIMIT - 1 && LION_POS_WEIGHT(out.pos[0]) == 2,
		  "last position round trip");

	/* capacity: an append that does not fit leaves the chunk alone */
	{
		Size		before = lion_poschunk_size(c);
		char		save[LION_CONTAINER_MAX_SIZE];

		memcpy(save, c, before);
		m.code = lion_make_code(0xFFFFFFFF, LION_CONTAINER_RANGE - 1) + 1;	/* past 47 bits is fine */
		CHECK(!lion_poschunk_append(c, before + 1, &m), "append past cap refused");
		CHECK(memcmp(save, c, before) == 0, "refused append changed nothing");
	}
	free(c);
}

static void
test_append_random(uint64 seed)
{
	LionContainer *c = new_buf();
	Ref			r;
	LionPosMember m;
	uint64		code;

	phase("append");
	rng_seed(seed);
	ref_init(&r);
	code = lion_make_code(rng_below(1000000), rng_below(LION_CONTAINER_RANGE));
	lion_poschunk_init(c, lion_code_ckey(code));
	for (;;)
	{
		Size		need;
		Size		before = lion_poschunk_size(c);

		rand_member(&m, code);
		need = lion_poschunk_append_need(c, &m);
		CHECK(need > 0, "append_need of an ascending member");
		if (!lion_poschunk_append(c, LION_CONTAINER_MAX_SIZE, &m))
		{
			CHECK(before + need > LION_CONTAINER_MAX_SIZE, "append fails only when full");
			break;
		}
		CHECK(lion_poschunk_size(c) == before + need, "append grew by append_need");
		ref_put(&r, &m);
		/* mostly neighbouring offsets, sometimes another container */
		code += (rng_below(8) == 0) ? 1 + rng_below(200000) : 1 + rng_below(6);
	}
	compare(c, &r, LION_CONTAINER_MAX_SIZE);
	ref_free(&r);
	free(c);
}

static void
test_insert_remove_split(uint64 seed)
{
	LionContainer *c = new_buf();
	LionContainer *l = new_buf();
	LionContainer *rt = new_buf();
	Ref			r;
	LionPosMember m;
	uint32		round;
	uint32		span = 1 + rng_below(3) * 50000;

	phase("insert/remove/split");
	rng_seed(seed);
	ref_init(&r);
	lion_poschunk_init(c, 100);
	for (round = 0; round < 400; round++)
	{
		uint32		op = rng_below(10);

		if (op < 6)
		{
			uint64		code = lion_make_code(100, 0) + rng_below(span * 3 + 64);
			bool		replaced;
			Size		before = lion_poschunk_size(c);
			char		save[LION_CONTAINER_MAX_SIZE];
			Size		cap = (rng_below(4) == 0) ? before + rng_below(16) : LION_CONTAINER_MAX_SIZE;

			if (rng_below(8) == 0 && r.n > 0)
				code = r.m[rng_below(r.n)].code;	/* replace */
			if (rng_below(16) == 0)
				code = rng_below(lion_make_code(100, 0) + 1);	/* below the ckey */
			rand_member(&m, code);
			memcpy(save, c, before);
			if (lion_poschunk_insert(c, cap, &m, &replaced))
			{
				CHECK(replaced == (ref_find(&r, code) >= 0), "replaced flag");
				ref_put(&r, &m);
				CHECK(lion_poschunk_size(c) <= cap, "insert stayed in cap");
			}
			else
				CHECK(memcmp(save, c, before) == 0 && lion_poschunk_size(c) == before,
					  "failed insert changed nothing");
		}
		else if (op < 8)
		{
			/* remove a random subset, then check the freed bytes are zero */
			RemoveArg	ra;
			uint32		removed;
			uint32		i;
			uint32		j = 0;
			Size		before = lion_poschunk_size(c);

			ra.mod = 1 + rng_below(5);
			ra.hit = rng_below(ra.mod);
			ra.calls = 0;
			removed = lion_poschunk_remove(c, remove_cb, &ra);
			CHECK(ra.calls == r.n, "remove asks the callback once per member");
			for (i = 0; i < r.n; i++)
				if (!remove_cb_pure(&ra, r.m[i].code))
					r.m[j++] = r.m[i];
			CHECK(removed == r.n - j, "remove counts what it removed");
			r.n = j;
			CHECK(lion_poschunk_size(c) <= before, "remove never grows the chunk");
			compare(c, &r, before);		/* the freed bytes are zero slack */
			continue;
		}
		else if (r.n >= 2)
		{
			LionPosIter it;
			LionPosMember a;
			uint32		i = 0;
			bool		same = true;

			CHECK(lion_poschunk_split(c, l, rt), "split of two or more members");
			CHECK(l->cardinality >= 1 && rt->cardinality >= 1, "both sides non-empty");
			CHECK(l->cardinality + rt->cardinality == r.n, "split keeps every member");
			CHECK(lion_poschunk_check(l, lion_poschunk_size(l), NULL, 0) &&
				  lion_poschunk_check(rt, lion_poschunk_size(rt), NULL, 0), "both halves check");
			CHECK(l->ckey == c->ckey, "left keeps the ckey");
			lion_poschunk_iter_init(&it, l);
			while (lion_poschunk_iter_next(&it, &a))
				same &= member_eq(&a, &r.m[i++]);
			CHECK(rt->ckey == lion_code_ckey(r.m[i].code), "right takes its first ckey");
			lion_poschunk_iter_init(&it, rt);
			while (lion_poschunk_iter_next(&it, &a))
				same &= (i < r.n) && member_eq(&a, &r.m[i++]);
			CHECK(same && i == r.n, "split halves concatenate to the chunk");
			{
				uint32		big = Max(lion_poschunk_used(l), lion_poschunk_used(rt));

				CHECK(big <= lion_poschunk_used(c) / 2 + LION_POS_MEMBER_MAX + 3,
					  "split is near the middle");
			}
		}
		compare(c, &r, LION_CONTAINER_MAX_SIZE);
	}
	ref_free(&r);
	free(c);
	free(l);
	free(rt);
}

/*
 * Whatever a damaged chunk holds, what the readers hand out keeps the
 * module's promises, and nothing reads or writes past the buffer.
 */
static void
probe_damaged(LionContainer *c)
{
	LionPosIter it;
	LionPosMember m;
	uint64		prev = 0;
	uint32		n = 0;
	bool		ok = true;
	bool		checks;
	LionContainer *l = new_buf();
	LionContainer *rt = new_buf();
	RemoveArg	ra = {3, 1, 0};

	checks = lion_poschunk_check(c, LION_CONTAINER_MAX_SIZE, NULL, 0);
	CHECK(lion_poschunk_size(c) <= LION_CONTAINER_MAX_SIZE, "size is capped");
	lion_poschunk_iter_init(&it, c);
	while (lion_poschunk_iter_next(&it, &m))
	{
		uint32		i;

		if (n > 0 && m.code <= prev)
			ok = false;
		if (m.npos > LION_POS_MAX_NPOS)
			ok = false;
		for (i = 0; i < m.npos && i < LION_POS_MAX_NPOS; i++)
			if (i > 0 && LION_POS_POS(m.pos[i]) <= LION_POS_POS(m.pos[i - 1]))
				ok = false;
		prev = m.code;
		n++;
	}
	CHECK(ok, "a damaged chunk yields only well-formed members");
	CHECK(n <= c->cardinality, "never more members than the header");
	if (checks)
		CHECK(n == c->cardinality, "a chunk check accepts is read whole");

	(void) lion_poschunk_find(c, prev, &m);
	(void) lion_poschunk_last_code(c, &prev);
	(void) lion_poschunk_split(c, l, rt);
	rand_member(&m, prev + 1);
	(void) lion_poschunk_append(c, LION_CONTAINER_MAX_SIZE, &m);
	rand_member(&m, prev / 2);
	(void) lion_poschunk_insert(c, LION_CONTAINER_MAX_SIZE, &m, NULL);
	(void) lion_poschunk_remove(c, remove_cb, &ra);
	CHECK(lion_poschunk_size(c) <= LION_CONTAINER_MAX_SIZE, "mutators stay in the buffer");
	free(l);
	free(rt);
}

static void
test_damaged(uint64 seed)
{
	LionContainer *c = new_buf();
	uint32		round;

	phase("damaged");
	rng_seed(seed);
	for (round = 0; round < 200; round++)
	{
		uint8	   *b = (uint8 *) c;
		uint16		used;
		uint32		i;

		if (rng_below(2) == 0)
		{
			/* random bytes behind a valid header */
			for (i = 0; i < LION_CONTAINER_MAX_SIZE; i++)
				b[i] = (uint8) rng_next();
			c->type = LION_CT_POSITIONS;
			used = (uint16) rng_below(rng_below(2) ? 70000 : 64);
			memcpy(b + LION_CONTAINER_HDRSZ, &used, sizeof(used));
			c->cardinality = (uint16) rng_below(rng_below(2) ? 65536 : 20);
		}
		else
		{
			/* a valid chunk, then a few bytes flipped */
			LionPosMember m;
			uint64		code = lion_make_code(rng_below(1000), 0);
			uint32		nflip = 1 + rng_below(4);

			memset(c, 0, LION_CONTAINER_MAX_SIZE);
			lion_poschunk_init(c, lion_code_ckey(code));
			for (i = 0; i < 1 + rng_below(60); i++)
			{
				code += 1 + rng_below(30);
				rand_member(&m, code);
				if (!lion_poschunk_append(c, LION_CONTAINER_MAX_SIZE, &m))
					break;
			}
			for (i = 0; i < nflip; i++)
			{
				uint32		at = rng_below((uint32) lion_poschunk_size(c));

				b[at] ^= (uint8) (1 + rng_below(255));
			}
			c->type = LION_CT_POSITIONS;
		}
		probe_damaged(c);
	}
	free(c);
}

int
main(void)
{
	uint64		s;

	test_encoding();
	for (s = 1; s <= 200; s++)
		test_append_random(s);
	for (s = 1; s <= 200; s++)
		test_insert_remove_split(s * 7919);
	for (s = 1; s <= 100; s++)
		test_damaged(s * 104729);

	printf("positions_test: %ld checks, %ld failures\n", nchecks, nfail);
	if (nfail == 0)
		printf("ALL TESTS PASSED\n");
	return nfail == 0 ? 0 : 1;
}
