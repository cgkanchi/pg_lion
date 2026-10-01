/*-------------------------------------------------------------------------
 * lion_container.c
 *	  Roaring-style containers for pg_lion.  See DESIGN.md §3.
 *
 *	  Four representations of a set of 15-bit "lo" values:
 *
 *		ARRAY	uint16 lo[cardinality], strictly ascending, cardinality <= 2048
 *		BITSET	uint64 words[512], fixed 4096 bytes
 *		RUN		uint16 nruns, then nruns ascending, non-overlapping and
 *				non-adjacent (start, len_minus_1) pairs, nruns <= 1023
 *		NARROW	uint64 words[128], fixed 1024 bytes: the offsets 0 .. 127 of
 *				each of the 64 heap blocks (DESIGN.md §38, lion_container.h)
 *
 *	  Representation policy (DESIGN.md §3, §38), enforced by the mutators:
 *		- adding to a full ARRAY (2048 members)			=> BITSET
 *		- adding to a RUN that would need a 1024th run	=> ARRAY when the
 *				members, the new one included, fit one (<= 2048), else BITSET
 *		- adding to a NARROW a member at offset >= 128	=> BITSET
 *		- removing from a RUN so that a split would need a 1024th run
 *														=> the same
 *		- removing from a BITSET leaving <= 2048 members	=> ARRAY
 *		- removing from a NARROW leaving <= 512 members	=> ARRAY
 *
 *	  No mutator makes a NARROW: lion_container_optimize() is the only
 *	  function that searches for the globally smallest representation, and
 *	  the only one that chooses NARROW.
 *
 *	  This file depends only on c.h, port/pg_bitutils.h and the project
 *	  headers, so that it also builds standalone with -DFRONTEND for
 *	  test/unit/container_test.c.  No palloc, no elog: the caller's side of
 *	  every contract is Assert()ed, structural damage is reported by
 *	  lion_container_check(), and a damaged container is never trusted with a
 *	  buffer (see "untrusted containers" below).
 *-------------------------------------------------------------------------
 */
/*
 * lion_container.h pulls in lion_tid.h, which needs the server's ItemPointer
 * declarations in a backend build; in a -DFRONTEND build (the unit tests)
 * lion_tid.h reduces itself to the code/ckey/lo arithmetic and c.h is enough.
 */
#ifndef FRONTEND
#include "postgres.h"
#else
#include "c.h"
#endif

#include <string.h>

#include "port/pg_bitutils.h"

#include "lion_container.h"

/*
 * pg_always_inline is the newer spelling of pg_attribute_always_inline, and
 * master's only one; released 16.x and 17.x minors have only the older
 * (lion_compat.h, which this file does not include, says the same).
 */
#ifndef pg_always_inline
#define pg_always_inline pg_attribute_always_inline
#endif

/*
 * The x86-64 kernels of "whole-bitset kernels" below: GCC or clang, whose
 * __attribute__((target)) compiles them for AVX2 and POPCNT whatever -march
 * the rest of the extension is built for, and whose __builtin_cpu_supports()
 * says at run time whether they may run.  LION_NO_SIMD (make
 * LION_NO_SIMD=1) leaves them out, and every build then runs the portable
 * kernel.
 */
#if !defined(LION_NO_SIMD) && defined(__x86_64__) && \
	((defined(__clang__) && __clang_major__ >= 6) || \
	 (!defined(__clang__) && defined(__GNUC__) && __GNUC__ >= 5))
#define LION_HAVE_X86_SIMD 1
#include <immintrin.h>
#endif

/* All-ones 64-bit word. */
#define LION_ALL_ONES		UINT64CONST(0xFFFFFFFFFFFFFFFF)
/* The words of one heap block in a BITSET: 8 at 8K. */
#define LION_BITSET_WORDS_PER_BLOCK	(LION_BITSET_WORDS / LION_BLOCKS_PER_CONTAINER)
/* The offsets of a block, as a mask of lo: 511 at 8K. */
#define LION_OFFSET_MASK	((uint32) ((1 << LION_OFFSET_BITS) - 1))

StaticAssertDecl(LION_BITSET_WORDS_PER_BLOCK * LION_BLOCKS_PER_CONTAINER ==
				 LION_BITSET_WORDS,
				 "pg_lion: bitset words do not divide evenly among heap blocks");
StaticAssertDecl(LION_NARROW_OFFSETS % 64 == 0 &&
				 LION_NARROW_WORDS_PER_BLOCK < LION_BITSET_WORDS_PER_BLOCK,
				 "pg_lion: a NARROW block is whole words, fewer than a BITSET's");
StaticAssertDecl(LION_NARROW_SIZE < LION_CONTAINER_MAX_SIZE,
				 "pg_lion: a NARROW is smaller than a BITSET");
/* Largest legal lo value. */
#define LION_LO_MAX			((uint32) (LION_CONTAINER_RANGE - 1))
/* A size that always loses the "smallest representation" comparison. */
#define LION_SIZE_INFEASIBLE	((Size) (LION_CONTAINER_MAX_SIZE + 1))

/*
 * A work buffer large enough for any container.  The union forces 8-byte
 * alignment so that the bitset payload (offset 8) is aligned as well.
 */
typedef union LionContainerBuf
{
	LionContainer hdr;
	uint64		force_align;
	char		data[LION_CONTAINER_MAX_SIZE];
} LionContainerBuf;


/* ----------------------------------------------------------------
 *						untrusted containers
 *
 * A container read from disk is DATA, and a damaged one must cost a wrong
 * answer or an error somewhere else, never a write outside a buffer.
 * lion_index_verify() runs lion_container_check() over every item, but the
 * readers do not, on purpose: the count engine reads millions of containers
 * per query, and checking each one again would cost about as much as using it
 * (a BITSET's check is a popcount of all 512 words, 0.8 us, which is what an
 * and_cardinality() of two bitsets costs too - 0.12 and 0.14 us with the
 * AVX2 kernels of "whole-bitset kernels").  What a reader does check is the
 * header and the size: lion_inline_fetch() refuses an item whose type is
 * not one of the four or whose lion_item_size() is past the payload or past
 * LION_CONTAINER_MAX_SIZE, and lion_page_item_fetch() does the same for an
 * item on a page, its line pointer included.  So everything in this file is
 * written to be memory-safe for ANY payload behind a header of a valid type,
 * which costs a mask or a comparison where the payload meets an index:
 *
 *	- A count is never trusted to size a loop past the largest legal
 *	  container.  array_card() and run_nruns() clamp the member and run counts
 *	  to LION_ARRAY_MAX_CARD and LION_RUN_MAX_NRUNS, so nothing here reads more
 *	  than LION_CONTAINER_MAX_SIZE bytes from the start of a container
 *	  whatever its header claims - which is what makes a work buffer of that
 *	  size, filled by ItemIdGetLength() rather than by the header, safe to
 *	  hand in - and a mutator first rewrites such a claim to what it is going
 *	  to use (container_clamp()), so that what it leaves behind is never
 *	  larger than LION_CONTAINER_MAX_SIZE either.
 *	- A member never indexes a bitset unmasked: bits_test() and friends take
 *	  lo & LION_LO_MASK.  A run's bounds are clamped to LION_LO_MAX by
 *	  run_last(), and a run that starts past it comes out empty.  Every bit
 *	  of a NARROW's payload is a legal member (an offset below 128), and a
 *	  member at an offset it has no bit for is one it does not hold, which
 *	  narrow_test() answers without an index: a NARROW's header has only its
 *	  cardinality to claim, and that claim is treated as every BITSET's is.
 *	- The header's CARDINALITY is a claim, not a bound.  Every extraction
 *	  into a fixed-size array is bounded by the array, and a representation
 *	  change the claim allows but the payload contradicts (a BITSET that says
 *	  it has 2000 members but holds 5000) is not made.
 *	- Every lo value handed to a caller is below LION_CONTAINER_RANGE, and a
 *	  RUN is walked strictly ascending, skipping what an earlier run already
 *	  covered, so that a caller never sees more than LION_CONTAINER_RANGE
 *	  members of one container - which is the size lion_container_to_array()
 *	  promises its output array needs.  The same goes for what a result of
 *	  the set algebra stores (array_out()), because a result is handed on.
 *
 * For a well-formed container every one of these is a no-op, and every
 * result is byte for byte what it was without them.  For a damaged one the
 * results are unspecified - lion_container_check() is what says why - but
 * deterministic, which WAL replay relies on: redo runs this same code on the
 * same bytes (DESIGN.md §25).  Assert() states the caller's side of each
 * contract - the arguments - and never what a container's bytes say, nor the
 * order of the values the bulk builder is handed, which one of its callers
 * reads off a page; so an assert-enabled build does not stop in here on a
 * damaged page any more than a production one.
 *
 * Measured in instructions (callgrind, -O2, 2026-09-25): and_cardinality()
 * and contains() of every type pair within 0 - 8% of what they were (six more
 * per run where a RUN meets a bitset; none where it meets an ARRAY or a RUN,
 * whose merge loops compare run ends unclamped, run_end()), iterate() one
 * more per ARRAY member, and to_array() of a 1000-member ARRAY 1800 against
 * the 260 of the memcpy() it was.  The lion_container_check() a reader would
 * otherwise need per container is 4700 instructions (0.8 us) for a BITSET
 * and 11800 (1.2 us) for a 1000-member ARRAY: as much as the and_cardinality()
 * it would guard.
 * ----------------------------------------------------------------
 */

/* ----------------------------------------------------------------
 *						payload accessors
 * ----------------------------------------------------------------
 */

static inline uint16 *
array_mdata(LionContainer *c)
{
	return (uint16 *) ((char *) c + LION_CONTAINER_HDRSZ);
}

static inline const uint16 *
array_cdata(const LionContainer *c)
{
	return (const uint16 *) ((const char *) c + LION_CONTAINER_HDRSZ);
}

/*
 * Members of an ARRAY container: the header's count, clamped so that a
 * damaged header can never walk past LION_CONTAINER_MAX_SIZE.
 */
static inline uint32
array_card(const LionContainer *c)
{
	return Min((uint32) c->cardinality, (uint32) LION_ARRAY_MAX_CARD);
}

static inline uint64 *
bitset_mdata(LionContainer *c)
{
	return (uint64 *) ((char *) c + LION_CONTAINER_HDRSZ);
}

static inline const uint64 *
bitset_cdata(const LionContainer *c)
{
	return (const uint64 *) ((const char *) c + LION_CONTAINER_HDRSZ);
}

/* A NARROW's LION_NARROW_WORDS words (DESIGN.md §38). */
static inline uint64 *
narrow_mdata(LionContainer *c)
{
	return (uint64 *) ((char *) c + LION_CONTAINER_HDRSZ);
}

static inline const uint64 *
narrow_cdata(const LionContainer *c)
{
	return (const uint64 *) ((const char *) c + LION_CONTAINER_HDRSZ);
}

/* nruns as stored: for lion_container_check(), the size, and the clamps */
static inline uint32
run_nruns_raw(const LionContainer *c)
{
	return (uint32) *(const uint16 *) ((const char *) c + LION_CONTAINER_HDRSZ);
}

/* Runs of a RUN container, clamped like array_card(). */
static inline uint32
run_nruns(const LionContainer *c)
{
	return Min(run_nruns_raw(c), (uint32) LION_RUN_MAX_NRUNS);
}

static inline void
run_set_nruns(LionContainer *c, uint32 nruns)
{
	Assert(nruns <= LION_RUN_MAX_NRUNS);
	*(uint16 *) ((char *) c + LION_CONTAINER_HDRSZ) = (uint16) nruns;
}

static inline LionRun *
run_mdata(LionContainer *c)
{
	return (LionRun *) ((char *) c + LION_CONTAINER_HDRSZ + sizeof(uint16));
}

static inline const LionRun *
run_cdata(const LionContainer *c)
{
	return (const LionRun *) ((const char *) c + LION_CONTAINER_HDRSZ + sizeof(uint16));
}

/*
 * Last lo value covered by a run, clamped to LION_LO_MAX: a damaged run may
 * claim to reach up to 131070.  A run whose START is past LION_LO_MAX then
 * ends before it starts, and every loop over a run's values (v = start; v <=
 * last) visits nothing; the loops that hand a run to the bitset range
 * primitives skip it explicitly.
 */
static inline int32
run_last(const LionRun *r)
{
	int32		last = (int32) r->start + (int32) r->len_minus_1;

	return Min(last, (int32) LION_LO_MAX);
}

/*
 * The same, NOT clamped: for code that only compares a run's end or adds it
 * up, where a damaged one costs a wrong answer and nothing else.  That is the
 * merge loops of the count engine's hot paths (an ARRAY or RUN against a
 * RUN), which the clamp slowed by up to a fifth.  Anything that indexes,
 * iterates or writes with the value uses run_last().
 */
static inline int32
run_end(const LionRun *r)
{
	return (int32) r->start + (int32) r->len_minus_1;
}

/*
 * Rewrite a count the header claims past its representation's limit to the
 * limit, which is what every function here reads it as anyway.  Mutators
 * call this first, so that the container they leave behind is never larger
 * than LION_CONTAINER_MAX_SIZE, whatever it came in as.  A no-op, and no
 * store at all, for a well-formed container.
 */
static inline void
container_clamp(LionContainer *c)
{
	if (c->type == LION_CT_ARRAY && c->cardinality > LION_ARRAY_MAX_CARD)
		c->cardinality = LION_ARRAY_MAX_CARD;
	else if (c->type == LION_CT_RUN && run_nruns_raw(c) > LION_RUN_MAX_NRUNS)
		run_set_nruns(c, LION_RUN_MAX_NRUNS);
}


/* ----------------------------------------------------------------
 *						whole-bitset kernels
 *
 * Every pass over all LION_BITSET_WORDS words of a bitset that counts what
 * it sees is one call of bits_kernel(d, a, b, op), which reads a (and b),
 * writes d where the op writes, and returns the popcount of what the op
 * counts - in one pass:
 *
 *	LION_BITS_COUNT			|a|						bits_cardinality()
 *	LION_BITS_RUNS			a's runs				bits_count_runs()
 *	LION_BITS_AND_COUNT		|a & b|					and_cardinality()
 *	LION_BITS_AND			d = a & b, |d|			the set algebra's
 *	LION_BITS_OR			d = a | b, |d|			bitset results
 *	LION_BITS_ANDNOT		d = a & ~b, |d|			(container_op_bitset())
 *	LION_BITS_OR_NEW		d = a | b, |b & ~a|		or_inplace()
 *
 * d may be a or b - a word is read before it is written - and otherwise
 * overlaps neither; the ops that write nothing take NULL.  The same passes
 * over a NARROW's LION_NARROW_WORDS words are narrow_kernel(d, a, b, op)
 * (DESIGN.md §38): one kernel of each implementation takes the word count,
 * and runs one of two passes compiled for it, so that a BITSET's is the
 * code it was.
 *
 * Why.  The cardinality of a BITSET x BITSET AND was an AND into a 4 KB
 * image on the stack and a pg_popcount() of it, and the set algebra's
 * bitset results a copy of one operand, a pass folding in the other and a
 * third counting the result; before PostgreSQL 17, pg_popcount() itself
 * calls pg_popcount64() a word at a time, which on x86-64 is a function
 * pointer, 512 indirect calls.  Measured through the library (2026-09-30,
 * Xeon at 2.8 GHz with AVX2 and no VPOPCNTDQ, gcc -O2, PostgreSQL 16's
 * libpgcommon; one pair of bitsets in L1 / 256 pairs, 2 MB), before ->
 * AVX2 kernel, POPCNT kernel, in ns:
 *
 *	and_cardinality()		775 / 878	->	144 / 255,	202 / 277
 *	and_raw()				914 / 1182	->	133 / 390,	211 / 407
 *	or_raw(), andnot_raw()	like and_raw()
 *	and() (optimizing)		1603 / 1814	->	357 / 623,	601 / 809
 *	or_inplace()			876 / 887	->	138 / 252,	318 / 333
 *	bitset_recount()		636 / 641	->	121 / 139,	163 / 166
 *	optimize(), a BITSET	644 / 651	->	184 / 190,	347 / 351
 *
 * PostgreSQL 17 and 18 run POPCNT inside pg_popcount(), which about halves
 * the "before" of the ones that count through it (and_cardinality() 404 /
 * 500 ns, bitset_recount() 290 / 289 against an -O1 build of 18's).
 *
 * Three implementations, one picked at the first call: bits_kernel starts
 * as bits_kernel_choose(), which asks the CPU and overwrites the pointer
 * with its choice, as PostgreSQL's own pg_popcount64 does.
 *
 *	- AVX2 (x86-64): 32 bytes at a time, each byte's popcount looked up a
 *	  nibble at a time in a 16-entry table with VPSHUFB (Mula), the byte
 *	  counts summed over 16 vectors - at most 128, which a byte holds - and
 *	  only then added up across the bytes with VPSADBW;
 *	- POPCNT (x86-64 without AVX2): a word at a time, the builtin compiled
 *	  for the POPCNT instruction;
 *	- portable, on every other target and compiler and under LION_NO_SIMD:
 *	  the same word-at-a-time loop where a word's popcount is an instruction
 *	  (lion_popcount64()), and what each caller did before this kernel -
 *	  pg_popcount() of what it counts - where it is not.
 *
 * No AVX-512, on purpose: Intel's client CPUs do not have it, AMD's Zen 4
 * runs it as two 256-bit halves, and outside L1 a pass waits on the cache,
 * not on the popcount - with the pairs cycling through 2 MB the AVX2 kernel
 * is within a tenth of the POPCNT one above - so a fourth implementation
 * to test would buy little beyond a hot L1.  (pg_popcount() uses VPOPCNTDQ
 * where it finds it, from PostgreSQL 17 on, and the portable kernel on x86
 * counts through it.)
 *
 * The x86-64 kernels are compiled with __attribute__((target)), so the
 * extension builds with the default -march and runs on any x86-64;
 * __builtin_cpu_supports("avx2") also checks that the OS saves the AVX
 * registers.  Every implementation returns the same count and writes the
 * same words: which one runs changes the time a pass takes, nothing else.
 * lion_container_simd_force() makes a -DFRONTEND build use a given one, for
 * test/unit/container_test.c, which compares each against a reference; the
 * unit tests are also built with -DLION_NO_SIMD, which leaves only the
 * portable one (container_test_nosimd).
 * ----------------------------------------------------------------
 */

typedef uint32 (*LionBitsKernel) (uint64 *d, const uint64 *a, const uint64 *b,
								  LionBitsOp op, uint32 nwords);

/*
 * The portable kernel's popcount of a word.  __builtin_popcountll() is an
 * instruction where the build's target has one - CNT on aarch64, POPCNT on
 * an x86 build for a CPU that has it (-mpopcnt, -march=native) - and there
 * the portable kernel is a fused loop like the POPCNT kernel's
 * (LION_POPCOUNT64_INLINE).  On x86 built for the baseline ISA the builtin is
 * a call into libgcc's table lookup, and pg_popcount64() either the POPCNT
 * instruction behind a function pointer (before PostgreSQL 19) or a dozen
 * instructions inline (19 on): a fused loop of those took 640 - 900 ns
 * where the stored result and one pg_popcount() of it take what they did
 * before this kernel (bits_kernel_portable()).  So does a compiler without
 * the builtin.
 */
#if (defined(__GNUC__) || defined(__clang__)) && \
	(defined(__POPCNT__) || !(defined(__x86_64__) || defined(__i386__)))
#define LION_POPCOUNT64_INLINE 1
#define lion_popcount64(w)	((uint32) __builtin_popcountll(w))
#else
#define lion_popcount64(w)	((uint32) pg_popcount64(w))
#endif

/*
 * The word-at-a-time kernels' popcount: POPCNT's own when hw (the POPCNT
 * kernel, compiled for the instruction), else lion_popcount64().
 */
#ifdef LION_HAVE_X86_SIMD
#define bits_popcount(hw, w) \
	((hw) ? (uint32) __builtin_popcountll(w) : lion_popcount64(w))
#else
#define bits_popcount(hw, w)	lion_popcount64(w)
#endif

/*
 * Word i of a pass: writes d[i] for the ops that write, and returns the word
 * whose bits the op counts.
 */
static pg_always_inline uint64
bits_word(uint64 *d, const uint64 *a, const uint64 *b, uint32 i,
		  LionBitsOp op)
{
	uint64		x = a[i];
	uint64		r;

	switch (op)
	{
		case LION_BITS_COUNT:
			return x;
		case LION_BITS_RUNS:
			/* a run starts at a set bit whose predecessor is clear */
			return x & ~((x << 1) | (i > 0 ? a[i - 1] >> 63 : 0));
		case LION_BITS_AND_COUNT:
			return x & b[i];
		case LION_BITS_AND:
			r = x & b[i];
			break;
		case LION_BITS_OR:
			r = x | b[i];
			break;
		case LION_BITS_ANDNOT:
			r = x & ~b[i];
			break;
		case LION_BITS_OR_NEW:
			{
				uint64		y = b[i];

				d[i] = x | y;
				return y & ~x;
			}
		default:
			pg_unreachable();
	}
	d[i] = r;
	return r;
}

/*
 * A pass a word at a time, op and nwords being constants wherever this is
 * inlined.  Four sums, so that neither their adds nor POPCNT's false
 * dependency on its destination (Intel, before Cannon Lake) chain the words:
 * with one sum the POPCNT AND_COUNT took 350 ns, with four 207.
 */
static pg_always_inline uint32
bits_pass_words(uint64 *d, const uint64 *a, const uint64 *b, LionBitsOp op,
				bool hw, uint32 nwords)
{
	uint64		n0 = 0;
	uint64		n1 = 0;
	uint64		n2 = 0;
	uint64		n3 = 0;
	uint32		i;

	for (i = 0; i < nwords; i += 4)
	{
		n0 += bits_popcount(hw, bits_word(d, a, b, i, op));
		n1 += bits_popcount(hw, bits_word(d, a, b, i + 1, op));
		n2 += bits_popcount(hw, bits_word(d, a, b, i + 2, op));
		n3 += bits_popcount(hw, bits_word(d, a, b, i + 3, op));
	}
	return (uint32) (n0 + n1 + n2 + n3);
}

#ifndef LION_POPCOUNT64_INLINE
/*
 * A pass that stores its result in d and counts it with one pg_popcount():
 * the portable kernel's where a word's popcount is not an instruction.
 */
static pg_always_inline uint32
bits_pass_stored(uint64 *d, const uint64 *a, const uint64 *b, LionBitsOp op,
				 uint32 nwords)
{
	uint32		i;

	for (i = 0; i < nwords; i++)
		(void) bits_word(d, a, b, i, op);
	return (uint32) pg_popcount((const char *) d, nwords * sizeof(uint64));
}
#endif

/*
 * The portable kernel.  Where a word's popcount is an instruction
 * (LION_POPCOUNT64_INLINE) every pass is a fused loop, which clang turns
 * into NEON's CNT and pairwise adds on aarch64.  Elsewhere each pass is what
 * its caller did before this kernel: a lone count one pg_popcount(), an op
 * that makes a result stores it (AND_COUNT in an image of its own) and
 * counts it with pg_popcount(), and RUNS and OR_NEW take a pg_popcount64()
 * a word.
 */
static pg_always_inline uint32
bits_portable_pass(uint64 *d, const uint64 *a, const uint64 *b,
				   LionBitsOp op, uint32 nwords)
{
#ifndef LION_POPCOUNT64_INLINE
	uint64		img[LION_BITSET_WORDS];
#endif

	switch (op)
	{
		case LION_BITS_RUNS:
			return bits_pass_words(d, a, b, LION_BITS_RUNS, false, nwords);
		case LION_BITS_OR_NEW:
			return bits_pass_words(d, a, b, LION_BITS_OR_NEW, false, nwords);
#ifdef LION_POPCOUNT64_INLINE
		case LION_BITS_COUNT:
			return bits_pass_words(d, a, b, LION_BITS_COUNT, false, nwords);
		case LION_BITS_AND_COUNT:
			return bits_pass_words(d, a, b, LION_BITS_AND_COUNT, false, nwords);
		case LION_BITS_AND:
			return bits_pass_words(d, a, b, LION_BITS_AND, false, nwords);
		case LION_BITS_OR:
			return bits_pass_words(d, a, b, LION_BITS_OR, false, nwords);
		case LION_BITS_ANDNOT:
			return bits_pass_words(d, a, b, LION_BITS_ANDNOT, false, nwords);
#else
		case LION_BITS_COUNT:
			return (uint32) pg_popcount((const char *) a,
										nwords * sizeof(uint64));
		case LION_BITS_AND_COUNT:
			return bits_pass_stored(img, a, b, LION_BITS_AND, nwords);
		case LION_BITS_AND:
			return bits_pass_stored(d, a, b, LION_BITS_AND, nwords);
		case LION_BITS_OR:
			return bits_pass_stored(d, a, b, LION_BITS_OR, nwords);
		case LION_BITS_ANDNOT:
			return bits_pass_stored(d, a, b, LION_BITS_ANDNOT, nwords);
#endif
	}
	Assert(false);
	return 0;
}

/*
 * Every kernel takes the words a pass covers, which is one of two: a BITSET's
 * LION_BITSET_WORDS or a NARROW's LION_NARROW_WORDS (DESIGN.md §38).  Each
 * runs its pass with the count as a constant, so that the BITSET pass is the
 * code it was before NARROW.
 */
static uint32
bits_kernel_portable(uint64 *d, const uint64 *a, const uint64 *b,
					 LionBitsOp op, uint32 nwords)
{
	if (nwords == LION_NARROW_WORDS)
		return bits_portable_pass(d, a, b, op, LION_NARROW_WORDS);
	Assert(nwords == LION_BITSET_WORDS);
	return bits_portable_pass(d, a, b, op, LION_BITSET_WORDS);
}

#ifdef LION_HAVE_X86_SIMD

#define LION_TARGET(isa)	__attribute__((target(isa)))

/* The POPCNT kernel's pass, inlined into the function compiled for it. */
static pg_always_inline uint32
bits_popcnt_pass(uint64 *d, const uint64 *a, const uint64 *b, LionBitsOp op,
				 uint32 nwords)
{
	switch (op)
	{
		case LION_BITS_COUNT:
			return bits_pass_words(d, a, b, LION_BITS_COUNT, true, nwords);
		case LION_BITS_RUNS:
			return bits_pass_words(d, a, b, LION_BITS_RUNS, true, nwords);
		case LION_BITS_AND_COUNT:
			return bits_pass_words(d, a, b, LION_BITS_AND_COUNT, true, nwords);
		case LION_BITS_AND:
			return bits_pass_words(d, a, b, LION_BITS_AND, true, nwords);
		case LION_BITS_OR:
			return bits_pass_words(d, a, b, LION_BITS_OR, true, nwords);
		case LION_BITS_ANDNOT:
			return bits_pass_words(d, a, b, LION_BITS_ANDNOT, true, nwords);
		case LION_BITS_OR_NEW:
			return bits_pass_words(d, a, b, LION_BITS_OR_NEW, true, nwords);
	}
	Assert(false);
	return 0;
}

static LION_TARGET("popcnt") uint32
bits_kernel_popcnt(uint64 *d, const uint64 *a, const uint64 *b,
				   LionBitsOp op, uint32 nwords)
{
	if (nwords == LION_NARROW_WORDS)
		return bits_popcnt_pass(d, a, b, op, LION_NARROW_WORDS);
	Assert(nwords == LION_BITSET_WORDS);
	return bits_popcnt_pass(d, a, b, op, LION_BITSET_WORDS);
}

/* Words i .. i + 3 of w; a bitset payload is only 8-byte aligned. */
static pg_always_inline LION_TARGET("avx2") __m256i
bits_avx2_load(const uint64 *w, uint32 i)
{
	return _mm256_loadu_si256((const __m256i *) (w + i));
}

static pg_always_inline LION_TARGET("avx2") void
bits_avx2_store(uint64 *w, uint32 i, __m256i v)
{
	_mm256_storeu_si256((__m256i *) (w + i), v);
}

/* The popcount of each byte of v, 0 .. 8: two nibbles looked up (Mula). */
static pg_always_inline LION_TARGET("avx2") __m256i
bits_avx2_byte_counts(__m256i v)
{
	const __m256i lut = _mm256_setr_epi8(0, 1, 1, 2, 1, 2, 2, 3,
										 1, 2, 2, 3, 2, 3, 3, 4,
										 0, 1, 1, 2, 1, 2, 2, 3,
										 1, 2, 2, 3, 2, 3, 3, 4);
	const __m256i nibble = _mm256_set1_epi8(0x0f);
	__m256i		lo = _mm256_and_si256(v, nibble);
	__m256i		hi = _mm256_and_si256(_mm256_srli_epi16(v, 4), nibble);

	return _mm256_add_epi8(_mm256_shuffle_epi8(lut, lo),
						   _mm256_shuffle_epi8(lut, hi));
}

/* Words i .. i + 3 of a pass: bits_word() four words at a time. */
static pg_always_inline LION_TARGET("avx2") __m256i
bits_avx2_vector(uint64 *d, const uint64 *a, const uint64 *b, uint32 i,
				 LionBitsOp op)
{
	__m256i		x = bits_avx2_load(a, i);
	__m256i		r;

	switch (op)
	{
		case LION_BITS_COUNT:
			return x;
		case LION_BITS_RUNS:
			{
				/* the words before a[i .. i + 3]; none before a[0] */
				__m256i		prev = (i > 0) ? bits_avx2_load(a, i - 1) :
					_mm256_setr_epi64x(0, (long long) a[0], (long long) a[1],
									   (long long) a[2]);

				return _mm256_andnot_si256(_mm256_or_si256(_mm256_slli_epi64(x, 1),
														   _mm256_srli_epi64(prev, 63)),
										   x);
			}
		case LION_BITS_AND_COUNT:
			return _mm256_and_si256(x, bits_avx2_load(b, i));
		case LION_BITS_AND:
			r = _mm256_and_si256(x, bits_avx2_load(b, i));
			break;
		case LION_BITS_OR:
			r = _mm256_or_si256(x, bits_avx2_load(b, i));
			break;
		case LION_BITS_ANDNOT:
			r = _mm256_andnot_si256(bits_avx2_load(b, i), x);
			break;
		case LION_BITS_OR_NEW:
			{
				__m256i		y = bits_avx2_load(b, i);

				bits_avx2_store(d, i, _mm256_or_si256(x, y));
				return _mm256_andnot_si256(x, y);
			}
		default:
			pg_unreachable();
	}
	bits_avx2_store(d, i, r);
	return r;
}

/*
 * The words a pass sums in bytes before it adds them up: 16 vectors, or all
 * of a pass of fewer words than that (a NARROW at BLCKSZ 32K, 32 words).
 */
#define LION_AVX2_BLOCK_WORDS(n)	((n) < 64 ? (n) : 64)
StaticAssertDecl(LION_BITSET_WORDS % LION_AVX2_BLOCK_WORDS(LION_BITSET_WORDS) == 0 &&
				 LION_NARROW_WORDS % LION_AVX2_BLOCK_WORDS(LION_NARROW_WORDS) == 0 &&
				 LION_NARROW_WORDS % 4 == 0,
				 "the AVX2 kernel takes whole blocks of whole vectors");

static pg_always_inline LION_TARGET("avx2") uint32
bits_pass_avx2(uint64 *d, const uint64 *a, const uint64 *b, LionBitsOp op,
			   uint32 nwords)
{
	__m256i		total = _mm256_setzero_si256();
	__m128i		sum;
	uint32		i;
	uint32		j;

	for (i = 0; i < nwords; i += LION_AVX2_BLOCK_WORDS(nwords))
	{
		__m256i		bytes = _mm256_setzero_si256();

		for (j = 0; j < LION_AVX2_BLOCK_WORDS(nwords); j += 4)
			bytes = _mm256_add_epi8(bytes,
									bits_avx2_byte_counts(bits_avx2_vector(d, a, b, i + j, op)));
		total = _mm256_add_epi64(total,
								 _mm256_sad_epu8(bytes, _mm256_setzero_si256()));
	}
	sum = _mm_add_epi64(_mm256_castsi256_si128(total),
						_mm256_extracti128_si256(total, 1));
	return (uint32) (_mm_cvtsi128_si64(sum) + _mm_extract_epi64(sum, 1));
}

static pg_always_inline LION_TARGET("avx2") uint32
bits_avx2_pass(uint64 *d, const uint64 *a, const uint64 *b, LionBitsOp op,
			   uint32 nwords)
{
	switch (op)
	{
		case LION_BITS_COUNT:
			return bits_pass_avx2(d, a, b, LION_BITS_COUNT, nwords);
		case LION_BITS_RUNS:
			return bits_pass_avx2(d, a, b, LION_BITS_RUNS, nwords);
		case LION_BITS_AND_COUNT:
			return bits_pass_avx2(d, a, b, LION_BITS_AND_COUNT, nwords);
		case LION_BITS_AND:
			return bits_pass_avx2(d, a, b, LION_BITS_AND, nwords);
		case LION_BITS_OR:
			return bits_pass_avx2(d, a, b, LION_BITS_OR, nwords);
		case LION_BITS_ANDNOT:
			return bits_pass_avx2(d, a, b, LION_BITS_ANDNOT, nwords);
		case LION_BITS_OR_NEW:
			return bits_pass_avx2(d, a, b, LION_BITS_OR_NEW, nwords);
	}
	Assert(false);
	return 0;
}

static LION_TARGET("avx2") uint32
bits_kernel_avx2(uint64 *d, const uint64 *a, const uint64 *b, LionBitsOp op,
				 uint32 nwords)
{
	if (nwords == LION_NARROW_WORDS)
		return bits_avx2_pass(d, a, b, op, LION_NARROW_WORDS);
	Assert(nwords == LION_BITSET_WORDS);
	return bits_avx2_pass(d, a, b, op, LION_BITSET_WORDS);
}

/* The best kernel this CPU runs. */
static LionBitsKernel
bits_kernel_best(void)
{
	/* a no-op after libgcc's constructor, which has run by any first call */
	__builtin_cpu_init();
	if (__builtin_cpu_supports("avx2"))
		return bits_kernel_avx2;
	if (__builtin_cpu_supports("popcnt"))
		return bits_kernel_popcnt;
	return bits_kernel_portable;
}

static uint32 bits_kernel_choose(uint64 *d, const uint64 *a, const uint64 *b,
								 LionBitsOp op, uint32 nwords);

static LionBitsKernel bits_kernel_fn = bits_kernel_choose;

static uint32
bits_kernel_choose(uint64 *d, const uint64 *a, const uint64 *b, LionBitsOp op,
				   uint32 nwords)
{
	bits_kernel_fn = bits_kernel_best();
	return bits_kernel_fn(d, a, b, op, nwords);
}

#else							/* !LION_HAVE_X86_SIMD */

#define bits_kernel_fn(d, a, b, op, n)	bits_kernel_portable((d), (a), (b), (op), (n))

#endif							/* LION_HAVE_X86_SIMD */

/* A pass over a BITSET's words, and over a NARROW's. */
#define bits_kernel(d, a, b, op) \
	bits_kernel_fn((d), (a), (b), (op), LION_BITSET_WORDS)
#define narrow_kernel(d, a, b, op) \
	bits_kernel_fn((d), (a), (b), (op), LION_NARROW_WORDS)

#ifdef FRONTEND
bool
lion_container_simd_force(LionSimdImpl impl)
{
#ifdef LION_HAVE_X86_SIMD
	__builtin_cpu_init();
	switch (impl)
	{
		case LION_SIMD_AUTO:
			bits_kernel_fn = bits_kernel_best();
			return true;
		case LION_SIMD_PORTABLE:
			bits_kernel_fn = bits_kernel_portable;
			return true;
		case LION_SIMD_POPCNT:
			if (!__builtin_cpu_supports("popcnt"))
				return false;
			bits_kernel_fn = bits_kernel_popcnt;
			return true;
		case LION_SIMD_AVX2:
			if (!__builtin_cpu_supports("avx2"))
				return false;
			bits_kernel_fn = bits_kernel_avx2;
			return true;
	}
	return false;
#else
	return impl == LION_SIMD_AUTO || impl == LION_SIMD_PORTABLE;
#endif
}

uint32
lion_container_bits_pass(LionBitsOp op, uint64 *d, const uint64 *a,
						 const uint64 *b)
{
	return bits_kernel(d, a, b, op);
}

uint32
lion_container_narrow_pass(LionBitsOp op, uint64 *d, const uint64 *a,
						   const uint64 *b)
{
	return narrow_kernel(d, a, b, op);
}

LionSimdImpl
lion_container_simd_current(void)
{
#ifdef LION_HAVE_X86_SIMD
	if (bits_kernel_fn == bits_kernel_choose)
		bits_kernel_fn = bits_kernel_best();
	if (bits_kernel_fn == bits_kernel_avx2)
		return LION_SIMD_AVX2;
	if (bits_kernel_fn == bits_kernel_popcnt)
		return LION_SIMD_POPCNT;
#endif
	return LION_SIMD_PORTABLE;
}
#endif							/* FRONTEND */


/* ----------------------------------------------------------------
 *						bitset primitives
 *
 * These all operate on a bare array of LION_BITSET_WORDS uint64s.
 * ----------------------------------------------------------------
 */

/* Word with bits lo .. hi (0-based, inclusive, within one word) set. */
static inline uint64
word_mask(uint32 lo, uint32 hi)
{
	Assert(lo <= hi && hi < 64);
	return (LION_ALL_ONES >> (63 - (hi - lo))) << lo;
}

/*
 * Single-bit primitives.  lo is often an ARRAY member, which is data: a
 * uint16 up to 65535 where only 0 .. 32767 are legal.  Unmasked, a member of
 * 65535 would address word 1023 of a 512-word bitset, 4 KB past its end, so
 * the index is masked instead of asserted.  A legal lo is unchanged by it.
 */
static inline bool
bits_test(const uint64 *w, uint32 lo)
{
	lo &= LION_LO_MASK;
	return (w[lo >> 6] & (UINT64CONST(1) << (lo & 63))) != 0;
}

static inline void
bits_set(uint64 *w, uint32 lo)
{
	lo &= LION_LO_MASK;
	w[lo >> 6] |= UINT64CONST(1) << (lo & 63);
}

static inline void
bits_clear(uint64 *w, uint32 lo)
{
	lo &= LION_LO_MASK;
	w[lo >> 6] &= ~(UINT64CONST(1) << (lo & 63));
}

static inline void
bits_set_range(uint64 *w, uint32 lo, uint32 hi)
{
	uint32		wlo = lo >> 6;
	uint32		whi = hi >> 6;

	Assert(lo <= hi && hi <= LION_LO_MAX);
	if (wlo == whi)
	{
		w[wlo] |= word_mask(lo & 63, hi & 63);
		return;
	}
	w[wlo] |= word_mask(lo & 63, 63);
	if (whi > wlo + 1)
		memset(&w[wlo + 1], 0xFF, (size_t) (whi - wlo - 1) * sizeof(uint64));
	w[whi] |= word_mask(0, hi & 63);
}

static void
bits_clear_range(uint64 *w, uint32 lo, uint32 hi)
{
	uint32		wlo = lo >> 6;
	uint32		whi = hi >> 6;

	Assert(lo <= hi && hi <= LION_LO_MAX);
	if (wlo == whi)
	{
		w[wlo] &= ~word_mask(lo & 63, hi & 63);
		return;
	}
	w[wlo] &= ~word_mask(lo & 63, 63);
	if (whi > wlo + 1)
		memset(&w[wlo + 1], 0x00, (size_t) (whi - wlo - 1) * sizeof(uint64));
	w[whi] &= ~word_mask(0, hi & 63);
}

/*
 * The number of set bits: a kernel pass (LION_BITS_COUNT).  It was one
 * pg_popcount() of the whole bitset rather than a pg_popcount64() a word,
 * which on x86-64 before PostgreSQL 19 is a function pointer, so the loop
 * made 512 indirect calls where pg_popcount() makes one: 768 ns -> 612 ns
 * on PostgreSQL 18 (2026-09-25).  The kernel pass takes 121 ns with AVX2
 * and 163 with POPCNT, against 636 for PostgreSQL 16's pg_popcount() and 290
 * for 18's (2026-09-30, "whole-bitset kernels").
 */
static inline uint32
bits_cardinality(const uint64 *w)
{
	return bits_kernel(NULL, w, NULL, LION_BITS_COUNT);
}

static uint32
bits_range_cardinality(const uint64 *w, uint32 lo, uint32 hi)
{
	uint32		wlo = lo >> 6;
	uint32		whi = hi >> 6;
	uint32		n;
	uint32		i;

	Assert(lo <= hi && hi <= LION_LO_MAX);
	if (wlo == whi)
		return (uint32) pg_popcount64(w[wlo] & word_mask(lo & 63, hi & 63));

	n = (uint32) pg_popcount64(w[wlo] & word_mask(lo & 63, 63));
	for (i = wlo + 1; i < whi; i++)
		n += (uint32) pg_popcount64(w[i]);
	n += (uint32) pg_popcount64(w[whi] & word_mask(0, hi & 63));
	return n;
}

/*
 * Number of maximal runs of consecutive set bits.  A bit starts a run if it
 * is set and its predecessor is not, so count those in a word-parallel way:
 * a kernel pass (LION_BITS_RUNS).  It was a pg_popcount64() a word - an
 * image of the run starts counted by one pg_popcount(), as bits_cardinality()
 * did, cost more than the calls it saved (1.22 us against 0.92,
 * 2026-09-25).  The kernel pass takes 184 ns with AVX2 and 347 with POPCNT,
 * against 644 (2026-09-30: the optimize() of a BITSET that stays one, which
 * is mostly this).
 */
static inline uint32
bits_count_runs(const uint64 *w)
{
	return bits_kernel(NULL, w, NULL, LION_BITS_RUNS);
}

/*
 * The member that bit 0 of word i of a payload stands for: i * 64 in a
 * BITSET's, and the same offset of word i's block in a NARROW's
 * (narrow_lo() below).
 */
static pg_always_inline uint32
words_base(uint32 i, bool narrow)
{
	if (narrow)
		return ((i / LION_NARROW_WORDS_PER_BLOCK) << LION_OFFSET_BITS) |
			((i % LION_NARROW_WORDS_PER_BLOCK) << 6);
	return i << 6;
}

/*
 * Materialise the set bits in ascending order into out, which has room for
 * cap values.  Returns the count, or cap + 1 as soon as there are more set
 * bits than that: the callers size out by a cardinality that a damaged header
 * may understate (DESIGN.md §3, "untrusted containers" above).  The members
 * of a BITSET's payload, or of a NARROW's (narrow_extract_array()).
 */
static pg_always_inline uint32
words_extract_array(const uint64 *w, uint32 nwords, bool narrow,
					uint16 *out, uint32 cap)
{
	uint32		n = 0;
	uint32		i;

	for (i = 0; i < nwords; i++)
	{
		uint64		cur = w[i];
		uint32		base = words_base(i, narrow);

		/* a word adds at most 64: test per word, and count only near cap */
		if (unlikely(n + 64 > cap) && cur != 0 &&
			n + (uint32) pg_popcount64(cur) > cap)
			return cap + 1;
		while (cur != 0)
		{
			out[n++] = (uint16) (base + (uint32) pg_rightmost_one_pos64(cur));
			cur &= cur - 1;
		}
	}
	return n;
}

static uint32
bits_extract_array(const uint64 *w, uint16 *out, uint32 cap)
{
	return words_extract_array(w, LION_BITSET_WORDS, false, out, cap);
}

/*
 * Materialise the runs of members.  At most maxruns are written to out, but
 * the total number of runs is always returned, so the caller can detect that
 * the buffer was too small.  A run is one of members, not of bits: in a
 * NARROW's payload a block's offset 0 follows the block before's offset 127,
 * which as members are two runs.
 */
static pg_always_inline uint32
words_extract_runs(const uint64 *w, uint32 nwords, bool narrow,
				   LionRun *out, uint32 maxruns)
{
	uint32		n = 0;
	int32		rstart = -1;
	int32		rprev = -2;
	uint32		i;

	for (i = 0; i < nwords; i++)
	{
		uint64		cur = w[i];
		int32		base = (int32) words_base(i, narrow);

		while (cur != 0)
		{
			int32		v = base + pg_rightmost_one_pos64(cur);

			cur &= cur - 1;
			if (v == rprev + 1)
			{
				rprev = v;
				continue;
			}
			if (rstart >= 0)
			{
				if (n < maxruns)
				{
					out[n].start = (uint16) rstart;
					out[n].len_minus_1 = (uint16) (rprev - rstart);
				}
				n++;
			}
			rstart = v;
			rprev = v;
		}
	}
	if (rstart >= 0)
	{
		if (n < maxruns)
		{
			out[n].start = (uint16) rstart;
			out[n].len_minus_1 = (uint16) (rprev - rstart);
		}
		n++;
	}
	return n;
}

static uint32
bits_extract_runs(const uint64 *w, LionRun *out, uint32 maxruns)
{
	return words_extract_runs(w, LION_BITSET_WORDS, false, out, maxruns);
}


/* ----------------------------------------------------------------
 *						NARROW primitives
 *
 * A NARROW's payload (DESIGN.md §38, lion_container.h) is LION_NARROW_WORDS
 * words, LION_NARROW_WORDS_PER_BLOCK to a heap block: member lo is bit
 * narrow_bit(lo), and bit i is member narrow_lo(i).  A block's words are the
 * first LION_NARROW_WORDS_PER_BLOCK of its LION_BITSET_WORDS_PER_BLOCK in a
 * BITSET (narrow_bits_word()), so that a NARROW meets a BITSET image a block
 * at a time.  A member at an offset of LION_NARROW_OFFSETS or more has no
 * bit, and a NARROW does not hold it; every bit of a payload is a legal
 * member, so that a damaged NARROW's payload is a set like any other and only
 * its header's cardinality can be wrong ("untrusted containers").
 * ----------------------------------------------------------------
 */

/* lo's bit in a NARROW's payload: lo is in range, and one a NARROW holds. */
static inline uint32
narrow_bit(uint32 lo)
{
	Assert(lo <= LION_LO_MAX && lion_lo_is_narrow(lo));
	return ((lo >> LION_OFFSET_BITS) << LION_NARROW_OFFSET_BITS) |
		(lo & (LION_NARROW_OFFSETS - 1));
}

/* The member bit i of a NARROW's payload stands for. */
static inline uint32
narrow_lo(uint32 bit)
{
	return ((bit >> LION_NARROW_OFFSET_BITS) << LION_OFFSET_BITS) |
		(bit & (LION_NARROW_OFFSETS - 1));
}

/* The word of a BITSET that holds the members of a NARROW's word i. */
static inline uint32
narrow_bits_word(uint32 i)
{
	return (i / LION_NARROW_WORDS_PER_BLOCK) * LION_BITSET_WORDS_PER_BLOCK +
		i % LION_NARROW_WORDS_PER_BLOCK;
}

/* Masked like bits_test(); false, not an index, for an offset of 128 on. */
static inline bool
narrow_test(const uint64 *w, uint32 lo)
{
	uint32		bit;

	lo &= LION_LO_MASK;
	if (!lion_lo_is_narrow(lo))
		return false;
	bit = narrow_bit(lo);
	return (w[bit >> 6] & (UINT64CONST(1) << (bit & 63))) != 0;
}

/* lo is one a NARROW holds: every caller has asked lion_lo_is_narrow(). */
static inline void
narrow_set(uint64 *w, uint32 lo)
{
	uint32		bit = narrow_bit(lo & LION_LO_MASK);

	w[bit >> 6] |= UINT64CONST(1) << (bit & 63);
}

/* A no-op for a member a NARROW cannot hold. */
static inline void
narrow_clear(uint64 *w, uint32 lo)
{
	uint32		bit;

	lo &= LION_LO_MASK;
	if (!lion_lo_is_narrow(lo))
		return;
	bit = narrow_bit(lo);
	w[bit >> 6] &= ~(UINT64CONST(1) << (bit & 63));
}

/*
 * The bits of block b's members among lo .. hi in a NARROW's payload, as
 * *first .. *last; false when the NARROW has none of them there.
 */
static inline bool
narrow_span(uint32 lo, uint32 hi, uint32 b, uint32 *first, uint32 *last)
{
	uint32		f = (b == lo >> LION_OFFSET_BITS) ? (lo & LION_OFFSET_MASK) : 0;
	uint32		l = (b == hi >> LION_OFFSET_BITS) ?
		(hi & LION_OFFSET_MASK) : LION_OFFSET_MASK;

	l = Min(l, (uint32) LION_NARROW_OFFSETS - 1);
	if (f > l)
		return false;
	*first = (b << LION_NARROW_OFFSET_BITS) | f;
	*last = (b << LION_NARROW_OFFSET_BITS) | l;
	return true;
}

/* The members lo .. hi that a NARROW holds, set, cleared or counted. */
static void
narrow_set_range(uint64 *w, uint32 lo, uint32 hi)
{
	uint32		b;
	uint32		first;
	uint32		last;

	Assert(lo <= hi && hi <= LION_LO_MAX);
	for (b = lo >> LION_OFFSET_BITS; b <= hi >> LION_OFFSET_BITS; b++)
		if (narrow_span(lo, hi, b, &first, &last))
			bits_set_range(w, first, last);
}

static void
narrow_clear_range(uint64 *w, uint32 lo, uint32 hi)
{
	uint32		b;
	uint32		first;
	uint32		last;

	Assert(lo <= hi && hi <= LION_LO_MAX);
	for (b = lo >> LION_OFFSET_BITS; b <= hi >> LION_OFFSET_BITS; b++)
		if (narrow_span(lo, hi, b, &first, &last))
			bits_clear_range(w, first, last);
}

static uint32
narrow_range_cardinality(const uint64 *w, uint32 lo, uint32 hi)
{
	uint32		n = 0;
	uint32		b;
	uint32		first;
	uint32		last;

	Assert(lo <= hi && hi <= LION_LO_MAX);
	for (b = lo >> LION_OFFSET_BITS; b <= hi >> LION_OFFSET_BITS; b++)
		if (narrow_span(lo, hi, b, &first, &last))
			n += bits_range_cardinality(w, first, last);
	return n;
}

/* The number of members: a kernel pass over the payload's words. */
static inline uint32
narrow_cardinality(const uint64 *w)
{
	return narrow_kernel(NULL, w, NULL, LION_BITS_COUNT);
}

/*
 * The number of runs of members.  The kernel's RUNS pass counts the runs of
 * the words as they lie, where a block's offset 0 follows the block before's
 * offset 127; as members those are 385 apart, so a block that starts with a
 * member where the block before ends with one is one run more.
 */
static uint32
narrow_count_runs(const uint64 *w)
{
	uint32		n = narrow_kernel(NULL, w, NULL, LION_BITS_RUNS);
	uint32		b;

	for (b = 1; b < LION_BLOCKS_PER_CONTAINER; b++)
		n += (uint32) (w[b * LION_NARROW_WORDS_PER_BLOCK] &
					   (w[b * LION_NARROW_WORDS_PER_BLOCK - 1] >> 63) & 1);
	return n;
}

static uint32
narrow_extract_array(const uint64 *w, uint16 *out, uint32 cap)
{
	return words_extract_array(w, LION_NARROW_WORDS, true, out, cap);
}

static uint32
narrow_extract_runs(const uint64 *w, LionRun *out, uint32 maxruns)
{
	return words_extract_runs(w, LION_NARROW_WORDS, true, out, maxruns);
}

/* nw = the members of the BITSET image w that a NARROW holds. */
static void
narrow_from_bits(const uint64 *w, uint64 *nw)
{
	uint32		i;

	for (i = 0; i < LION_NARROW_WORDS; i++)
		nw[i] = w[narrow_bits_word(i)];
}

/* Does the BITSET image w hold no member that a NARROW cannot? */
static bool
bits_narrow_feasible(const uint64 *w)
{
	uint64		any = 0;
	uint32		b;
	uint32		k;

	for (b = 0; b < LION_BLOCKS_PER_CONTAINER; b++)
		for (k = LION_NARROW_WORDS_PER_BLOCK; k < LION_BITSET_WORDS_PER_BLOCK; k++)
			any |= w[b * LION_BITSET_WORDS_PER_BLOCK + k];
	return any == 0;
}


/* ----------------------------------------------------------------
 *						array primitives
 * ----------------------------------------------------------------
 */

/* First index i with arr[i] >= key (i.e. n if there is none). */
static uint32
array_lower_bound(const uint16 *arr, uint32 n, uint32 key)
{
	uint32		lo = 0;
	uint32		hi = n;

	while (lo < hi)
	{
		uint32		mid = lo + (hi - lo) / 2;

		if (arr[mid] < key)
			lo = mid + 1;
		else
			hi = mid;
	}
	return lo;
}

/* First index i with arr[i] > key. */
static uint32
array_upper_bound(const uint16 *arr, uint32 n, uint32 key)
{
	uint32		lo = 0;
	uint32		hi = n;

	while (lo < hi)
	{
		uint32		mid = lo + (hi - lo) / 2;

		if (arr[mid] <= key)
			lo = mid + 1;
		else
			hi = mid;
	}
	return lo;
}

/*
 * array_lower_bound() without a branch to mispredict at every step: the base
 * moves by a conditional move, and only the loop's own count decides when it
 * ends.  The count engine searches a large ARRAY for each member of a small
 * one at keys that follow no pattern - a group's container against the
 * WHERE's, DESIGN.md §10 - where a mispredicted step costs more than the
 * comparison it decides.  For a well-formed (ascending) array it returns what
 * array_lower_bound() returns.
 */
static inline uint32
array_lower_bound_bl(const uint16 *arr, uint32 n, uint32 key)
{
	const uint16 *base = arr;

	if (n == 0)
		return 0;
	while (n > 1)
	{
		uint32		half = n / 2;

		base = ((uint32) base[half] < key) ? base + half : base;
		n -= half;
	}
	return (uint32) (base - arr) + ((uint32) *base < key ? 1 : 0);
}

/*
 * Exponential ("galloping") search: first index i >= from with arr[i] >= key.
 * Used when intersecting arrays of very different sizes.  The search inside
 * the window the gallop found is the branch-free one.
 */
static uint32
array_gallop(const uint16 *arr, uint32 n, uint32 from, uint32 key)
{
	uint32		lo;
	uint32		hi;
	uint32		step = 1;

	if (from >= n)
		return n;
	if (arr[from] >= key)
		return from;

	lo = from;
	while (from + step < n && arr[from + step] < key)
	{
		lo = from + step;
		step *= 2;
	}
	hi = (from + step < n) ? from + step : n;
	return lo + 1 + array_lower_bound_bl(arr + lo + 1, hi - lo - 1, key);
}


/* ----------------------------------------------------------------
 *						run primitives
 * ----------------------------------------------------------------
 */

/* Index of the rightmost run whose start is <= lo, or -1. */
static int32
run_locate(const LionRun *runs, uint32 nruns, uint32 lo)
{
	int32		lo_i = 0;
	int32		hi_i = (int32) nruns - 1;
	int32		res = -1;

	while (lo_i <= hi_i)
	{
		int32		mid = lo_i + (hi_i - lo_i) / 2;

		if ((uint32) runs[mid].start <= lo)
		{
			res = mid;
			lo_i = mid + 1;
		}
		else
			hi_i = mid - 1;
	}
	return res;
}


/* ----------------------------------------------------------------
 *				container -> bitset image helpers
 *
 * "w" is always a caller-supplied array of LION_BITSET_WORDS uint64s that does
 * not overlap the container's own payload.
 * ----------------------------------------------------------------
 */

/* w |= c; exported as lion_container_or_into_bitset() */
static void
container_or_bitset(const LionContainer *c, uint64 *w)
{
	uint32		i;

	switch (c->type)
	{
		case LION_CT_ARRAY:
			{
				const uint16 *arr = array_cdata(c);
				uint32		n = array_card(c);

				for (i = 0; i < n; i++)
					bits_set(w, arr[i]);
				break;
			}
		case LION_CT_BITSET:
			{
				const uint64 *src = bitset_cdata(c);

				for (i = 0; i < LION_BITSET_WORDS; i++)
					w[i] |= src[i];
				break;
			}
		case LION_CT_RUN:
			{
				const LionRun *runs = run_cdata(c);
				uint32		nruns = run_nruns(c);

				for (i = 0; i < nruns; i++)
				{
					int32		last = run_last(&runs[i]);

					if ((int32) runs[i].start <= last)
						bits_set_range(w, runs[i].start, (uint32) last);
				}
				break;
			}
		case LION_CT_NARROW:
			{
				const uint64 *src = narrow_cdata(c);

				for (i = 0; i < LION_NARROW_WORDS; i++)
					w[narrow_bits_word(i)] |= src[i];
				break;
			}
		default:
			Assert(false);
			break;
	}
}

/* w &= ~c */
static void
container_andnot_bitset(const LionContainer *c, uint64 *w)
{
	uint32		i;

	switch (c->type)
	{
		case LION_CT_ARRAY:
			{
				const uint16 *arr = array_cdata(c);
				uint32		n = array_card(c);

				for (i = 0; i < n; i++)
					bits_clear(w, arr[i]);
				break;
			}
		case LION_CT_BITSET:
			{
				const uint64 *src = bitset_cdata(c);

				for (i = 0; i < LION_BITSET_WORDS; i++)
					w[i] &= ~src[i];
				break;
			}
		case LION_CT_RUN:
			{
				const LionRun *runs = run_cdata(c);
				uint32		nruns = run_nruns(c);

				for (i = 0; i < nruns; i++)
				{
					int32		last = run_last(&runs[i]);

					if ((int32) runs[i].start <= last)
						bits_clear_range(w, runs[i].start, (uint32) last);
				}
				break;
			}
		case LION_CT_NARROW:
			{
				const uint64 *src = narrow_cdata(c);

				for (i = 0; i < LION_NARROW_WORDS; i++)
					w[narrow_bits_word(i)] &= ~src[i];
				break;
			}
		default:
			Assert(false);
			break;
	}
}

/*
 * w &= c.  Only BITSET, RUN and NARROW operands get here: the public entry
 * points intersect ARRAY operands by probing, without materialising a bitset
 * (and an AND with a NARROW makes a NARROW, container_op_narrow(), but this
 * is total).
 */
static void
container_and_bitset(const LionContainer *c, uint64 *w)
{
	uint32		i;

	switch (c->type)
	{
		case LION_CT_BITSET:
			{
				const uint64 *src = bitset_cdata(c);

				for (i = 0; i < LION_BITSET_WORDS; i++)
					w[i] &= src[i];
				break;
			}
		case LION_CT_RUN:
			{
				const LionRun *runs = run_cdata(c);
				uint32		nruns = run_nruns(c);
				uint32		prev = 0;

				/*
				 * Clear the gaps between (and around) the runs.  A run that
				 * starts past LION_LO_MAX is empty (run_last()) and leaves
				 * its gap to the final clear.
				 */
				for (i = 0; i < nruns; i++)
				{
					int32		last = run_last(&runs[i]);

					if ((int32) runs[i].start > last)
						continue;
					if ((uint32) runs[i].start > prev)
						bits_clear_range(w, prev, (uint32) runs[i].start - 1);
					prev = (uint32) last + 1;
				}
				if (prev <= LION_LO_MAX)
					bits_clear_range(w, prev, LION_LO_MAX);
				break;
			}
		case LION_CT_NARROW:
			{
				const uint64 *src = narrow_cdata(c);
				uint32		b;
				uint32		k;

				for (b = 0; b < LION_BLOCKS_PER_CONTAINER; b++)
				{
					uint64	   *bw = &w[b * LION_BITSET_WORDS_PER_BLOCK];

					for (k = 0; k < LION_NARROW_WORDS_PER_BLOCK; k++)
						bw[k] &= src[b * LION_NARROW_WORDS_PER_BLOCK + k];
					for (; k < LION_BITSET_WORDS_PER_BLOCK; k++)
						bw[k] = 0;
				}
				break;
			}
		default:
			Assert(false);
			break;
	}
}

/* w = c */
static void
container_fill_bitset(const LionContainer *c, uint64 *w)
{
	if (c->type == LION_CT_BITSET)
		memcpy(w, bitset_cdata(c), LION_BITSET_BYTES);
	else
	{
		memset(w, 0, LION_BITSET_BYTES);
		container_or_bitset(c, w);
	}
}

/*
 * w = a AND, OR or ANDNOT b (op LION_BITS_AND, _OR or _ANDNOT), and its
 * cardinality: the set algebra's bitset results.  When b is a BITSET, the
 * op and the count are one kernel pass, which reads a's payload itself when
 * a is a BITSET too - the AND of two BITSETs used to be a 4 KB copy of one,
 * a pass ANDing in the other and a third counting the result - and a filled
 * into w otherwise.  Other b's are folded into a filled w, which is then
 * counted.  w overlaps neither operand.
 */
static uint32
container_op_bitset(const LionContainer *a, const LionContainer *b,
					LionBitsOp op, uint64 *w)
{
	if (b->type == LION_CT_BITSET)
	{
		const uint64 *src = w;

		if (a->type == LION_CT_BITSET)
			src = bitset_cdata(a);
		else
			container_fill_bitset(a, w);
		return bits_kernel(w, src, bitset_cdata(b), op);
	}

	container_fill_bitset(a, w);
	switch (op)
	{
		case LION_BITS_AND:
			container_and_bitset(b, w);
			break;
		case LION_BITS_OR:
			container_or_bitset(b, w);
			break;
		case LION_BITS_ANDNOT:
			container_andnot_bitset(b, w);
			break;
		default:
			Assert(false);
			break;
	}
	return bits_cardinality(w);
}

/* Number of maximal runs of consecutive members, whatever the type. */
static uint32
container_count_runs(const LionContainer *c)
{
	switch (c->type)
	{
		case LION_CT_ARRAY:
			{
				const uint16 *arr = array_cdata(c);
				uint32		n = array_card(c);
				uint32		nruns = 0;
				uint32		i;

				for (i = 0; i < n; i++)
					if (i == 0 || (uint32) arr[i] != (uint32) arr[i - 1] + 1)
						nruns++;
				return nruns;
			}
		case LION_CT_BITSET:
			return bits_count_runs(bitset_cdata(c));
		case LION_CT_RUN:
			return run_nruns(c);
		case LION_CT_NARROW:
			return narrow_count_runs(narrow_cdata(c));
		default:
			Assert(false);
			return 0;
	}
}

/*
 * nw = c's members that a NARROW holds - all of them, for a c that
 * lion_container_narrow_feasible() passes - as a NARROW's payload.  nw does
 * not overlap c.
 */
static void
container_fill_narrow(const LionContainer *c, uint64 *nw)
{
	uint32		i;

	switch (c->type)
	{
		case LION_CT_NARROW:
			memcpy(nw, narrow_cdata(c), LION_NARROW_BYTES);
			return;
		case LION_CT_BITSET:
			narrow_from_bits(bitset_cdata(c), nw);
			return;
		default:
			break;
	}

	memset(nw, 0, LION_NARROW_BYTES);
	if (c->type == LION_CT_ARRAY)
	{
		const uint16 *arr = array_cdata(c);
		uint32		n = array_card(c);

		for (i = 0; i < n; i++)
			if (lion_lo_is_narrow(arr[i] & LION_LO_MASK))
				narrow_set(nw, arr[i] & LION_LO_MASK);
	}
	else if (c->type == LION_CT_RUN)
	{
		const LionRun *runs = run_cdata(c);
		uint32		nruns = run_nruns(c);

		for (i = 0; i < nruns; i++)
		{
			int32		last = run_last(&runs[i]);

			if ((int32) runs[i].start <= last)
				narrow_set_range(nw, runs[i].start, (uint32) last);
		}
	}
	else
		Assert(false);
}

/*
 * nw = a AND, OR or ANDNOT b (op LION_BITS_AND, _OR or _ANDNOT), a NARROW's
 * payload, and its cardinality: the set algebra's NARROW results
 * (lion_container.h).  a is a NARROW, and so is b for OR; for AND and ANDNOT
 * b is any container, whose members a NARROW cannot hold change nothing.  One
 * kernel pass over a's payload and b's, or over an image of b's members in
 * nw, which overlaps neither operand.
 */
static uint32
container_op_narrow(const LionContainer *a, const LionContainer *b,
					LionBitsOp op, uint64 *nw)
{
	const uint64 *src = nw;

	Assert(a->type == LION_CT_NARROW);
	Assert(op != LION_BITS_OR || b->type == LION_CT_NARROW);
	if (b->type == LION_CT_NARROW)
		src = narrow_cdata(b);
	else
		container_fill_narrow(b, nw);
	return narrow_kernel(nw, narrow_cdata(a), src, op);
}


/* ----------------------------------------------------------------
 *						representation changes
 * ----------------------------------------------------------------
 */

static void
container_make_bitset(LionContainer *c)
{
	uint64		w[LION_BITSET_WORDS];

	if (c->type == LION_CT_BITSET)
		return;
	container_fill_bitset(c, w);
	c->type = LION_CT_BITSET;
	memcpy(bitset_mdata(c), w, LION_BITSET_BYTES);
}

/*
 * Materialise the members of c in ascending order into out, which has room
 * for cap of them.  Returns the count, or cap + 1 when there are more than
 * that, which only a damaged container can have (a header claiming fewer
 * members than its payload holds, or overlapping runs):
 * lion_container_to_array() passes LION_CONTAINER_RANGE, which no walk below
 * can exceed, and container_make_array() the LION_ARRAY_MAX_CARD its header's
 * cardinality promised.  What comes out is what lion_container_iterate()
 * visits: members masked into range, and each run's values only from past the
 * last value already emitted.
 */
static uint32
container_extract(const LionContainer *c, uint16 *out, uint32 cap)
{
	uint32		n = 0;
	uint32		i;

	switch (c->type)
	{
		case LION_CT_ARRAY:
			{
				const uint16 *arr = array_cdata(c);

				n = array_card(c);
				if (n > cap)
					return cap + 1;

				/*
				 * Copied, then masked four members at a time: a plain loop
				 * of per-member masks is not vectorized at -O2 and made a
				 * 1000-member to_array() 13 times slower than the memcpy()
				 * it replaces (0.4 us against 0.03), four at a time half
				 * that - next to the heap fetch each member then costs.
				 */
				memcpy(out, arr, (size_t) n * sizeof(uint16));
				for (i = 0; i + 4 <= n; i += 4)
				{
					uint64		quad;

					memcpy(&quad, &out[i], sizeof(quad));
					quad &= LION_LO_MASK * UINT64CONST(0x0001000100010001);
					memcpy(&out[i], &quad, sizeof(quad));
				}
				for (; i < n; i++)
					out[i] &= (uint16) LION_LO_MASK;
				break;
			}
		case LION_CT_BITSET:
			n = bits_extract_array(bitset_cdata(c), out, cap);
			break;
		case LION_CT_NARROW:
			n = narrow_extract_array(narrow_cdata(c), out, cap);
			break;
		case LION_CT_RUN:
			{
				const LionRun *runs = run_cdata(c);
				uint32		nruns = run_nruns(c);
				int32		next = 0;	/* first value not emitted yet */

				for (i = 0; i < nruns; i++)
				{
					int32		v = Max((int32) runs[i].start, next);
					int32		last = run_last(&runs[i]);

					if (v > last)
						continue;
					if (n + (uint32) (last - v + 1) > cap)
						return cap + 1;
					for (; v <= last; v++)
						out[n++] = (uint16) v;
					next = last + 1;
				}
				break;
			}
		default:
			Assert(false);
			break;
	}
	return n;
}

/*
 * The ARRAY form of c, whose header says it has at most LION_ARRAY_MAX_CARD
 * members.  A damaged container whose payload holds MORE than that keeps its
 * representation instead: tmp[] is sized by the header's word, and the caller
 * is left with a container that is still what it was, for
 * lion_container_check() to report, rather than a smaller one that has
 * silently lost members.  The cardinality is set to what was extracted, which
 * for a well-formed container is what it already was.
 */
static void
container_make_array(LionContainer *c)
{
	uint16		tmp[LION_ARRAY_MAX_CARD];
	uint32		n;

	if (c->type == LION_CT_ARRAY)
		return;
	Assert(c->cardinality <= LION_ARRAY_MAX_CARD);
	n = container_extract(c, tmp, LION_ARRAY_MAX_CARD);
	if (n > LION_ARRAY_MAX_CARD)
		return;
	c->type = LION_CT_ARRAY;
	c->cardinality = (uint16) n;
	memcpy(array_mdata(c), tmp, (size_t) n * sizeof(uint16));
}

static void
container_make_run(LionContainer *c)
{
	LionRun		tmp[LION_RUN_MAX_NRUNS];
	uint32		n;

	if (c->type == LION_CT_RUN)
		return;
	if (c->type == LION_CT_BITSET)
		n = bits_extract_runs(bitset_cdata(c), tmp, LION_RUN_MAX_NRUNS);
	else if (c->type == LION_CT_NARROW)
		n = narrow_extract_runs(narrow_cdata(c), tmp, LION_RUN_MAX_NRUNS);
	else
	{
		const uint16 *arr = array_cdata(c);
		uint32		card = array_card(c);
		uint32		i;

		n = 0;
		for (i = 0; i < card; i++)
		{
			if (i > 0 && (uint32) arr[i] == (uint32) arr[i - 1] + 1)
			{
				if (n <= LION_RUN_MAX_NRUNS)
					tmp[n - 1].len_minus_1++;
				continue;
			}
			if (n < LION_RUN_MAX_NRUNS)
			{
				tmp[n].start = arr[i];
				tmp[n].len_minus_1 = 0;
			}
			n++;
		}
	}
	Assert(n <= LION_RUN_MAX_NRUNS);
	c->type = LION_CT_RUN;
	run_set_nruns(c, n);
	memcpy(run_mdata(c), tmp, (size_t) n * sizeof(LionRun));
}

/*
 * Rewrite a RUN container's payload from a bitset image holding exactly
 * "card" members (the caller counted them as it set them, so a damaged
 * header has no say here); "w" must not overlap c.  The RUN encoding is kept
 * when the runs still fit, otherwise the smaller of ARRAY (when it is legal)
 * and BITSET is used.  This is the tail of lion_container_remove_if() on a
 * RUN, which per DESIGN.md §3 must never leave a BITSET holding <=
 * LION_ARRAY_MAX_CARD members.
 */
static void
container_rebuild(LionContainer *c, const uint64 *w, uint32 card)
{
	uint32		nruns;

	Assert(card <= LION_CONTAINER_RANGE);
	c->cardinality = (uint16) card;

	nruns = bits_count_runs(w);
	if (nruns <= LION_RUN_MAX_NRUNS)
	{
		LionRun		runs[LION_RUN_MAX_NRUNS];
		uint32		n = bits_extract_runs(w, runs, LION_RUN_MAX_NRUNS);

		Assert(n == nruns);
		c->type = LION_CT_RUN;
		run_set_nruns(c, n);
		memcpy(run_mdata(c), runs, (size_t) n * sizeof(LionRun));
		return;
	}
	if (card <= LION_ARRAY_MAX_CARD)
	{
		uint16		vals[LION_ARRAY_MAX_CARD];
		uint32		n = bits_extract_array(w, vals, LION_ARRAY_MAX_CARD);

		Assert(n == card);
		c->type = LION_CT_ARRAY;
		memcpy(array_mdata(c), vals, (size_t) n * sizeof(uint16));
		return;
	}
	c->type = LION_CT_BITSET;
	memcpy(bitset_mdata(c), w, LION_BITSET_BYTES);
}

/* DESIGN.md §3: a BITSET that has shrunk to <= 2048 members becomes an ARRAY. */
static inline void
container_shrink_bitset(LionContainer *c)
{
	if (c->type == LION_CT_BITSET && c->cardinality <= LION_ARRAY_MAX_CARD)
		container_make_array(c);
}

/*
 * The NARROW form of c, every member of which has an offset below
 * LION_NARROW_OFFSETS (lion_container_narrow_feasible(), which a caller has
 * asked).  The cardinality is what the payload holds, which for a
 * well-formed container is what it already was.
 */
static void
container_make_narrow(LionContainer *c)
{
	uint64		nw[LION_NARROW_WORDS];

	if (c->type == LION_CT_NARROW)
		return;
	container_fill_narrow(c, nw);
	c->type = LION_CT_NARROW;
	memcpy(narrow_mdata(c), nw, LION_NARROW_BYTES);
	c->cardinality = (uint16) narrow_cardinality(nw);
}

/*
 * DESIGN.md §38: a NARROW that has shrunk to LION_NARROW_ARRAY_CARD (512)
 * members or fewer becomes an ARRAY, which is then no larger.
 */
static inline void
container_shrink_narrow(LionContainer *c)
{
	if (c->type == LION_CT_NARROW && c->cardinality <= LION_NARROW_ARRAY_CARD)
		container_make_array(c);
}


/* ----------------------------------------------------------------
 *						size, construction, lookup
 * ----------------------------------------------------------------
 */

Size
lion_container_size_for(LionContainerType type, uint32 cardinality, uint32 nruns)
{
	switch (type)
	{
		case LION_CT_ARRAY:
			return LION_CONTAINER_HDRSZ + (Size) cardinality * sizeof(uint16);
		case LION_CT_BITSET:
			return LION_CONTAINER_HDRSZ + LION_BITSET_BYTES;
		case LION_CT_RUN:
			return LION_CONTAINER_HDRSZ + sizeof(uint16) +
				(Size) nruns * sizeof(LionRun);
		case LION_CT_NARROW:
			return LION_CONTAINER_HDRSZ + LION_NARROW_BYTES;
		case LION_CT_SPARSE:
			/* a segment is not a container: lion_sparse_size() sizes those */
			break;
	}
	Assert(false);
	return LION_CONTAINER_HDRSZ;
}

Size
lion_container_size(const LionContainer *c)
{
	if (c->type == LION_CT_RUN)
		return lion_container_size_for(LION_CT_RUN, c->cardinality,
									   run_nruns_raw(c));
	return lion_container_size_for((LionContainerType) c->type, c->cardinality, 0);
}

void
lion_container_init(LionContainer *c, uint32 ckey)
{
	c->ckey = ckey;
	c->cardinality = 0;
	c->type = LION_CT_ARRAY;
	c->flags = 0;
}

/*
 * GROWTH IN PLACE (lion_container.h): what add() writes when it keeps the
 * representation, as the table there says.  The clamps add() applies first
 * leave an ARRAY or a RUN whose header claims past its limit at the limit,
 * where it converts, so a claim of that or more is 0 here too.
 */
Size
lion_container_inplace_need(const LionContainer *c, uint16 lo)
{
	switch (c->type)
	{
		case LION_CT_BITSET:
			return LION_CONTAINER_MAX_SIZE;
		case LION_CT_ARRAY:
			if (c->cardinality >= LION_ARRAY_MAX_CARD)
				return 0;		/* would become a BITSET */
			return lion_container_size(c) + sizeof(uint16);
		case LION_CT_RUN:
			if (run_nruns_raw(c) >= LION_RUN_MAX_NRUNS)
				return 0;		/* would become an ARRAY or a BITSET */
			return lion_container_size(c) + sizeof(LionRun);
		case LION_CT_NARROW:
			if (!lion_lo_is_narrow((uint32) lo & LION_LO_MASK))
				return 0;		/* would become a BITSET */
			return LION_NARROW_SIZE;
		default:
			return 0;
	}
}

uint32
lion_container_cardinality(const LionContainer *c)
{
	return c->cardinality;
}

bool
lion_container_contains(const LionContainer *c, uint16 lo)
{
	Assert((uint32) lo <= LION_LO_MAX);

	switch (c->type)
	{
		case LION_CT_ARRAY:
			{
				const uint16 *arr = array_cdata(c);
				uint32		n = array_card(c);
				uint32		pos = array_lower_bound(arr, n, lo);

				return pos < n && arr[pos] == lo;
			}
		case LION_CT_BITSET:
			return bits_test(bitset_cdata(c), lo);
		case LION_CT_RUN:
			{
				const LionRun *runs = run_cdata(c);
				int32		idx = run_locate(runs, run_nruns(c), lo);

				return idx >= 0 && (int32) lo <= run_end(&runs[idx]);
			}
		case LION_CT_NARROW:
			return narrow_test(narrow_cdata(c), lo);
		default:
			Assert(false);
			return false;
	}
}

void
lion_container_to_bitset(LionContainer *c)
{
	container_clamp(c);
	container_make_bitset(c);
}

void
lion_container_optimize(LionContainer *c)
{
	lion_container_optimize_ext(c, true);
}

/*
 * NARROW is a candidate when allow_narrow and every member's offset is below
 * LION_NARROW_OFFSETS, and is then never larger than a BITSET: the choice is
 * among the sizes, ties going to ARRAY, then RUN, then NARROW, then BITSET
 * (DESIGN.md §38, "Rules").  An ARRAY of LION_NARROW_ARRAY_CARD (512)
 * members is a NARROW's size, so a NARROW has more members than that.
 */
void
lion_container_optimize_ext(LionContainer *c, bool allow_narrow)
{
	uint32		card;
	uint32		nruns;
	Size		asz;
	Size		rsz;
	Size		nsz;
	Size		bsz;

	container_clamp(c);
	card = c->cardinality;
	nruns = container_count_runs(c);
	bsz = lion_container_size_for(LION_CT_BITSET, card, 0);

	asz = (card <= LION_ARRAY_MAX_CARD)
		? lion_container_size_for(LION_CT_ARRAY, card, 0)
		: LION_SIZE_INFEASIBLE;
	rsz = (nruns <= LION_RUN_MAX_NRUNS)
		? lion_container_size_for(LION_CT_RUN, card, nruns)
		: LION_SIZE_INFEASIBLE;
	nsz = (allow_narrow && lion_container_narrow_feasible(c))
		? lion_container_size_for(LION_CT_NARROW, card, 0)
		: LION_SIZE_INFEASIBLE;

	/* ties prefer ARRAY, then RUN, then NARROW, then BITSET */
	if (asz <= rsz && asz <= nsz && asz <= bsz)
		container_make_array(c);
	else if (rsz <= nsz && rsz <= bsz)
		container_make_run(c);
	else if (nsz <= bsz)
		container_make_narrow(c);
	else
		container_make_bitset(c);

	Assert(lion_container_size(c) <= LION_CONTAINER_MAX_SIZE);
}

/*
 * Members as iterate() takes them: an ARRAY's masked, a run's clamped and
 * one that starts past LION_LO_MAX left out (it is empty).
 */
bool
lion_container_narrow_feasible(const LionContainer *c)
{
	uint32		i;

	switch (c->type)
	{
		case LION_CT_NARROW:
			return true;
		case LION_CT_BITSET:
			return bits_narrow_feasible(bitset_cdata(c));
		case LION_CT_ARRAY:
			{
				const uint16 *arr = array_cdata(c);
				uint32		n = array_card(c);
				uint32		far = 0;

				/* an offset of 128 or more has a bit at or past 128 */
				for (i = 0; i < n; i++)
					far |= (uint32) arr[i] & LION_OFFSET_MASK;
				return far < LION_NARROW_OFFSETS;
			}
		case LION_CT_RUN:
			{
				const LionRun *runs = run_cdata(c);
				uint32		nruns = run_nruns(c);

				for (i = 0; i < nruns; i++)
				{
					int32		last = run_last(&runs[i]);

					if ((int32) runs[i].start > last)
						continue;
					if (((uint32) runs[i].start >> LION_OFFSET_BITS) !=
						((uint32) last >> LION_OFFSET_BITS) ||
						!lion_lo_is_narrow((uint32) last))
						return false;
				}
				return true;
			}
		default:
			return false;
	}
}


/* ----------------------------------------------------------------
 *							mutators
 * ----------------------------------------------------------------
 */

static bool
run_add(LionContainer *c, uint32 lo)
{
	LionRun	   *runs = run_mdata(c);
	int32		nruns = (int32) run_nruns(c);
	int32		idx = run_locate(runs, (uint32) nruns, lo);
	int32		v = (int32) lo;

	if (idx >= 0)
	{
		int32		last = run_last(&runs[idx]);

		if (v <= last)
			return false;		/* already a member */
		if (v == last + 1)
		{
			/* extend run idx to the right */
			runs[idx].len_minus_1++;
			/* and merge with the next run if that closed the gap */
			if (idx + 1 < nruns && (int32) runs[idx + 1].start == v + 1)
			{
				runs[idx].len_minus_1 = (uint16)
					(run_last(&runs[idx + 1]) - (int32) runs[idx].start);
				memmove(&runs[idx + 1], &runs[idx + 2],
						(size_t) (nruns - idx - 2) * sizeof(LionRun));
				run_set_nruns(c, (uint32) (nruns - 1));
			}
			c->cardinality++;
			return true;
		}
	}

	/* extend the following run to the left? */
	if (idx + 1 < nruns && (int32) runs[idx + 1].start == v + 1)
	{
		runs[idx + 1].start = (uint16) v;
		runs[idx + 1].len_minus_1++;
		c->cardinality++;
		return true;
	}

	/*
	 * A brand new one-element run is needed.  When there is no room for a
	 * 1024th run, DESIGN.md §3: the container becomes an ARRAY if the members
	 * fit one with the new member in, which is always smaller than a BITSET
	 * (8 + 2 * 2048 = 4104 at worst), and a BITSET otherwise.  Converting
	 * straight to a BITSET, as this did before, turned 20 runs of 10 plus
	 * 1003 scattered inserts - a 4102-byte RUN of 1203 members - into a
	 * 4104-byte BITSET on the 1004th, where a 2416-byte ARRAY would do, and
	 * the insert path does not optimize afterwards.
	 *
	 * Neither conversion is ever made on a page item in place: the in-place
	 * insert (lion_insert_container_inplace()) and its redo
	 * (LION_OP_CONTAINER_ADD) only take a RUN below LION_RUN_MAX_NRUNS runs,
	 * which never gets here (lion_container.h, "growth in place").  The
	 * general path works in a LION_CONTAINER_MAX_SIZE buffer and writes back
	 * whatever size results.
	 */
	if (nruns >= (int32) LION_RUN_MAX_NRUNS)
	{
		if (c->cardinality < LION_ARRAY_MAX_CARD)
			container_make_array(c);
		if (c->type == LION_CT_RUN)		/* too many members, or damaged */
			container_make_bitset(c);
		return lion_container_add(c, (uint16) lo);
	}
	memmove(&runs[idx + 2], &runs[idx + 1],
			(size_t) (nruns - idx - 1) * sizeof(LionRun));
	runs[idx + 1].start = (uint16) v;
	runs[idx + 1].len_minus_1 = 0;
	run_set_nruns(c, (uint32) (nruns + 1));
	c->cardinality++;
	return true;
}

static bool
run_remove(LionContainer *c, uint32 lo)
{
	LionRun	   *runs = run_mdata(c);
	int32		nruns = (int32) run_nruns(c);
	int32		idx = run_locate(runs, (uint32) nruns, lo);
	int32		v = (int32) lo;
	int32		start;
	int32		last;

	if (idx < 0)
		return false;
	start = (int32) runs[idx].start;
	last = run_last(&runs[idx]);
	if (v > last)
		return false;

	if (start == last)
	{
		/* the whole run disappears */
		memmove(&runs[idx], &runs[idx + 1],
				(size_t) (nruns - idx - 1) * sizeof(LionRun));
		run_set_nruns(c, (uint32) (nruns - 1));
	}
	else if (v == start)
	{
		runs[idx].start = (uint16) (start + 1);
		runs[idx].len_minus_1--;
	}
	else if (v == last)
	{
		runs[idx].len_minus_1--;
	}
	else
	{
		/*
		 * The run splits in two.  With no room for a 1024th run the container
		 * changes representation exactly as run_add() does: an ARRAY if the
		 * members fit one before the removal, and otherwise a BITSET, which
		 * the removal then shrinks to an ARRAY if it can
		 * (container_shrink_bitset()).  Either way a RUN of 1023 runs never
		 * becomes a BITSET of <= 2048 members.
		 */
		if (nruns >= (int32) LION_RUN_MAX_NRUNS)
		{
			if (c->cardinality <= LION_ARRAY_MAX_CARD)
				container_make_array(c);
			if (c->type == LION_CT_RUN)
				container_make_bitset(c);
			return lion_container_remove(c, (uint16) lo);
		}
		memmove(&runs[idx + 2], &runs[idx + 1],
				(size_t) (nruns - idx - 1) * sizeof(LionRun));
		runs[idx].len_minus_1 = (uint16) (v - 1 - start);
		runs[idx + 1].start = (uint16) (v + 1);
		runs[idx + 1].len_minus_1 = (uint16) (last - v - 1);
		run_set_nruns(c, (uint32) (nruns + 1));
	}
	c->cardinality--;
	return true;
}

bool
lion_container_add(LionContainer *c, uint16 lo)
{
	Assert((uint32) lo <= LION_LO_MAX);

	container_clamp(c);
	switch (c->type)
	{
		case LION_CT_ARRAY:
			{
				uint16	   *arr = array_mdata(c);
				uint32		n = c->cardinality;
				uint32		pos = array_lower_bound(arr, n, lo);

				if (pos < n && arr[pos] == lo)
					return false;
				if (n >= LION_ARRAY_MAX_CARD)
				{
					container_make_bitset(c);
					bits_set(bitset_mdata(c), lo);
					c->cardinality++;
					return true;
				}
				memmove(&arr[pos + 1], &arr[pos],
						(size_t) (n - pos) * sizeof(uint16));
				arr[pos] = lo;
				c->cardinality++;
				return true;
			}
		case LION_CT_BITSET:
			{
				uint64	   *w = bitset_mdata(c);

				if (bits_test(w, lo))
					return false;
				bits_set(w, lo);
				c->cardinality++;
				return true;
			}
		case LION_CT_RUN:
			return run_add(c, lo);
		case LION_CT_NARROW:
			{
				uint64	   *w = narrow_mdata(c);

				if (!lion_lo_is_narrow(lo))
				{
					/* a member it has no bit for: not one it holds */
					container_make_bitset(c);
					bits_set(bitset_mdata(c), lo);
					c->cardinality++;
					return true;
				}
				if (narrow_test(w, lo))
					return false;
				narrow_set(w, lo);
				c->cardinality++;
				return true;
			}
		default:
			Assert(false);
			return false;
	}
}

bool
lion_container_remove(LionContainer *c, uint16 lo)
{
	Assert((uint32) lo <= LION_LO_MAX);

	container_clamp(c);
	switch (c->type)
	{
		case LION_CT_ARRAY:
			{
				uint16	   *arr = array_mdata(c);
				uint32		n = c->cardinality;
				uint32		pos = array_lower_bound(arr, n, lo);

				if (pos >= n || arr[pos] != lo)
					return false;
				memmove(&arr[pos], &arr[pos + 1],
						(size_t) (n - pos - 1) * sizeof(uint16));
				c->cardinality--;
				return true;
			}
		case LION_CT_BITSET:
			{
				uint64	   *w = bitset_mdata(c);

				if (!bits_test(w, lo))
					return false;
				bits_clear(w, lo);
				c->cardinality--;
				container_shrink_bitset(c);
				return true;
			}
		case LION_CT_RUN:
			return run_remove(c, lo);
		case LION_CT_NARROW:
			{
				uint64	   *w = narrow_mdata(c);

				if (!narrow_test(w, lo))
					return false;
				narrow_clear(w, lo);
				c->cardinality--;
				container_shrink_narrow(c);
				return true;
			}
		default:
			Assert(false);
			return false;
	}
}

/*
 * The builder is promised ascending, unique values in range, and a well-formed
 * input keeps that promise; but one of its callers expands a sparse segment
 * straight off a page (lion_cursor_emit_segment()), whose pairs are data.  So
 * the promise is checked rather than Assert()ed, as everything here that
 * meets disk bytes is ("untrusted containers"): a value is masked into range,
 * one that does not ascend is added the slow way - which keeps the members
 * sorted and unique whatever order they came in - and one a BITSET already
 * holds is not counted twice.  For a well-formed input that is one comparison
 * per member and the same bytes as before.
 */
void
lion_container_append_sorted(LionContainer *c, uint16 lo)
{
	lo &= (uint16) LION_LO_MASK;

	if (c->type == LION_CT_ARRAY)
	{
		uint16	   *arr = array_mdata(c);
		uint32		n = c->cardinality;

		if (unlikely(n > 0 && n <= LION_ARRAY_MAX_CARD && arr[n - 1] >= lo))
		{
			(void) lion_container_add(c, lo);
			return;
		}
		if (n < LION_ARRAY_MAX_CARD)
		{
			arr[n] = lo;
			c->cardinality = (uint16) (n + 1);
			return;
		}
		container_make_bitset(c);
	}

	if (c->type == LION_CT_BITSET)
	{
		uint64	   *w = bitset_mdata(c);

		if (unlikely(bits_test(w, lo)))
			return;
		bits_set(w, lo);
		c->cardinality++;
		return;
	}

	/* A builder never produces a RUN or a NARROW, but stay total. */
	Assert(c->type == LION_CT_RUN || c->type == LION_CT_NARROW);
	(void) lion_container_add(c, lo);
}


/* ----------------------------------------------------------------
 *							iteration
 * ----------------------------------------------------------------
 */

void
lion_container_iterate(const LionContainer *c, lion_lo_callback cb, void *arg)
{
	uint32		i;

	switch (c->type)
	{
		case LION_CT_ARRAY:
			{
				const uint16 *arr = array_cdata(c);
				uint32		n = array_card(c);

				/* masked: a caller's per-block arrays are indexed by lo */
				for (i = 0; i < n; i++)
					if (!cb((uint16) (arr[i] & LION_LO_MASK), arg))
						return;
				break;
			}
		case LION_CT_BITSET:
			{
				const uint64 *w = bitset_cdata(c);

				for (i = 0; i < LION_BITSET_WORDS; i++)
				{
					uint64		cur = w[i];
					uint32		base = i << 6;

					while (cur != 0)
					{
						uint32		lo = base + (uint32) pg_rightmost_one_pos64(cur);

						cur &= cur - 1;
						if (!cb((uint16) lo, arg))
							return;
					}
				}
				break;
			}
		case LION_CT_NARROW:
			{
				const uint64 *w = narrow_cdata(c);

				for (i = 0; i < LION_NARROW_WORDS; i++)
				{
					uint64		cur = w[i];
					uint32		base = words_base(i, true);

					while (cur != 0)
					{
						uint32		lo = base + (uint32) pg_rightmost_one_pos64(cur);

						cur &= cur - 1;
						if (!cb((uint16) lo, arg))
							return;
					}
				}
				break;
			}
		case LION_CT_RUN:
			{
				const LionRun *runs = run_cdata(c);
				uint32		nruns = run_nruns(c);
				int32		next = 0;	/* first value not visited yet */

				/*
				 * Strictly ascending even if the runs overlap, so that no
				 * container yields more than LION_CONTAINER_RANGE values
				 * (container_extract() walks the same way).
				 */
				for (i = 0; i < nruns; i++)
				{
					int32		v = Max((int32) runs[i].start, next);
					int32		last = run_last(&runs[i]);

					if (v > last)
						continue;
					for (; v <= last; v++)
						if (!cb((uint16) v, arg))
							return;
					next = last + 1;
				}
				break;
			}
		default:
			Assert(false);
			break;
	}
}

/*
 * No container yields more than LION_CONTAINER_RANGE values, damaged or not
 * (container_extract()), so out never overflows; a well-formed one yields
 * exactly its cardinality.
 */
uint32
lion_container_to_array(const LionContainer *c, uint16 *out)
{
	uint32		n = container_extract(c, out, LION_CONTAINER_RANGE);

	Assert(n <= LION_CONTAINER_RANGE);
	return n;
}


/* ----------------------------------------------------------------
 *				images for the count engine
 *
 * Two readers the count engine (lion_count.h) used to carry copies of, and
 * which met page bytes without the masks above: an ARRAY member or a RUN past
 * the range wrote up to 12 KiB past a 4 KiB image, or set block bits past
 * LION_BLOCKS_PER_CONTAINER through undefined shifts (2026-09-27 review).
 * They live here so that they are written, and unit-tested, like everything
 * else that reads a container.
 * ----------------------------------------------------------------
 */

void
lion_container_or_into_bitset(const LionContainer *c, uint64 *w)
{
	container_or_bitset(c, w);
}

/*
 * DENSE ACCUMULATION (DESIGN.md §32, "Summed ranges: dense and probed").
 *
 * The union of many small containers of one container key - a range's sets,
 * collected - was folded one lion_container_or() at a time: each built a
 * bitset image of the union so far, set the newcomer's bits, counted the
 * whole image, optimized it and copied it out, a 4 KB round trip per
 * member once the union was a bitset, and a merge of the whole array per
 * member before that.  The three functions below let a caller keep the
 * union in the form it grows in instead, and optimize it once at the end.
 */

/*
 * acc |= c in place, for an acc that is a BITSET of the caller's (its full
 * LION_CONTAINER_MAX_SIZE bytes): c's members are set in acc's words and
 * acc's cardinality is raised by the ones that were new, which is what is
 * returned.  acc stays a BITSET whatever it now holds; the caller optimizes
 * it when it is done.  c may come off a page: its members are taken as
 * iterate() takes them, and nothing outside acc's words is written.
 */
uint32
lion_container_or_inplace(LionContainer *acc, const LionContainer *c)
{
	uint64	   *w = bitset_mdata(acc);
	uint32		added = 0;
	uint32		i;

	Assert(acc->type == LION_CT_BITSET);

	switch (c->type)
	{
		case LION_CT_ARRAY:
			{
				const uint16 *arr = array_cdata(c);
				uint32		n = array_card(c);

				for (i = 0; i < n; i++)
				{
					uint32		lo = arr[i] & LION_LO_MASK;
					uint64		bit = UINT64CONST(1) << (lo & 63);

					if ((w[lo >> 6] & bit) == 0)
					{
						w[lo >> 6] |= bit;
						added++;
					}
				}
				break;
			}
		case LION_CT_BITSET:
			/* the OR and the count of what was new, one kernel pass */
			added = bits_kernel(w, w, bitset_cdata(c), LION_BITS_OR_NEW);
			break;
		case LION_CT_RUN:
			{
				const LionRun *runs = run_cdata(c);
				uint32		nruns = run_nruns(c);

				/*
				 * Overlapping runs (a damaged container) count what the
				 * earlier ones set as already there, so nothing is counted
				 * twice.
				 */
				for (i = 0; i < nruns; i++)
				{
					int32		last = run_last(&runs[i]);
					uint32		had;

					if ((int32) runs[i].start > last)
						continue;
					had = bits_range_cardinality(w, runs[i].start, (uint32) last);
					bits_set_range(w, runs[i].start, (uint32) last);
					added += (uint32) (last - (int32) runs[i].start + 1) - had;
				}
				break;
			}
		case LION_CT_NARROW:
			{
				const uint64 *src = narrow_cdata(c);

				for (i = 0; i < LION_NARROW_WORDS; i++)
				{
					uint64	   *dw = &w[narrow_bits_word(i)];

					added += lion_popcount64(src[i] & ~*dw);
					*dw |= src[i];
				}
				break;
			}
		default:
			Assert(false);
			break;
	}

	acc->cardinality = (uint16) Min((uint32) acc->cardinality + added,
									(uint32) LION_CONTAINER_RANGE);
	return added;
}

/*
 * acc ∪= the n values of vals: in any order, repeats allowed, each masked into
 * range.  acc has LION_CONTAINER_MAX_SIZE bytes of capacity and comes back
 * optimized, which is its cardinality returned; w is the caller's image of
 * LION_BITSET_WORDS words, which must not overlap acc.  One pass over acc and
 * the values and one over the image, however many values there are: what a
 * union whose members arrive a few at a time flushes a batch of them with.
 */
uint32
lion_container_add_many(LionContainer *acc, const uint16 *vals, uint32 n,
						uint64 *w)
{
	uint32		card;
	uint32		i;

	container_clamp(acc);
	container_fill_bitset(acc, w);
	for (i = 0; i < n; i++)
		bits_set(w, vals[i]);
	card = bits_cardinality(w);

	acc->type = LION_CT_BITSET;
	acc->flags = 0;
	memcpy(bitset_mdata(acc), w, LION_BITSET_BYTES);
	acc->cardinality = (uint16) card;
	lion_container_optimize(acc);
	return acc->cardinality;
}

/*
 * Which of the values sorted[0 .. n - 1] c holds: bit j of marks, which has
 * (n + 63) / 64 words, is set for every j whose value is a member of c, and
 * no other bit is written.  Returns how many of the bits it set were clear.
 * sorted must be strictly ascending and below LION_CONTAINER_RANGE, and n at
 * most LION_CONTAINER_RANGE: the members of an ARRAY container the caller
 * holds, whose rows it asks a range about (DESIGN.md §32, "probed").  c may
 * come off a page and be damaged; which bits it then sets is unspecified,
 * but none past bit n - 1.
 *
 * The smaller side drives: an ARRAY no larger than a few times n is walked
 * member by member, each looked up in sorted by a galloping search from
 * where the last one was found; otherwise every value of sorted is looked up
 * in c - a bit test in a BITSET, a merge step against a RUN's runs, a binary
 * search in a large ARRAY.
 */
uint32
lion_container_mark_members(const LionContainer *c, const uint16 *sorted,
							uint32 n, uint64 *marks)
{
	uint32		added = 0;
	uint32		j;

#define MARK(j) \
	do { \
		uint64		bit_ = UINT64CONST(1) << ((j) & 63); \
		if ((marks[(j) >> 6] & bit_) == 0) \
		{ \
			marks[(j) >> 6] |= bit_; \
			added++; \
		} \
	} while (0)

	Assert(n <= LION_CONTAINER_RANGE);
	if (n == 0)
		return 0;

	switch (c->type)
	{
		case LION_CT_ARRAY:
			{
				const uint16 *arr = array_cdata(c);
				uint32		na = array_card(c);
				uint32		i;

				if (na <= 4 * n)
				{
					j = 0;
					for (i = 0; i < na && j < n; i++)
					{
						uint32		lo = arr[i] & LION_LO_MASK;

						j = array_gallop(sorted, n, j, lo);
						if (j < n && (uint32) sorted[j] == lo)
							MARK(j);
					}
				}
				else
				{
					for (j = 0; j < n; j++)
					{
						uint32		pos = array_lower_bound(arr, na, sorted[j]);

						if (pos < na && (uint32) (arr[pos] & LION_LO_MASK) ==
							(uint32) sorted[j])
							MARK(j);
					}
				}
				break;
			}
		case LION_CT_BITSET:
			{
				const uint64 *w = bitset_cdata(c);

				for (j = 0; j < n; j++)
					if (bits_test(w, sorted[j]))
						MARK(j);
				break;
			}
		case LION_CT_NARROW:
			{
				const uint64 *w = narrow_cdata(c);

				for (j = 0; j < n; j++)
					if (narrow_test(w, sorted[j]))
						MARK(j);
				break;
			}
		case LION_CT_RUN:
			{
				const LionRun *runs = run_cdata(c);
				uint32		nruns = run_nruns(c);
				uint32		r = 0;

				/* both ascending: one merge of the values against the runs */
				for (j = 0; j < n && r < nruns; j++)
				{
					int32		lo = (int32) sorted[j];

					while (r < nruns && run_last(&runs[r]) < lo)
						r++;
					if (r < nruns && (int32) runs[r].start <= lo)
						MARK(j);
				}
				break;
			}
		default:
			Assert(false);
			break;
	}
#undef MARK

	return added;
}

/*
 * One pass over the container, whatever its representation.  The obvious
 * alternative - lion_container_range_cardinality() once per block range - is
 * LION_BLOCKS_PER_CONTAINER binary searches whether the container holds three
 * members or thirty-two thousand, and that cost is paid even when the answer
 * is going to be "all of it is all-visible, count the cardinality".
 *
 * A member is masked and a run clamped exactly as iterate() takes them, so
 * the bit of every block that iterate() hands out a member of is set - the
 * count engine queues exactly those members for a heap recheck, and a block
 * missing here would have its members counted without a look at the
 * visibility map - and no bit at or above LION_BLOCKS_PER_CONTAINER ever is:
 * the engine indexes an array of that many flags with them.
 */
/* the bits of the LION_BLOCKS_PER_CONTAINER blocks, all 64 at BLCKSZ 8K */
#define LION_BLOCK_BITS		(LION_ALL_ONES >> (64 - LION_BLOCKS_PER_CONTAINER))

uint64
lion_container_block_mask(const LionContainer *c)
{
	uint64		mask = 0;
	uint32		i;

	switch (c->type)
	{
		case LION_CT_ARRAY:
			{
				const uint16 *arr = array_cdata(c);
				uint32		n = array_card(c);

				for (i = 0; i < n; i++)
					mask |= UINT64CONST(1) <<
						((arr[i] & LION_LO_MASK) >> LION_OFFSET_BITS);
				break;
			}
		case LION_CT_BITSET:
			{
				const uint64 *w = bitset_cdata(c);
				uint32		b;

				for (b = 0; b < LION_BLOCKS_PER_CONTAINER; b++)
				{
					uint64		any = 0;
					uint32		k;

					for (k = 0; k < LION_BITSET_WORDS_PER_BLOCK; k++)
						any |= w[b * LION_BITSET_WORDS_PER_BLOCK + k];
					if (any != 0)
						mask |= UINT64CONST(1) << b;
				}
				break;
			}
		case LION_CT_NARROW:
			{
				const uint64 *w = narrow_cdata(c);
				uint32		b;

				for (b = 0; b < LION_BLOCKS_PER_CONTAINER; b++)
				{
					uint64		any = 0;
					uint32		k;

					for (k = 0; k < LION_NARROW_WORDS_PER_BLOCK; k++)
						any |= w[b * LION_NARROW_WORDS_PER_BLOCK + k];
					if (any != 0)
						mask |= UINT64CONST(1) << b;
				}
				break;
			}
		case LION_CT_RUN:
			{
				const LionRun *runs = run_cdata(c);
				uint32		nruns = run_nruns(c);

				for (i = 0; i < nruns; i++)
				{
					int32		last = run_last(&runs[i]);
					uint32		first_blk;
					uint32		last_blk;

					if ((int32) runs[i].start > last)
						continue;	/* starts past the range: empty */
					first_blk = (uint32) runs[i].start >> LION_OFFSET_BITS;
					last_blk = (uint32) last >> LION_OFFSET_BITS;
					mask |= (LION_ALL_ONES >> (63 - last_blk)) &
						(LION_ALL_ONES << first_blk);
				}
				break;
			}
		default:
			Assert(false);
			break;
	}

	/* a no-op, given the above; the engine's flag array is that long */
	return mask & LION_BLOCK_BITS;
}


/* ----------------------------------------------------------------
 *					bulk removal and range helpers
 * ----------------------------------------------------------------
 */

uint32
lion_container_remove_if(LionContainer *c, lion_lo_predicate pred, void *arg)
{
	uint32		removed = 0;
	uint32		i;

	container_clamp(c);
	switch (c->type)
	{
		case LION_CT_ARRAY:
			{
				uint16	   *arr = array_mdata(c);
				uint32		n = c->cardinality;
				uint32		keep = 0;

				for (i = 0; i < n; i++)
				{
					if (pred((uint16) (arr[i] & LION_LO_MASK), arg))
						removed++;
					else
						arr[keep++] = arr[i];
				}
				c->cardinality = (uint16) keep;
				break;
			}
		case LION_CT_BITSET:
			{
				uint64	   *w = bitset_mdata(c);

				for (i = 0; i < LION_BITSET_WORDS; i++)
				{
					uint64		cur = w[i];
					uint64		keep = cur;
					uint32		base = i << 6;

					while (cur != 0)
					{
						int			b = pg_rightmost_one_pos64(cur);

						cur &= cur - 1;
						if (pred((uint16) (base + (uint32) b), arg))
						{
							keep &= ~(UINT64CONST(1) << b);
							removed++;
						}
					}
					w[i] = keep;
				}
				/* a damaged header can understate; it wraps, verify says so */
				c->cardinality -= (uint16) removed;
				container_shrink_bitset(c);
				break;
			}
		case LION_CT_NARROW:
			{
				uint64	   *w = narrow_mdata(c);

				for (i = 0; i < LION_NARROW_WORDS; i++)
				{
					uint64		cur = w[i];
					uint64		keep = cur;
					uint32		base = words_base(i, true);

					while (cur != 0)
					{
						int			b = pg_rightmost_one_pos64(cur);

						cur &= cur - 1;
						if (pred((uint16) (base + (uint32) b), arg))
						{
							keep &= ~(UINT64CONST(1) << b);
							removed++;
						}
					}
					w[i] = keep;
				}
				/* as a BITSET's: a damaged header wraps, verify says so */
				c->cardinality -= (uint16) removed;
				container_shrink_narrow(c);
				break;
			}
		case LION_CT_RUN:
			{
				uint64		w[LION_BITSET_WORDS];
				const LionRun *runs = run_cdata(c);
				uint32		nruns = run_nruns(c);
				uint32		kept = 0;
				int32		next = 0;

				memset(w, 0, LION_BITSET_BYTES);
				for (i = 0; i < nruns; i++)
				{
					int32		v = Max((int32) runs[i].start, next);
					int32		last = run_last(&runs[i]);

					if (v > last)
						continue;
					for (; v <= last; v++)
					{
						if (pred((uint16) v, arg))
							removed++;
						else
						{
							bits_set(w, (uint32) v);
							kept++;
						}
					}
					next = last + 1;
				}

				/*
				 * Nothing removed, which is VACUUM's common case: the runs
				 * are what they were, and rebuilding them from the image
				 * would only write the same bytes back (two passes over the
				 * 4 KB image and a copy).
				 */
				if (removed == 0)
					return 0;
				container_rebuild(c, w, kept);
				break;
			}
		default:
			Assert(false);
			break;
	}
	Assert(lion_container_size(c) <= LION_CONTAINER_MAX_SIZE);
	return removed;
}

uint32
lion_container_range_cardinality(const LionContainer *c, uint16 lo_start, uint16 lo_end)
{
	Assert((uint32) lo_start <= LION_LO_MAX && (uint32) lo_end <= LION_LO_MAX);
	if (lo_start > lo_end || c->cardinality == 0)
		return 0;

	switch (c->type)
	{
		case LION_CT_ARRAY:
			{
				const uint16 *arr = array_cdata(c);
				uint32		n = array_card(c);
				uint32		from = array_lower_bound(arr, n, lo_start);
				uint32		to = array_upper_bound(arr, n, lo_end);

				/* only an unsorted (damaged) array can have to < from */
				return (to > from) ? to - from : 0;
			}
		case LION_CT_BITSET:
			return bits_range_cardinality(bitset_cdata(c), lo_start, lo_end);
		case LION_CT_NARROW:
			return narrow_range_cardinality(narrow_cdata(c), lo_start, lo_end);
		case LION_CT_RUN:
			{
				const LionRun *runs = run_cdata(c);
				int32		nruns = (int32) run_nruns(c);
				int32		i = run_locate(runs, (uint32) nruns, lo_start);
				uint32		n = 0;

				if (i < 0 || run_end(&runs[i]) < (int32) lo_start)
					i++;
				for (; i < nruns && (int32) runs[i].start <= (int32) lo_end; i++)
				{
					int32		s = Max((int32) runs[i].start, (int32) lo_start);
					int32		e = Min(run_end(&runs[i]), (int32) lo_end);

					if (s <= e)
						n += (uint32) (e - s + 1);
				}
				return n;
			}
		default:
			Assert(false);
			return 0;
	}
}

/*
 * remove_range() the representation-independent way: through a BITSET, which
 * the removal then shrinks to an ARRAY if it can.  For a RUN whose split
 * would need a 1024th run, and for one whose runs are not in order, which
 * only a damaged container has and the run arithmetic below assumes.
 */
static uint32
container_remove_range_bitset(LionContainer *c, uint16 lo_start, uint16 lo_end)
{
	uint32		removed;

	container_make_bitset(c);
	removed = bits_range_cardinality(bitset_mdata(c), lo_start, lo_end);
	bits_clear_range(bitset_mdata(c), lo_start, lo_end);
	c->cardinality -= (uint16) removed;
	container_shrink_bitset(c);
	return removed;
}

uint32
lion_container_remove_range(LionContainer *c, uint16 lo_start, uint16 lo_end)
{
	uint32		removed;

	Assert((uint32) lo_start <= LION_LO_MAX && (uint32) lo_end <= LION_LO_MAX);

	/* first, like every mutator: an early return leaves a container too */
	container_clamp(c);
	if (lo_start > lo_end || c->cardinality == 0)
		return 0;

	switch (c->type)
	{
		case LION_CT_ARRAY:
			{
				uint16	   *arr = array_mdata(c);
				uint32		n = c->cardinality;
				uint32		from = array_lower_bound(arr, n, lo_start);
				uint32		to = array_upper_bound(arr, n, lo_end);

				/*
				 * Only an unsorted (damaged) array can have to < from, and
				 * the memmove below would then write past the array.
				 */
				removed = (to > from) ? to - from : 0;
				if (removed > 0)
				{
					memmove(&arr[from], &arr[to],
							(size_t) (n - to) * sizeof(uint16));
					c->cardinality = (uint16) (n - removed);
				}
				break;
			}
		case LION_CT_BITSET:
			{
				uint64	   *w = bitset_mdata(c);

				removed = bits_range_cardinality(w, lo_start, lo_end);
				if (removed > 0)
				{
					bits_clear_range(w, lo_start, lo_end);
					c->cardinality -= (uint16) removed;
					container_shrink_bitset(c);
				}
				break;
			}
		case LION_CT_NARROW:
			{
				uint64	   *w = narrow_mdata(c);

				removed = narrow_range_cardinality(w, lo_start, lo_end);
				if (removed > 0)
				{
					narrow_clear_range(w, lo_start, lo_end);
					c->cardinality -= (uint16) removed;
					container_shrink_narrow(c);
				}
				break;
			}
		case LION_CT_RUN:
			{
				LionRun	   *runs = run_mdata(c);
				int32		nruns = (int32) run_nruns(c);
				int32		s = (int32) lo_start;
				int32		e = (int32) lo_end;
				LionRun		repl[2];
				int32		nrepl = 0;
				int32		newn;
				int32		i;
				int32		j;
				int32		k;

				/* first run reaching into the range */
				i = run_locate(runs, (uint32) nruns, lo_start);
				if (i < 0 || run_last(&runs[i]) < s)
					i++;
				if (i >= nruns || (int32) runs[i].start > e)
					return 0;
				/* last run starting at or before the end of the range */
				j = run_locate(runs, (uint32) nruns, lo_end);
				if (unlikely(j < i))
				{
					/* runs out of order: nothing below holds */
					removed = container_remove_range_bitset(c, lo_start, lo_end);
					break;
				}

				removed = 0;
				for (k = i; k <= j; k++)
				{
					int32		rs = Max((int32) runs[k].start, s);
					int32		re = Min(run_last(&runs[k]), e);

					if (rs <= re)
						removed += (uint32) (re - rs + 1);
				}

				if ((int32) runs[i].start < s)
				{
					repl[nrepl].start = runs[i].start;
					repl[nrepl].len_minus_1 = (uint16) (s - 1 - (int32) runs[i].start);
					nrepl++;
				}
				if (run_last(&runs[j]) > e)
				{
					repl[nrepl].start = (uint16) (e + 1);
					repl[nrepl].len_minus_1 = (uint16) (run_last(&runs[j]) - (e + 1));
					nrepl++;
				}

				newn = nruns - (j - i + 1) + nrepl;
				if (newn > (int32) LION_RUN_MAX_NRUNS)
				{
					removed = container_remove_range_bitset(c, lo_start, lo_end);
					break;
				}
				memmove(&runs[i + nrepl], &runs[j + 1],
						(size_t) (nruns - j - 1) * sizeof(LionRun));
				for (k = 0; k < nrepl; k++)
					runs[i + k] = repl[k];
				run_set_nruns(c, (uint32) newn);
				c->cardinality -= (uint16) removed;
				break;
			}
		default:
			Assert(false);
			removed = 0;
			break;
	}
	Assert(lion_container_size(c) <= LION_CONTAINER_MAX_SIZE);
	return removed;
}


/* ----------------------------------------------------------------
 *							set algebra
 * ----------------------------------------------------------------
 */

/*
 * An ARRAY operand's member as a result stores it: masked into range, which
 * is how bits_test() and iterate() read it.  The merges below compare the
 * members as they are, and for a well-formed operand that is the same thing;
 * but a result is handed on - into a caller's bitset image, into the next
 * operation, onto a page - and must not carry a damaged operand's member past
 * LION_LO_MAX with it ("untrusted containers").  One AND per member written.
 */
static inline uint16
array_out(uint16 lo)
{
	return (uint16) (lo & LION_LO_MASK);
}

/* Branch-free: every member is written, and kept when its bit is set. */
static uint32
array_and_bitset(const uint16 *arr, uint32 na, const uint64 *w, uint16 *out)
{
	uint32		n = 0;
	uint32		i;

	for (i = 0; i < na; i++)
	{
		out[n] = array_out(arr[i]);
		n += bits_test(w, arr[i]) ? 1 : 0;
	}
	return n;
}

/*
 * HOW TWO SIDES OF AN AND MEET (the count engine's merges, DESIGN.md §15,
 * "Unions and intersections unoptimized").  A merge of two sorted sides is a
 * chain of dependent steps - each comparison decides where the next loads
 * are - and costs 2.5 to 4 ns a step however it is branched, measured on
 * containers of a heap of 34 rows a block.  Setting the larger side's members
 * in a bitset image and testing the smaller side's against it are
 * independent steps, about a nanosecond each, after a fixed 4 KB clear: three
 * to four times faster than the merge for two ARRAYs of a hundred members or
 * more (1,035 x 1,022 members: 4.2 us merged, 1.8 as an image) and for an
 * ARRAY against a dense column's RUN (591 members against 400 runs: 4.3 us,
 * 1.1).  The merge stays for two small sides, where the clear would cost more
 * than the steps; and a few members against many are searched for instead -
 * galloped through a large ARRAY, or each looked up in a RUN's runs by a
 * branch-free binary search (8 members against 400 runs: 1.1 us merged,
 * 0.06 searched).  The thresholds are those measurements'.
 */
#define LION_AND_MERGE_MAX		48	/* both sides together, at most: merge */
#define LION_AND_SEARCH_RATIO	8	/* fewer than 1/8 of the other side (plus
									 * the clear's worth): search, not image */
#define LION_AND_SEARCH_SLACK	64

/* The larger ARRAY's members as an image, and the smaller's tested against it. */
static uint32
array_and_image(const uint16 *small, uint32 nsmall, const uint16 *large,
				uint32 nlarge, uint16 *out)
{
	uint64		w[LION_BITSET_WORDS];
	uint32		i;

	memset(w, 0, sizeof(w));
	for (i = 0; i < nlarge; i++)
		bits_set(w, large[i]);
	return array_and_bitset(small, nsmall, w, out);
}

/*
 * Intersection of two sorted arrays; out gets at most min(na, nb) values, in
 * the order of the smaller side's (both ascending for well-formed operands).
 */
static uint32
array_intersect(const uint16 *a, uint32 na, const uint16 *b, uint32 nb,
				uint16 *out)
{
	const uint16 *small = (na <= nb) ? a : b;
	const uint16 *large = (na <= nb) ? b : a;
	uint32		nsmall = (na <= nb) ? na : nb;
	uint32		nlarge = (na <= nb) ? nb : na;
	uint32		n = 0;
	uint32		i = 0;
	uint32		j = 0;

	/* a few members against many: gallop through the larger array */
	if (nsmall * LION_AND_SEARCH_RATIO <= nlarge + LION_AND_SEARCH_SLACK &&
		nlarge > nsmall * 8)
	{
		uint32		li = 0;

		for (i = 0; i < nsmall; i++)
		{
			li = array_gallop(large, nlarge, li, small[i]);
			if (li >= nlarge)
				break;
			if (large[li] == small[i])
				out[n++] = array_out(small[i]);
		}
		return n;
	}

	if (na + nb > LION_AND_MERGE_MAX)
		return array_and_image(small, nsmall, large, nlarge, out);

	/*
	 * The merge of two small arrays, without a branch on the comparison:
	 * whether a member is written, and which side steps, are sums of
	 * comparisons.  out[n] is written at every step and kept only on a
	 * match; n never reaches min(na, nb) while the loop runs, so the write
	 * stays inside the result's room.  The steps are exactly those of the
	 * three-way branch it replaces, for any input.
	 */
	while (i < na && j < nb)
	{
		uint16		va = a[i];
		uint16		vb = b[j];

		out[n] = array_out(va);
		n += (va == vb);
		i += (va <= vb);
		j += (vb <= va);
	}
	return n;
}

/*
 * Union of two sorted arrays.  The merge writes the smaller of the two heads
 * and steps the side (or sides, on a tie) it came from, without a branch on
 * the comparison - the steps of the three-way branch it replaces, for any
 * input.
 */
static uint32
array_union(const uint16 *a, uint32 na, const uint16 *b, uint32 nb, uint16 *out)
{
	uint32		n = 0;
	uint32		i = 0;
	uint32		j = 0;

	while (i < na && j < nb)
	{
		uint16		va = a[i];
		uint16		vb = b[j];

		out[n++] = array_out(va <= vb ? va : vb);
		i += (va <= vb);
		j += (vb <= va);
	}
	while (i < na)
		out[n++] = array_out(a[i++]);
	while (j < nb)
		out[n++] = array_out(b[j++]);
	return n;
}

/* a \ b for two sorted arrays. */
static uint32
array_difference(const uint16 *a, uint32 na, const uint16 *b, uint32 nb,
				 uint16 *out)
{
	uint32		n = 0;
	uint32		i = 0;
	uint32		j = 0;

	while (i < na && j < nb)
	{
		if (a[i] < b[j])
			out[n++] = array_out(a[i++]);
		else if (a[i] > b[j])
			j++;
		else
		{
			i++;
			j++;
		}
	}
	while (i < na)
		out[n++] = array_out(a[i++]);
	return n;
}

/*
 * Is v in a run of runs[0 .. nruns - 1] (nruns > 0)?  A branch-free binary
 * search for the last run that starts at or below v.  For damaged runs the
 * answer is unspecified, and the search stays inside the runs.
 */
static inline bool
run_has_member(const LionRun *runs, uint32 nruns, uint32 v)
{
	const LionRun *base = runs;

	while (nruns > 1)
	{
		uint32		half = nruns / 2;

		base = ((uint32) base[half].start <= v) ? base + half : base;
		nruns -= half;
	}
	return (uint32) base->start <= v && (int32) v <= run_end(base);
}

/*
 * Members of arr that are (not) in the run container rc.
 *
 * The AND takes one of three ways (see "how two sides of an AND meet" above):
 * a few members are each looked up in the runs; many are tested against an
 * image of the runs; two small sides are merged, without a branch on the
 * comparison - a member at or below the current run's end steps the array,
 * and is written when it is also at or above its start, one past the end
 * steps the runs (for a well-formed run the three-way branch's steps; a
 * damaged run that ends before it starts is passed over where the branch
 * stepped the array).  Each writes at most one member per member of arr.
 */
static uint32
array_and_run(const uint16 *arr, uint32 na, const LionContainer *rc, uint16 *out)
{
	const LionRun *runs = run_cdata(rc);
	uint32		nruns = run_nruns(rc);
	uint32		i = 0;
	uint32		j = 0;
	uint32		n = 0;

	if (nruns == 0 || na == 0)
		return 0;

	if (na + nruns > LION_AND_MERGE_MAX)
	{
		if (na * LION_AND_SEARCH_RATIO <= nruns + LION_AND_SEARCH_SLACK)
		{
			for (i = 0; i < na; i++)
			{
				out[n] = array_out(arr[i]);
				n += run_has_member(runs, nruns, arr[i]) ? 1 : 0;
			}
			return n;
		}
		else
		{
			uint64		w[LION_BITSET_WORDS];

			memset(w, 0, sizeof(w));
			container_or_bitset(rc, w);
			return array_and_bitset(arr, na, w, out);
		}
	}

	while (i < na && j < nruns)
	{
		int32		v = (int32) arr[i];
		uint32		inrun = (v <= run_end(&runs[j]));

		out[n] = array_out(arr[i]);
		n += inrun & (v >= (int32) runs[j].start);
		i += inrun;
		j += inrun ^ 1;
	}
	return n;
}

static uint32
array_andnot_run(const uint16 *arr, uint32 na, const LionContainer *rc,
				 uint16 *out)
{
	const LionRun *runs = run_cdata(rc);
	uint32		nruns = run_nruns(rc);
	uint32		i = 0;
	uint32		j = 0;
	uint32		n = 0;

	while (i < na && j < nruns)
	{
		if ((int32) arr[i] < (int32) runs[j].start)
			out[n++] = array_out(arr[i++]);
		else if ((int32) arr[i] > run_end(&runs[j]))
			j++;
		else
			i++;
	}
	while (i < na)
		out[n++] = array_out(arr[i++]);
	return n;
}

static uint32
array_andnot_bitset(const uint16 *arr, uint32 na, const uint64 *w, uint16 *out)
{
	uint32		n = 0;
	uint32		i;

	for (i = 0; i < na; i++)
		if (!bits_test(w, arr[i]))
			out[n++] = array_out(arr[i]);
	return n;
}

/* The same two against a NARROW's payload: a member it has no bit for is out. */
static uint32
array_and_narrow(const uint16 *arr, uint32 na, const uint64 *w, uint16 *out)
{
	uint32		n = 0;
	uint32		i;

	for (i = 0; i < na; i++)
	{
		out[n] = array_out(arr[i]);
		n += narrow_test(w, arr[i]) ? 1 : 0;
	}
	return n;
}

static uint32
array_andnot_narrow(const uint16 *arr, uint32 na, const uint64 *w, uint16 *out)
{
	uint32		n = 0;
	uint32		i;

	for (i = 0; i < na; i++)
		if (!narrow_test(w, arr[i]))
			out[n++] = array_out(arr[i]);
	return n;
}

/*
 * Intersect two RUN containers straight into o's run payload.  Returns false
 * (leaving o's payload undefined) if the result needs more than
 * LION_RUN_MAX_NRUNS runs; the caller then falls back to the bitset path.
 */
static bool
run_and_run(const LionContainer *a, const LionContainer *b, LionContainer *o)
{
	const LionRun *ra = run_cdata(a);
	const LionRun *rb = run_cdata(b);
	uint32		na = run_nruns(a);
	uint32		nb = run_nruns(b);
	LionRun	   *out = run_mdata(o);
	uint32		i = 0;
	uint32		j = 0;
	uint32		n = 0;
	uint32		card = 0;

	while (i < na && j < nb)
	{
		int32		ea = run_end(&ra[i]);
		int32		eb = run_end(&rb[j]);
		int32		s = Max((int32) ra[i].start, (int32) rb[j].start);

		/* clamped where it is written, as run_last() would have it */
		int32		e = Min(Min(ea, eb), (int32) LION_LO_MAX);

		if (s <= e)
		{
			if (n >= LION_RUN_MAX_NRUNS)
				return false;
			out[n].start = (uint16) s;
			out[n].len_minus_1 = (uint16) (e - s);
			n++;
			card += (uint32) (e - s + 1);
		}
		if (ea < eb)
			i++;
		else
			j++;
	}
	o->type = LION_CT_RUN;
	run_set_nruns(o, n);
	o->cardinality = (uint16) card;
	return true;
}

/* Number of members two RUN containers have in common. */
static uint32
run_and_run_cardinality(const LionContainer *a, const LionContainer *b)
{
	const LionRun *ra = run_cdata(a);
	const LionRun *rb = run_cdata(b);
	uint32		na = run_nruns(a);
	uint32		nb = run_nruns(b);
	uint32		i = 0;
	uint32		j = 0;
	uint32		card = 0;

	while (i < na && j < nb)
	{
		int32		ea = run_end(&ra[i]);
		int32		eb = run_end(&rb[j]);
		int32		s = Max((int32) ra[i].start, (int32) rb[j].start);
		int32		e = Min(ea, eb);

		if (s <= e)
			card += (uint32) (e - s + 1);
		if (ea < eb)
			i++;
		else
			j++;
	}
	return card;
}

/*
 * o = c, for the set algebra's shortcuts, in what c's readers here look at:
 * a well-formed c is copied byte for byte, and a header claiming more than
 * LION_ARRAY_MAX_CARD members or LION_RUN_MAX_NRUNS runs is copied as its
 * clamped self, so that the copy fits the LION_CONTAINER_MAX_SIZE work buffer
 * whatever the header says.  The copy is a result like any other, so its
 * members are in range too (array_out()): an ARRAY's are masked, a run is
 * clamped at LION_LO_MAX (run_last()) and one that starts past it, which is
 * empty, is left out.
 */
static void
container_copy(const LionContainer *c, LionContainer *o)
{
	Size		size;
	uint32		i;

	switch (c->type)
	{
		case LION_CT_ARRAY:
			size = lion_container_size_for(LION_CT_ARRAY, array_card(c), 0);
			break;
		case LION_CT_RUN:
			size = lion_container_size_for(LION_CT_RUN, 0, run_nruns(c));
			break;
		default:
			size = lion_container_size_for((LionContainerType) c->type, 0, 0);
			break;
	}
	memcpy(o, c, size);
	container_clamp(o);

	if (o->type == LION_CT_ARRAY)
	{
		uint16	   *arr = array_mdata(o);
		uint32		n = o->cardinality;

		for (i = 0; i < n; i++)
			arr[i] = array_out(arr[i]);
	}
	else if (o->type == LION_CT_RUN)
	{
		LionRun    *runs = run_mdata(o);
		uint32		nruns = run_nruns(o);
		uint32		keep = 0;

		for (i = 0; i < nruns; i++)
		{
			int32		last = run_last(&runs[i]);

			if ((int32) runs[i].start > last)
				continue;
			runs[keep].start = runs[i].start;
			runs[keep].len_minus_1 = (uint16) (last - (int32) runs[i].start);
			keep++;
		}
		run_set_nruns(o, keep);
	}
}

/* Finish a freshly computed result: optimize and copy into dest. */
static uint32
container_emit_result(LionContainer *o, LionContainer *dest)
{
	lion_container_optimize(o);
	Assert(lion_container_size(o) <= LION_CONTAINER_MAX_SIZE);
	memcpy(dest, o, lion_container_size(o));
	return o->cardinality;
}

/*
 * The AND of an ARRAY of a few members with anything, member by member.
 *
 * The count engine's commonest AND is a key's container of one or two rows
 * against a filter's container of dozens or thousands (an FK-side join's key
 * against the collected copy of its fact filters, DESIGN.md §27, "The copy,
 * looked up by key"): the general path merged or galloped through the other
 * operand and then built its result in a work buffer, optimized it and copied
 * it out, for a result that is almost always empty.  Here each member is
 * looked up in the other operand - a bit, a binary search of its members or
 * of its runs - and the result is written straight into dest.
 *
 * LION_AND_PROBE_MAX members at most, because a result of that many or fewer
 * is an ARRAY in lion_container_optimize()'s choice (three members in one
 * run take fourteen bytes either way, and a tie goes to the ARRAY): the
 * result is the one the general path makes, byte for byte.  A member is
 * compared as the merges compare it, unmasked (bits_test() masks it itself),
 * and written masked (array_out()); a damaged operand's member that is not
 * above the last one written is passed over, so that what comes out is a
 * well-formed ARRAY whatever went in.
 */
#define LION_AND_PROBE_MAX	3

/* Is v, an ARRAY operand's member, in c - as the merges would find it? */
static inline bool
container_has_member(const LionContainer *c, uint16 v)
{
	switch (c->type)
	{
		case LION_CT_ARRAY:
			{
				const uint16 *base = array_cdata(c);
				uint32		n = array_card(c);

				if (n == 0)
					return false;

				/*
				 * The last member at or below v, if there is one, is in
				 * base[0 .. n - 1] - with no branch to mispredict.
				 */
				while (n > 1)
				{
					uint32		half = n / 2;

					base = (base[half] <= v) ? base + half : base;
					n -= half;
				}
				return *base == v;
			}
		case LION_CT_BITSET:
			return bits_test(bitset_cdata(c), v);
		case LION_CT_NARROW:
			return narrow_test(narrow_cdata(c), v);
		default:
			{
				const LionRun *runs = run_cdata(c);
				int32		idx = run_locate(runs, run_nruns(c), v);

				return idx >= 0 && (int32) v <= run_end(&runs[idx]);
			}
	}
}

static uint32
container_and_probe(const LionContainer *small, const LionContainer *other,
					uint32 ckey, LionContainer *dest)
{
	const uint16 *arr = array_cdata(small);
	uint32		na = array_card(small);
	uint16		out[LION_AND_PROBE_MAX];
	uint32		n = 0;
	uint32		i;

	Assert(na <= LION_AND_PROBE_MAX);
	for (i = 0; i < na; i++)
	{
		uint16		v = arr[i];

		if (n > 0 && array_out(v) <= out[n - 1])
			continue;			/* damaged: out of order, or repeated */
		if (container_has_member(other, v))
			out[n++] = array_out(v);
	}

	/* dest may be either operand: it is written only now */
	dest->ckey = ckey;
	dest->cardinality = (uint16) n;
	dest->type = LION_CT_ARRAY;
	dest->flags = 0;
	if (n > 0)
		memcpy(array_mdata(dest), out, (size_t) n * sizeof(uint16));
	return n;
}

uint32
lion_container_and(const LionContainer *a, const LionContainer *b,
				  LionContainer *dest)
{
	LionContainerBuf buf;
	LionContainer *o = &buf.hdr;

	Assert(a->ckey == b->ckey);

	if (a->cardinality != 0 && b->cardinality != 0)
	{
		if (a->type == LION_CT_ARRAY && array_card(a) <= LION_AND_PROBE_MAX)
			return container_and_probe(a, b, a->ckey, dest);
		if (b->type == LION_CT_ARRAY && array_card(b) <= LION_AND_PROBE_MAX)
			return container_and_probe(b, a, a->ckey, dest);
	}

	lion_container_init(o, a->ckey);

	if (a->cardinality == 0 || b->cardinality == 0)
	{
		/* empty result */
	}
	else if (a->type == LION_CT_ARRAY || b->type == LION_CT_ARRAY)
	{
		const LionContainer *arr = (a->type == LION_CT_ARRAY) ? a : b;
		const LionContainer *oth = (a->type == LION_CT_ARRAY) ? b : a;
		uint16	   *out = array_mdata(o);
		uint32		n;

		if (oth->type == LION_CT_ARRAY)
			n = array_intersect(array_cdata(a), array_card(a),
								array_cdata(b), array_card(b), out);
		else if (oth->type == LION_CT_BITSET)
			n = array_and_bitset(array_cdata(arr), array_card(arr),
								 bitset_cdata(oth), out);
		else if (oth->type == LION_CT_NARROW)
			n = array_and_narrow(array_cdata(arr), array_card(arr),
								 narrow_cdata(oth), out);
		else
			n = array_and_run(array_cdata(arr), array_card(arr), oth, out);
		o->cardinality = (uint16) n;
	}
	else if (a->type == LION_CT_RUN && b->type == LION_CT_RUN &&
			 run_and_run(a, b, o))
	{
		/* done: o already holds the intersected runs */
	}
	else if (a->type == LION_CT_NARROW || b->type == LION_CT_NARROW)
	{
		const LionContainer *nar = (a->type == LION_CT_NARROW) ? a : b;
		const LionContainer *oth = (a->type == LION_CT_NARROW) ? b : a;

		o->type = LION_CT_NARROW;
		o->cardinality = (uint16) container_op_narrow(nar, oth, LION_BITS_AND,
													  narrow_mdata(o));
	}
	else
	{
		o->type = LION_CT_BITSET;
		o->cardinality = (uint16) container_op_bitset(a, b, LION_BITS_AND,
													  bitset_mdata(o));
	}
	return container_emit_result(o, dest);
}

uint32
lion_container_or(const LionContainer *a, const LionContainer *b,
				 LionContainer *dest)
{
	LionContainerBuf buf;
	LionContainer *o = &buf.hdr;

	Assert(a->ckey == b->ckey);
	lion_container_init(o, a->ckey);

	if (a->type == LION_CT_ARRAY && b->type == LION_CT_ARRAY &&
		array_card(a) + array_card(b) <= LION_ARRAY_MAX_CARD)
	{
		uint32		n = array_union(array_cdata(a), array_card(a),
									array_cdata(b), array_card(b),
									array_mdata(o));

		o->cardinality = (uint16) n;
	}
	else if (a->cardinality == 0 || b->cardinality == 0)
	{
		const LionContainer *src = (a->cardinality == 0) ? b : a;

		container_copy(src, o);
		o->ckey = a->ckey;
		o->flags = 0;
	}
	else if (a->type == LION_CT_NARROW && b->type == LION_CT_NARROW)
	{
		o->type = LION_CT_NARROW;
		o->cardinality = (uint16) container_op_narrow(a, b, LION_BITS_OR,
													  narrow_mdata(o));
	}
	else
	{
		o->type = LION_CT_BITSET;
		o->cardinality = (uint16) container_op_bitset(a, b, LION_BITS_OR,
													  bitset_mdata(o));
	}
	return container_emit_result(o, dest);
}

uint32
lion_container_andnot(const LionContainer *a, const LionContainer *b,
					 LionContainer *dest)
{
	LionContainerBuf buf;
	LionContainer *o = &buf.hdr;

	Assert(a->ckey == b->ckey);
	lion_container_init(o, a->ckey);

	if (a->cardinality == 0)
	{
		/* empty result */
	}
	else if (b->cardinality == 0)
	{
		container_copy(a, o);
		o->flags = 0;
	}
	else if (a->type == LION_CT_ARRAY)
	{
		uint16	   *out = array_mdata(o);
		uint32		n;

		if (b->type == LION_CT_ARRAY)
			n = array_difference(array_cdata(a), array_card(a),
								 array_cdata(b), array_card(b), out);
		else if (b->type == LION_CT_BITSET)
			n = array_andnot_bitset(array_cdata(a), array_card(a),
									bitset_cdata(b), out);
		else if (b->type == LION_CT_NARROW)
			n = array_andnot_narrow(array_cdata(a), array_card(a),
									narrow_cdata(b), out);
		else
			n = array_andnot_run(array_cdata(a), array_card(a), b, out);
		o->cardinality = (uint16) n;
	}
	else if (a->type == LION_CT_NARROW)
	{
		o->type = LION_CT_NARROW;
		o->cardinality = (uint16) container_op_narrow(a, b, LION_BITS_ANDNOT,
													  narrow_mdata(o));
	}
	else
	{
		o->type = LION_CT_BITSET;
		o->cardinality = (uint16) container_op_bitset(a, b, LION_BITS_ANDNOT,
													  bitset_mdata(o));
	}
	return container_emit_result(o, dest);
}

/*
 * THE SET ALGEBRA UNOPTIMIZED (lion_container.h).  Each is its optimizing
 * twin above without container_emit_result(): the result is written straight
 * into dest in the representation the operation builds it in, and is not
 * searched for a smaller one.  The count engine's merge ANDs a container with
 * the next source's and hands the result on at once - to the next AND, to
 * the visibility map, to a count - where optimizing each intermediate result
 * cost a count of its runs and, as often as not, a conversion the next
 * operation undid: an ARRAY of a dense column's members turned into a RUN,
 * only to be filled into a bitset image by the OR after it.  On the synthetic
 * repro's filter of six clauses, over a heap of 34 rows a page, that was most
 * of the count's time (DESIGN.md §15, "Unions and intersections
 * unoptimized").
 */
uint32
lion_container_and_raw(const LionContainer *a, const LionContainer *b,
					   LionContainer *dest)
{
	uint32		n;

	Assert(a->ckey == b->ckey);
	Assert(dest != a && dest != b);

	if (a->cardinality != 0 && b->cardinality != 0)
	{
		if (a->type == LION_CT_ARRAY && array_card(a) <= LION_AND_PROBE_MAX)
			return container_and_probe(a, b, a->ckey, dest);
		if (b->type == LION_CT_ARRAY && array_card(b) <= LION_AND_PROBE_MAX)
			return container_and_probe(b, a, a->ckey, dest);
	}

	lion_container_init(dest, a->ckey);

	if (a->cardinality == 0 || b->cardinality == 0)
		return 0;

	if (a->type == LION_CT_ARRAY || b->type == LION_CT_ARRAY)
	{
		const LionContainer *arr = (a->type == LION_CT_ARRAY) ? a : b;
		const LionContainer *oth = (a->type == LION_CT_ARRAY) ? b : a;
		uint16	   *out = array_mdata(dest);

		if (oth->type == LION_CT_ARRAY)
			n = array_intersect(array_cdata(a), array_card(a),
								array_cdata(b), array_card(b), out);
		else if (oth->type == LION_CT_BITSET)
			n = array_and_bitset(array_cdata(arr), array_card(arr),
								 bitset_cdata(oth), out);
		else if (oth->type == LION_CT_NARROW)
			n = array_and_narrow(array_cdata(arr), array_card(arr),
								 narrow_cdata(oth), out);
		else
			n = array_and_run(array_cdata(arr), array_card(arr), oth, out);
		dest->cardinality = (uint16) n;
		return n;
	}

	if (a->type == LION_CT_RUN && b->type == LION_CT_RUN &&
		run_and_run(a, b, dest))
		return dest->cardinality;

	/* A NARROW and anything: one kernel pass over a NARROW's words. */
	if (a->type == LION_CT_NARROW || b->type == LION_CT_NARROW)
	{
		if (a->type == LION_CT_NARROW)
			n = container_op_narrow(a, b, LION_BITS_AND, narrow_mdata(dest));
		else
			n = container_op_narrow(b, a, LION_BITS_AND, narrow_mdata(dest));
		dest->type = LION_CT_NARROW;
		dest->cardinality = (uint16) n;
		return n;
	}

	/*
	 * A BITSET operand is copied and the other ANDed in; two BITSETs are
	 * ANDed and counted in one pass (container_op_bitset()).
	 */
	if (b->type == LION_CT_BITSET)
		n = container_op_bitset(b, a, LION_BITS_AND, bitset_mdata(dest));
	else
		n = container_op_bitset(a, b, LION_BITS_AND, bitset_mdata(dest));
	dest->type = LION_CT_BITSET;
	dest->cardinality = (uint16) n;
	return n;
}

uint32
lion_container_andnot_raw(const LionContainer *a, const LionContainer *b,
						  LionContainer *dest)
{
	uint32		n;

	Assert(a->ckey == b->ckey);
	Assert(dest != a && dest != b);

	lion_container_init(dest, a->ckey);

	if (a->cardinality == 0)
		return 0;
	if (b->cardinality == 0)
	{
		container_copy(a, dest);
		dest->ckey = a->ckey;
		dest->flags = 0;
		return dest->cardinality;
	}
	if (a->type == LION_CT_ARRAY)
	{
		uint16	   *out = array_mdata(dest);

		if (b->type == LION_CT_ARRAY)
			n = array_difference(array_cdata(a), array_card(a),
								 array_cdata(b), array_card(b), out);
		else if (b->type == LION_CT_BITSET)
			n = array_andnot_bitset(array_cdata(a), array_card(a),
									bitset_cdata(b), out);
		else if (b->type == LION_CT_NARROW)
			n = array_andnot_narrow(array_cdata(a), array_card(a),
									narrow_cdata(b), out);
		else
			n = array_andnot_run(array_cdata(a), array_card(a), b, out);
		dest->cardinality = (uint16) n;
		return n;
	}
	if (a->type == LION_CT_NARROW)
	{
		n = container_op_narrow(a, b, LION_BITS_ANDNOT, narrow_mdata(dest));
		dest->type = LION_CT_NARROW;
		dest->cardinality = (uint16) n;
		return n;
	}

	n = container_op_bitset(a, b, LION_BITS_ANDNOT, bitset_mdata(dest));
	dest->type = LION_CT_BITSET;
	dest->cardinality = (uint16) n;
	return n;
}

uint32
lion_container_or_raw(const LionContainer *a, const LionContainer *b,
					  LionContainer *dest)
{
	uint32		n;

	Assert(a->ckey == b->ckey);
	Assert(dest != a && dest != b);

	lion_container_init(dest, a->ckey);

	if (a->type == LION_CT_ARRAY && b->type == LION_CT_ARRAY &&
		array_card(a) + array_card(b) <= LION_ARRAY_MAX_CARD)
	{
		n = array_union(array_cdata(a), array_card(a),
						array_cdata(b), array_card(b), array_mdata(dest));
		dest->cardinality = (uint16) n;
		return n;
	}
	if (a->cardinality == 0 || b->cardinality == 0)
	{
		container_copy((a->cardinality == 0) ? b : a, dest);
		dest->ckey = a->ckey;
		dest->flags = 0;
		return dest->cardinality;
	}
	if (a->type == LION_CT_NARROW && b->type == LION_CT_NARROW)
	{
		n = container_op_narrow(a, b, LION_BITS_OR, narrow_mdata(dest));
		dest->type = LION_CT_NARROW;
		dest->cardinality = (uint16) n;
		return n;
	}

	n = container_op_bitset(a, b, LION_BITS_OR, bitset_mdata(dest));
	dest->type = LION_CT_BITSET;
	dest->cardinality = (uint16) n;
	return n;
}

/*
 * THE AND OF A FEW MEMBERS WITH A UNION, WITHOUT THE UNION (DESIGN.md
 * §29.11, "Unions probed").  The count engine's leapfrog ANDs its running
 * intersection with every source in turn, and a source that is an IN list or
 * a multi-key `&&` is the union of the containers its sets have at the key.
 * Building that union costs every member of every one of those containers -
 * a bitset image cleared, filled and counted, or a fold of merges - whatever
 * the intersection holds, and a running intersection of a few rows is ANDed
 * with it only to keep those few.  Here each of a's members is looked up in
 * the union's members one after the other instead, and a member once found
 * is not looked up again: at most |a| lookups a member, and none past the
 * member that finds the last of them.
 *
 * pending[] holds the positions in vals[] of the values not found yet, in
 * ascending order, and found[] (a bit a position) the ones that were; each
 * pass leaves in pending[] the values b does not hold and returns how many
 * those are, a position written back and kept by adding a comparison rather
 * than by a branch.  The lookup: a bit test in a BITSET; a galloping search
 * through an ARRAY, from where the last value was found; a binary search of
 * a RUN's runs for a few values against many runs, else a merge with them.
 * Measured by a harness beside this file (-O2, containers of a heap of 70
 * rows a block, a key's lookups timed 20,000 times): the gallop is never
 * slower than a merge of the two sorted sides - 16 values against an ARRAY
 * of 450 members 0.19 us against 1.6, 256 against 97 members 0.84 against
 * 1.6, the merge's steps being a chain of dependent loads - and within a
 * fifth of setting the ARRAY's members in an image and testing the values
 * against it wherever that is faster, so it is the one way for an ARRAY.
 * Against 723 runs the search takes 13 ns a value and the merge 0.6 ns a run
 * or so, level at about one value for every eight runs, which is
 * lion_container_and()'s own ratio (LION_AND_SEARCH_RATIO).  For a damaged b
 * - members out of order, runs overlapping - which positions it keeps is
 * unspecified, and every index stays inside vals[] and b.
 */
static uint32
probe_pending(const LionContainer *b, const uint16 *vals, uint16 *pending,
			  uint32 np, uint64 *found)
{
	uint32		k = 0;
	uint32		i = 0;
	uint32		j = 0;

#define PROBE_KEEP(p_, h_) \
	do { \
		found[(p_) >> 6] |= (uint64) (h_) << ((p_) & 63); \
		pending[k] = (uint16) (p_); \
		k += 1 - (h_); \
	} while (0)

	switch (b->type)
	{
		case LION_CT_BITSET:
			{
				const uint64 *w = bitset_cdata(b);

				for (i = 0; i < np; i++)
				{
					uint32		p = pending[i];
					uint32		h = bits_test(w, vals[p]) ? 1 : 0;

					PROBE_KEEP(p, h);
				}
				return k;
			}
		case LION_CT_NARROW:
			{
				const uint64 *w = narrow_cdata(b);

				for (i = 0; i < np; i++)
				{
					uint32		p = pending[i];
					uint32		h = narrow_test(w, vals[p]) ? 1 : 0;

					PROBE_KEEP(p, h);
				}
				return k;
			}
		case LION_CT_ARRAY:
			{
				const uint16 *arr = array_cdata(b);
				uint32		s = array_card(b);

				for (i = 0; i < np; i++)
				{
					uint32		p = pending[i];
					uint32		h;

					j = array_gallop(arr, s, j, vals[p]);
					h = (j < s && arr[j] == vals[p]) ? 1 : 0;
					PROBE_KEEP(p, h);
				}
				return k;
			}
		case LION_CT_RUN:
			{
				const LionRun *runs = run_cdata(b);
				uint32		nruns = run_nruns(b);

				if (nruns == 0)
					return np;
				if (nruns >= LION_AND_SEARCH_RATIO * np)
				{
					for (i = 0; i < np; i++)
					{
						uint32		p = pending[i];
						uint32		h = run_has_member(runs, nruns, vals[p]) ? 1 : 0;

						PROBE_KEEP(p, h);
					}
					return k;
				}
				while (i < np && j < nruns)
				{
					uint32		p = pending[i];
					int32		v = (int32) vals[p];

					if (v > run_end(&runs[j]))
						j++;
					else
					{
						uint32		h = (v >= (int32) runs[j].start) ? 1 : 0;

						PROBE_KEEP(p, h);
						i++;
					}
				}
				break;
			}
		default:
			Assert(false);
			return np;
	}
#undef PROBE_KEEP

	/* the merge ran out of b: what is left of pending[] stays pending */
	for (; i < np; i++)
		pending[k++] = pending[i];
	return k;
}

uint32
lion_container_and_union_raw(const LionContainer *a,
							 const LionContainer *const *b, uint32 nb,
							 LionContainer *dest)
{
	uint16		extracted[LION_ARRAY_MAX_CARD];
	uint16		pending[LION_ARRAY_MAX_CARD];
	uint64		found[LION_ARRAY_MAX_CARD / 64];
	const uint16 *vals;
	uint32		n;
	uint32		np;
	uint32		i;

	Assert(dest != a);
#ifdef USE_ASSERT_CHECKING
	for (i = 0; i < nb; i++)
		Assert(b[i]->ckey == a->ckey && dest != b[i]);
#endif

	/*
	 * a's members, as iterate() hands them out: an ARRAY's own, read in
	 * place, or extracted.  A damaged a whose payload holds more than an
	 * ARRAY may (a caller checks the header's cardinality) is taken as
	 * empty.
	 */
	if (a->type == LION_CT_ARRAY)
	{
		vals = array_cdata(a);
		n = array_card(a);
	}
	else
	{
		n = container_extract(a, extracted, LION_ARRAY_MAX_CARD);
		if (n > LION_ARRAY_MAX_CARD)
			n = 0;
		vals = extracted;
	}

	memset(found, 0, sizeof(uint64) * ((n + 63) / 64));
	for (i = 0; i < n; i++)
		pending[i] = (uint16) i;
	np = n;
	for (i = 0; i < nb && np > 0; i++)
		np = probe_pending(b[i], vals, pending, np, found);

	/* the found ones, in a's order */
	return lion_container_array_from_marks(dest, a->ckey, vals, n, found);
}

/*
 * THE PIECES OF THAT PROBE, FOR A TREE (DESIGN.md §29.11, "Trees probed").
 * lion_container_and_union_raw() is one OR of containers probed with a's
 * members; the count engine's AND also meets nested trees - an AND of ORs,
 * an OR of ANDs - which it evaluates for the running intersection's members
 * alone, node by node: a leaf's container probed with the positions still
 * wanted, an AND node's children each given the positions the one before it
 * kept, an OR node's the positions none before it found.  These are the
 * three steps it is made of, as that function takes them.
 *
 * lion_container_extract_members(): c's members as iterate() hands them out,
 * in ascending order, into out, which has room for cap of them; the count,
 * or cap + 1 when there are more than that, which only a damaged container
 * (or one of more members than the caller wanted) can have.
 */
uint32
lion_container_extract_members(const LionContainer *c, uint16 *out, uint32 cap)
{
	return container_extract(c, out, cap);
}

/*
 * lion_container_probe_members(): of the positions pending[0 .. np - 1] into
 * vals[], ascending, mark in found[] (a bit a position) the ones whose value
 * b holds, leave in pending[], in order, the ones it does not, and return
 * how many those are - probe_pending() above: a bit test in a BITSET, a
 * gallop through an ARRAY, a search or a merge of a RUN's runs.  vals[] is
 * ascending for the lookups to be right; for a damaged b or vals[] which
 * positions are kept is unspecified, and no index leaves vals[], pending[],
 * found[] or b.
 */
uint32
lion_container_probe_members(const LionContainer *b, const uint16 *vals,
							 uint16 *pending, uint32 np, uint64 *found)
{
	return probe_pending(b, vals, pending, np, found);
}

/*
 * lion_container_array_from_marks(): dest (capacity LION_CONTAINER_MAX_SIZE)
 * becomes the ARRAY of ckey holding vals[p] for every position p < n marked
 * in marks[], in vals[]'s order, and its cardinality is returned.  A member
 * written is masked and above the one before (array_out()), so that a
 * damaged vals[]'s repeats and disorder come out a well-formed ARRAY, as
 * container_and_probe() leaves them; past LION_ARRAY_MAX_CARD positions
 * nothing is read.  dest does not overlap vals[].
 */
uint32
lion_container_array_from_marks(LionContainer *dest, uint32 ckey,
								const uint16 *vals, uint32 n,
								const uint64 *marks)
{
	uint16	   *out = array_mdata(dest);
	uint32		card = 0;
	uint32		i;

	n = Min(n, (uint32) LION_ARRAY_MAX_CARD);
	lion_container_init(dest, ckey);
	for (i = 0; i < (n + 63) / 64; i++)
	{
		uint64		word = marks[i];

		if ((i << 6) + 64 > n)
			word &= (n & 63) ? ~(~UINT64CONST(0) << (n & 63)) : ~UINT64CONST(0);
		while (word != 0)
		{
			uint32		p = (i << 6) + (uint32) pg_rightmost_one_pos64(word);
			uint16		v = array_out(vals[p]);

			word &= word - 1;
			if (card > 0 && v <= out[card - 1])
				continue;
			out[card++] = v;
		}
	}
	dest->cardinality = (uint16) card;
	return card;
}

/*
 * How many members of an ARRAY c are set in the image w, and in *blocks which
 * heap blocks they lie on - the cardinality and lion_container_block_mask()
 * of the intersection, without writing it.  The count engine's grouped walk
 * (DESIGN.md §10, "The groups of a walk, counted together") tests every
 * group's container at a key against one image of the WHERE's there, and
 * needs no more than this of an intersection whose blocks the visibility map
 * vouches for.  Branch-free; a member is masked into range as iterate() and
 * block_mask() take it, so *blocks has no bit at or above
 * LION_BLOCKS_PER_CONTAINER.
 */
uint32
lion_container_and_image_count(const LionContainer *c, const uint64 *w,
							   uint64 *blocks)
{
	const uint16 *arr = array_cdata(c);
	uint32		n = array_card(c);
	uint32		card = 0;
	uint64		mask = 0;
	uint32		i;

	Assert(c->type == LION_CT_ARRAY);
	for (i = 0; i < n; i++)
	{
		uint32		lo = arr[i] & LION_LO_MASK;
		uint64		hit = (w[lo >> 6] >> (lo & 63)) & 1;

		card += (uint32) hit;
		mask |= hit << (lo >> LION_OFFSET_BITS);
	}
	*blocks = mask;
	return card;
}

void
lion_container_bitset_init(LionContainer *dest, uint32 ckey)
{
	dest->ckey = ckey;
	dest->cardinality = 0;
	dest->type = LION_CT_BITSET;
	dest->flags = 0;
	memset(bitset_mdata(dest), 0, LION_BITSET_BYTES);
}

uint32
lion_container_bitset_recount(LionContainer *c)
{
	uint32		n;

	Assert(c->type == LION_CT_BITSET);
	n = bits_cardinality(bitset_cdata(c));
	c->cardinality = (uint16) n;
	return n;
}

uint32
lion_container_image_cardinality(const uint64 *w)
{
	return bits_cardinality(w);
}

uint32
lion_container_and_cardinality(const LionContainer *a, const LionContainer *b)
{
	Assert(a->ckey == b->ckey);

	if (a->cardinality == 0 || b->cardinality == 0)
		return 0;

	/*
	 * BITSET x BITSET: the AND counted as it is made, nothing stored (a
	 * kernel pass, LION_BITS_AND_COUNT).  Popcounting the ANDed words one
	 * pg_popcount64() at a time made 512 indirect calls on x86-64 before
	 * PostgreSQL 19 (2.3 - 3.8 us in a review); an AND into a 4 KB image on
	 * the stack and one pg_popcount() of it took 837 ns, and 775 on the
	 * machine the kernels were measured on (404 with PostgreSQL 18's
	 * pg_popcount()), where they take 144 ns with AVX2 and 202 with POPCNT.
	 */
	if (a->type == LION_CT_BITSET && b->type == LION_CT_BITSET)
		return bits_kernel(NULL, bitset_cdata(a), bitset_cdata(b),
						   LION_BITS_AND_COUNT);

	if (a->type == LION_CT_ARRAY || b->type == LION_CT_ARRAY)
	{
		const LionContainer *arr = (a->type == LION_CT_ARRAY) ? a : b;
		const LionContainer *oth = (a->type == LION_CT_ARRAY) ? b : a;
		const uint16 *data = array_cdata(arr);
		uint32		n = array_card(arr);
		uint32		card = 0;
		uint32		i;

		if (oth->type == LION_CT_ARRAY)
		{
			const uint16 *ba = array_cdata(a);
			const uint16 *bb = array_cdata(b);
			uint32		na = array_card(a);
			uint32		nb = array_card(b);
			uint32		x = 0;
			uint32		y = 0;

			/* branch-free, as array_intersect()'s merge */
			while (x < na && y < nb)
			{
				uint16		va = ba[x];
				uint16		vb = bb[y];

				card += (va == vb);
				x += (va <= vb);
				y += (vb <= va);
			}
			return card;
		}
		if (oth->type == LION_CT_BITSET)
		{
			const uint64 *w = bitset_cdata(oth);

			for (i = 0; i < n; i++)
				card += bits_test(w, data[i]) ? 1 : 0;
			return card;
		}
		if (oth->type == LION_CT_NARROW)
		{
			const uint64 *w = narrow_cdata(oth);

			for (i = 0; i < n; i++)
				card += narrow_test(w, data[i]) ? 1 : 0;
			return card;
		}
		/* ARRAY x RUN, branch-free as array_and_run() */
		{
			const LionRun *runs = run_cdata(oth);
			uint32		nruns = run_nruns(oth);
			uint32		j = 0;

			i = 0;
			while (i < n && j < nruns)
			{
				int32		v = (int32) data[i];
				uint32		inrun = (v <= run_end(&runs[j]));

				card += inrun & (v >= (int32) runs[j].start);
				i += inrun;
				j += inrun ^ 1;
			}
			return card;
		}
	}

	if (a->type == LION_CT_RUN && b->type == LION_CT_RUN)
		return run_and_run_cardinality(a, b);

	/*
	 * A NARROW and a NARROW, a BITSET or a RUN: a kernel pass over two
	 * NARROWs' words; a BITSET's words that a NARROW has, a word at a time;
	 * a NARROW's members in each run's span.
	 */
	if (a->type == LION_CT_NARROW || b->type == LION_CT_NARROW)
	{
		const LionContainer *nar = (a->type == LION_CT_NARROW) ? a : b;
		const LionContainer *oth = (a->type == LION_CT_NARROW) ? b : a;
		const uint64 *nw = narrow_cdata(nar);
		uint32		card = 0;
		uint32		i;

		if (oth->type == LION_CT_NARROW)
			return narrow_kernel(NULL, nw, narrow_cdata(oth),
								 LION_BITS_AND_COUNT);
		if (oth->type == LION_CT_BITSET)
		{
			const uint64 *w = bitset_cdata(oth);

			for (i = 0; i < LION_NARROW_WORDS; i++)
				card += lion_popcount64(nw[i] & w[narrow_bits_word(i)]);
			return card;
		}
		{
			const LionRun *runs = run_cdata(oth);
			uint32		nruns = run_nruns(oth);

			Assert(oth->type == LION_CT_RUN);
			for (i = 0; i < nruns; i++)
			{
				int32		last = run_last(&runs[i]);

				if ((int32) runs[i].start <= last)
					card += narrow_range_cardinality(nw, runs[i].start,
													 (uint32) last);
			}
			return card;
		}
	}

	/* BITSET x RUN */
	{
		const LionContainer *bs = (a->type == LION_CT_BITSET) ? a : b;
		const LionContainer *rc = (a->type == LION_CT_BITSET) ? b : a;
		const uint64 *w = bitset_cdata(bs);
		const LionRun *runs = run_cdata(rc);
		uint32		nruns = run_nruns(rc);
		uint32		card = 0;
		uint32		i;

		for (i = 0; i < nruns; i++)
		{
			int32		last = run_last(&runs[i]);

			if ((int32) runs[i].start <= last)
				card += bits_range_cardinality(w, runs[i].start, (uint32) last);
		}
		return card;
	}
}


/* ----------------------------------------------------------------
 *						structural validation
 * ----------------------------------------------------------------
 */

#define LION_CHECK_FAIL(msg) \
	do { *errmsg = (msg); return false; } while (0)

bool
lion_container_check(const LionContainer *c, Size avail_bytes, const char **errmsg)
{
	uint32		card;

	*errmsg = NULL;

	if (avail_bytes < LION_CONTAINER_HDRSZ)
		LION_CHECK_FAIL("container header does not fit in the available space");

	/*
	 * A sparse segment (DESIGN.md §13) shares this header but is not a
	 * container: it is checked by lion_sparse_check(), and reaching this
	 * function with one means the caller failed to dispatch on the type.
	 */
	if (c->type == LION_CT_SPARSE)
		LION_CHECK_FAIL("item is a sparse segment, not a container");

	if (!lion_container_type_valid(c->type))
		LION_CHECK_FAIL("invalid container type");

	if (c->flags != 0)
		LION_CHECK_FAIL("container flags are not zero");

	card = c->cardinality;
	if (card > LION_CONTAINER_RANGE)
		LION_CHECK_FAIL("container cardinality is out of range");

	switch (c->type)
	{
		case LION_CT_ARRAY:
			{
				const uint16 *arr;
				uint32		i;

				if (card > LION_ARRAY_MAX_CARD)
					LION_CHECK_FAIL("array container has more than LION_ARRAY_MAX_CARD members");
				if (lion_container_size_for(LION_CT_ARRAY, card, 0) > avail_bytes)
					LION_CHECK_FAIL("array container does not fit in the available space");
				arr = array_cdata(c);
				for (i = 0; i < card; i++)
				{
					if ((uint32) arr[i] > LION_LO_MAX)
						LION_CHECK_FAIL("array container member is out of range");
					if (i > 0 && arr[i] <= arr[i - 1])
						LION_CHECK_FAIL("array container members are not strictly ascending");
				}
				break;
			}
		case LION_CT_BITSET:
			{
				if (lion_container_size_for(LION_CT_BITSET, card, 0) > avail_bytes)
					LION_CHECK_FAIL("bitset container does not fit in the available space");
				if (bits_cardinality(bitset_cdata(c)) != card)
					LION_CHECK_FAIL("bitset container cardinality does not match its payload");
				break;
			}
		case LION_CT_NARROW:
			{
				/* every bit is a legal member: the count is all there is */
				if (lion_container_size_for(LION_CT_NARROW, card, 0) > avail_bytes)
					LION_CHECK_FAIL("narrow container does not fit in the available space");
				if (narrow_cardinality(narrow_cdata(c)) != card)
					LION_CHECK_FAIL("narrow container cardinality does not match its payload");
				break;
			}
		case LION_CT_RUN:
			{
				const LionRun *runs;
				uint32		nruns;
				uint32		total = 0;
				int32		prev_last = -2;
				uint32		i;

				if (avail_bytes < LION_CONTAINER_HDRSZ + sizeof(uint16))
					LION_CHECK_FAIL("run container header does not fit in the available space");
				nruns = run_nruns_raw(c);
				if (nruns > LION_RUN_MAX_NRUNS)
					LION_CHECK_FAIL("run container has more than LION_RUN_MAX_NRUNS runs");
				if (lion_container_size_for(LION_CT_RUN, card, nruns) > avail_bytes)
					LION_CHECK_FAIL("run container does not fit in the available space");
				if (nruns == 0 && card != 0)
					LION_CHECK_FAIL("run container cardinality does not match its runs");
				runs = run_cdata(c);
				for (i = 0; i < nruns; i++)
				{
					int32		start = (int32) runs[i].start;
					int32		last = run_end(&runs[i]); /* not clamped */

					if (last > (int32) LION_LO_MAX)
						LION_CHECK_FAIL("run container run extends past the container range");
					if (start <= prev_last + 1)
						LION_CHECK_FAIL("run container runs are not ascending, disjoint and merged");
					prev_last = last;
					total += (uint32) (last - start + 1);
				}
				if (total != card)
					LION_CHECK_FAIL("run container cardinality does not match its runs");
				break;
			}
	}

	Assert(lion_container_size(c) <= avail_bytes);
	return true;
}

/*
 * The bits of BITSET word `word` whose members are tuples.  A word lies
 * inside one heap block's lo values (LION_BITSET_WORDS_PER_BLOCK), of which
 * it covers the offsets base .. base + 63.
 */
static uint64
bitset_tuple_mask(uint32 word, uint32 maxoff)
{
	uint32		base = (word % LION_BITSET_WORDS_PER_BLOCK) * 64;
	uint32		first = Max(base, 1);
	uint32		last = Min(base + 63, maxoff);
	uint64		mask;

	if (first > last)
		return 0;
	mask = (last - base == 63) ? ~UINT64CONST(0) :
		(UINT64CONST(1) << (last - base + 1)) - 1;
	return mask & ~((UINT64CONST(1) << (first - base)) - 1);
}

bool
lion_container_check_offsets(const LionContainer *c, uint32 maxoff,
							 const char **errmsg)
{
	*errmsg = NULL;

	Assert(maxoff < (1U << LION_OFFSET_BITS));

	switch (c->type)
	{
		case LION_CT_ARRAY:
			{
				const uint16 *arr = array_cdata(c);
				uint32		n = array_card(c);
				uint32		i;

				for (i = 0; i < n; i++)
				{
					if (!lion_lo_is_tuple(arr[i], maxoff))
						LION_CHECK_FAIL("array container member is not a heap tuple offset");
				}
				break;
			}
		case LION_CT_BITSET:
			{
				const uint64 *w = bitset_cdata(c);
				uint32		k;

				for (k = 0; k < LION_BITSET_WORDS; k++)
				{
					if ((w[k] & ~bitset_tuple_mask(k, maxoff)) != 0)
						LION_CHECK_FAIL("bitset container member is not a heap tuple offset");
				}
				break;
			}
		case LION_CT_NARROW:
			{
				const uint64 *w = narrow_cdata(c);
				uint32		k;

				/* a NARROW's word covers the offsets its BITSET word does */
				for (k = 0; k < LION_NARROW_WORDS; k++)
				{
					if ((w[k] & ~bitset_tuple_mask(narrow_bits_word(k), maxoff)) != 0)
						LION_CHECK_FAIL("narrow container member is not a heap tuple offset");
				}
				break;
			}
		case LION_CT_RUN:
			{
				const LionRun *runs = run_cdata(c);
				uint32		nruns = run_nruns(c);
				uint32		i;

				for (i = 0; i < nruns; i++)
				{
					uint32		start = runs[i].start;
					uint32		last = (uint32) run_end(&runs[i]);

					if (!lion_lo_is_tuple(start, maxoff) ||
						!lion_lo_is_tuple(last, maxoff) ||
						(start >> LION_OFFSET_BITS) != (last >> LION_OFFSET_BITS))
						LION_CHECK_FAIL("run container run is not inside the tuple offsets of one heap block");
				}
				break;
			}
		default:
			LION_CHECK_FAIL("invalid container type");
	}

	return true;
}
