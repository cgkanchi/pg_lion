/*-------------------------------------------------------------------------
 * lion_vec_math.h
 *	  The arithmetic of lionvec (DESIGN.md §44.3 - §44.4): the rotation, the
 *	  bit-plane codes a vector is stored as, and the estimates, with their
 *	  error bounds, that a scan computes from those codes.
 *
 *	  Every vector is padded with zeros to a whole number of 64-bit words,
 *	  dimsp dimensions, rotated by a fixed pseudo-random rotation and centred
 *	  on the indexed rows' mean (also rotated), which leaves a vector x whose
 *	  coordinates look like independent Gaussians of variance |x|^2 / dimsp.
 *	  The codes of x are
 *
 *		plane 1		the sign of each coordinate: one bit a dimension, and
 *					alpha1 = <sign(x) / sqrt(dimsp), x / |x|>
 *		planes 2-8	with plane 1, an 8-bit code a dimension: x_i sqrt(dimsp) / |x|,
 *					a standard Gaussian, rounded down to a 32nd within four
 *					standard deviations; r is the vector of the steps'
 *					middles, and alpha8 = <r / |r|, x / |x|>
 *
 *	  and the inner product of x with a query q is estimated from either as
 *	  |x| <c, q> / (|c| alpha), c the code's vector; the error of that is
 *	  the part of x / |x| that c misses, sqrt(1 - alpha^2) long and pointing
 *	  nowhere in particular after the rotation, times q, which is Gaussian
 *	  with standard deviation |q| sqrt(1 - alpha^2) / (alpha sqrt(dimsp - 1))
 *	  (RaBitQ, Gao and Long, SIGMOD 2024).  A bound of epsilon of those
 *	  either side holds with the Gaussian's probability, whatever the data.
 *
 *	  Plane 1 is compared with a query quantized to four bits a dimension,
 *	  as four more bit-planes, so that the inner product is popcounts of
 *	  ANDs; the rounding adds Delta^2 / 12 to the variance, Delta the step.
 *
 *	  A vector comes from pgvector's `vector`, whose elements are floats, or
 *	  its `halfvec`, whose elements are IEEE 754 binary16 halves and are
 *	  converted to floats as they are read; everything after that is the same.
 *
 *	  Frontend-safe (c.h and libpgcommon only), so that test/unit/vec_test.c
 *	  can check it without a server.
 *-------------------------------------------------------------------------
 */
#ifndef LION_VEC_MATH_H
#define LION_VEC_MATH_H

#include <math.h>

/* Metrics, numbered as the operator classes' ORDER BY strategies. */
#define LIONVEC_L2				1	/* <-> */
#define LIONVEC_IP				2	/* <#> */
#define LIONVEC_COSINE			3	/* <=> */
#define LIONVEC_NSTRATEGIES		3

/* The rotation: rounds of random signs and overlapping Hadamard blocks. */
#define LIONVEC_ROUNDS			3

/* The query's quantization: bits a dimension, so planes a word of plane 1. */
#define LIONVEC_QBITS			4

/*
 * The 8-bit codes' bound is twice as wide, in standard deviations, as plane
 * 1's: its deviation is about 1/70 of plane 1's, so twice as wide is still
 * far the tighter, and its failures - the rows a scan finds too late - all
 * but vanish.
 */
#define LIONVEC_EPS8(eps)		(2.0 * (eps))

/* dims padded to whole 64-bit words */
#define LIONVEC_PADDED(dims)	((((dims) + 63) / 64) * 64)

/*
 * pgvector's `vector` and `halfvec`, as they lay their datums out: a
 * varlena of a 16-bit dimension count, 16 unused bits and the elements,
 * floats or halves (IEEE 754 binary16, which lionvec_halves() converts).
 * pg_lion reads them here rather than linking pgvector, so that pg_lion
 * itself loads without pgvector.
 */
typedef struct LionVecPgVector
{
	int32		vl_len_;
	int16		dim;
	int16		unused;
	float		x[FLEXIBLE_ARRAY_MEMBER];
} LionVecPgVector;

typedef struct LionVecPgHalfVector
{
	int32		vl_len_;
	int16		dim;
	int16		unused;
	uint16		x[FLEXIBLE_ARRAY_MEMBER];
} LionVecPgHalfVector;

/*
 * The dimension count of a detoasted `vector` datum, or of a `halfvec` one
 * if half, of len bytes at p, its varlena header included; or -1 if those
 * bytes cannot be one: fewer than the header, a negative count, or fewer
 * than the count's elements take.
 */
extern int	lionvec_datum_dim(const void *p, Size len, bool half);

typedef struct LionVecRotation
{
	int			dimsp;
	int			nwin;			/* Hadamard windows: 1, or 2 overlapping */
	int			block;			/* their size, a power of two */
	int			win[2];			/* their starts */
	float	   *signs;			/* [LIONVEC_ROUNDS][nwin][dimsp], +-1 */
} LionVecRotation;

/* What a row's codes are, before they are laid out in its items. */
typedef struct LionVecEncoded
{
	float4		norm;			/* |x|, x centred */
	float4		alpha1;
	float4		alpha8;
	float4		rnorm8;			/* |r| */
	float4		xm;				/* <x, mean> */
	uint16		popcount;		/* set bits of plane 1 */
	uint64	   *bits;			/* [dimsp / 64], plane 1 */
	uint8	   *codes;			/* [dimsp], 8-bit codes */
} LionVecEncoded;

/*
 * A query, prepared against one index: centred and rotated as the rows are,
 * and quantized for plane 1.
 */
typedef struct LionVecQuery
{
	int			dimsp;
	float	   *qc;				/* [dimsp], q - mean, rotated */
	uint64	   *planes;			/* [dimsp / 64][LIONVEC_QBITS], interleaved */
	double		qlo;			/* the quantization: qc_i ~ qlo + qdelta * u_i */
	double		qdelta;
	double		qsum;			/* sum of the quantized qc_i */
	double		qnorm2;			/* |qc|^2 */
	double		var1;			/* |qc|^2 / (dimsp - 1): the codes' variance factor */
	double		varq;			/* the quantization's own variance, plane 1 only */
	double		eps;			/* the bounds' width, in standard deviations */
	double		offset;			/* key minus distance: lionvec_key_exact() */
} LionVecQuery;

/* the 8-bit codes: 256 steps of 1/32 over [-4, 4), and their middles */
#define LIONVEC_CLIP8			4.0f
#define LIONVEC_STEPS8			32.0f
extern float lionvec_recon8[256];

extern void lionvec_tables_init(void);
extern uint8 lionvec_code8(float z);

extern Size lionvec_rotation_nsigns(int dimsp);
extern void lionvec_rotation_init(LionVecRotation *rot, int dimsp, uint32 seed,
								  float *signs);
extern void lionvec_rotate(const LionVecRotation *rot, float *x);

extern bool lionvec_prepare(const LionVecRotation *rot, int metric,
							const float *v, int dims, float *x);
extern void lionvec_encode(const float *mean, int dimsp, float *x,
						   LionVecEncoded *e);

extern void lionvec_query_init(LionVecQuery *q, int metric, const float *mean,
							   const float *x, double eps);

/*
 * The kernels (DESIGN.md §44.3), chosen once by lionvec_kernels_init():
 *
 *	lionvec_plane_dot	sum over j of 2^j popcount(bits & plane j), the inner
 *						product of a row's sign bits with the quantized
 *						query's codes, over nwords words of each
 *	lionvec_sum8		sum over i < dimsp of lionvec_recon8[codes[i]] qc[i]
 *	lionvec_l2sq		sum over i < dimsp of (a[i] - b[i])^2
 *	lionvec_halves		x[i] = the float of the half h[i], i < n: a halfvec's
 *						elements, exactly, a NaN as the quiet NaN of its
 *						payload
 *
 * and the rotation's, inside lionvec_rotate().  The sums are in double, in
 * LIONVEC_LANES partial sums - the i-th term into partial i % LIONVEC_LANES
 * - added up in a fixed order, which every kernel keeps: each returns what
 * the portable one does, to the bit, as do the rotation's and the halves'
 * (lion_vec_math.c, "Kernels").  dimsp is a multiple of 64.
 */
#define LIONVEC_LANES			16

typedef uint32 (*LionVecDotKernel) (const uint64 *bits, const uint64 *planes,
									int nwords);
typedef double (*LionVecSum8Kernel) (const uint8 *codes, const float *qc,
									 int dimsp);
typedef double (*LionVecL2Kernel) (const float *a, const float *b, int dimsp);
typedef void (*LionVecHalfKernel) (const uint16 *h, int n, float *x);
extern LionVecDotKernel lionvec_plane_dot;
extern LionVecSum8Kernel lionvec_sum8;
extern LionVecL2Kernel lionvec_l2sq;
extern LionVecHalfKernel lionvec_halves;
extern void lionvec_kernels_init(void);

/* the sets of kernels, as the unit test forces them */
#define LIONVEC_KERNELS_PORTABLE	0
#define LIONVEC_KERNELS_POPCNT		1	/* x86-64: POPCNT for plane 1 */
#define LIONVEC_KERNELS_AVX2		2	/* x86-64: AVX2 for all; halves by
										 * F16C where the CPU has it */
#define LIONVEC_KERNELS_AVX512		3	/* x86-64: AVX2, plane 1 by VPOPCNTDQ */
#define LIONVEC_KERNELS_MAX			LIONVEC_KERNELS_AVX512
#ifdef FRONTEND
/* the unit test's way to run each set; false if the build or the CPU lacks it */
extern bool lionvec_kernel_force(int which);
/* ... and whether the set it forced converts halves with F16C */
extern bool lionvec_kernel_f16c(void);
#endif

extern void lionvec_estimate1(const LionVecQuery *q, const uint64 *bits,
							  uint16 popcount, float norm, float alpha1,
							  double *est, double *err);
extern void lionvec_estimate8(const LionVecQuery *q, const uint8 *codes,
							  float norm, float alpha8, float rnorm8,
							  double *est, double *err);

/*
 * Partitions (DESIGN.md §44.2): k centroids, each coded as a row is, and the
 * k-means that finds them.  A row belongs to the centroid nearest it by L2;
 * cosine's centroids are of unit length, as its rows are, so that nearest by
 * L2 is nearest by angle too.
 */
/* the width of the bounds a row's nearest centroids are found by */
#define LIONVEC_ASSIGN_EPS		4.0

typedef struct LionVecCentroids
{
	int			k;
	int			dimsp;
	const uint64 *bits;			/* [k][dimsp / 64]: plane 1 */
	const uint16 *popcount;		/* [k] */
	const float *norm;			/* [k]: |c|, c centred */
	const float *alpha1;		/* [k] */
} LionVecCentroids;

/*
 * The centroids that can be the nearest to the row the query q was made from
 * (lionvec_query_init(), LIONVEC_L2): each one's key bounds into lo[k] and
 * hi[k], and into cand[k] the indices of those whose lower bound is no more
 * than the least upper bound, which it returns the number of - at least one
 * when k is.  The bounds' width is q's.
 */
extern int	lionvec_candidates(const LionVecCentroids *c, const LionVecQuery *q,
							   double *lo, double *hi, int *cand);

/*
 * ... and the centroids that can be among the m nearest (1 <= m <=
 * LIONVEC_RANK_MAX), those whose lower bound is no more than the m-th least
 * upper bound: at least min(m, k) of them, as every centroid's lo <= hi, or
 * its bound is none - which holds when each norm is a non-negative number,
 * and which callers reading centroids from an index check rather than
 * assume.  m = 1 is lionvec_candidates().
 */
#define LIONVEC_RANK_MAX		16
extern int	lionvec_candidates_n(const LionVecCentroids *c, const LionVecQuery *q,
								 int m, double *lo, double *hi, int *cand);

/*
 * ... and among the n centroids idx[0 .. n) alone (lionvec_route()): the
 * candidates lionvec_candidates_n() would find were those the only
 * centroids, in idx's order - their bounds into lo[i] and hi[i] by their
 * places in idx, and into cand the centroids themselves.  idx NULL is every
 * centroid, n being c->k, which is lionvec_candidates_n().
 */
extern int	lionvec_candidates_among(const LionVecCentroids *c, const int *idx,
									 int n, const LionVecQuery *q, int m,
									 double *lo, double *hi, int *cand);

/*
 * lionvec_candidates_n() a centroid at a time, for a caller that reads the
 * centroids from pages and would rather not hold all k of them (an insert,
 * lion_vec.c): begin with m, add each centroid in turn - its bounds into *lo
 * and *hi, and whether it can still be a candidate, its lower bound being no
 * more than the m-th least upper bound so far - and at the end keep those
 * whose lower bound is no more than the threshold.  The threshold only
 * falls, so a centroid add() turns down is never a candidate; the end's set
 * and order are lionvec_candidates_n()'s, which is built of the same calls.
 */
typedef struct LionVecCandScan
{
	int			m;
	int			nleast;
	double		least[LIONVEC_RANK_MAX];	/* the m least upper bounds, ascending */
} LionVecCandScan;

extern void lionvec_cands_begin(LionVecCandScan *cs, int m);
extern bool lionvec_cands_add(LionVecCandScan *cs, const LionVecQuery *q,
							  const uint64 *bits, uint16 popcount, float norm,
							  float alpha1, double *lo, double *hi);
extern double lionvec_cands_threshold(const LionVecCandScan *cs);

/*
 * The m least of key[0 .. n), as their places in key, least first: into
 * order[], which must hold min(n, m) of them, and how many that is.  Ties go
 * to the lower place, and a NaN key counts as infinite.
 */
extern int	lionvec_rank(const double *key, int n, int m, int *order);

/*
 * Spill (DESIGN.md §44.3): an index that spills keeps every row with a
 * vector in two partitions, its nearest centroid c1's and a second, chosen
 * by SOAR (Sun and others, NeurIPS 2023): of the LIONVEC_SOAR_CANDS
 * centroids next nearest after c1, the one that minimizes
 *
 *		|x - c|^2 + lambda <r / |r|, x - c>^2,		r = x - c1
 *
 * - a centroid near the row, and off the line of its residual r, so that a
 * query whose nearest rows are the row lies where c1's partition was likely
 * to be passed by (where the query lies along r beyond the row) finds the
 * row in the second instead.  lambda = 0 is the plain second-nearest.
 * lionvec_soar_cost() is that cost of a centroid c, x, c1 and c dimsp
 * floats alike centred; lionvec_soar_second() the place in cands[0 .. n) of
 * the one of least cost, ties to the lower place (the nearer, when cands are
 * nearest first), or -1 when n is 0.  A row at c1 has no residual and costs
 * |x - c|^2; a cost that is not a number counts as infinite.
 */
#define LIONVEC_SOAR_CANDS		8
#define LIONVEC_SOAR_LAMBDA		1.0
extern double lionvec_soar_cost(const float *x, const float *c1, const float *c,
								int dimsp, double lambda);
extern int	lionvec_soar_second(const float *x, const float *c1,
								const float *const *cands, int n, int dimsp,
								double lambda);

/*
 * A vector from its 8-bit codes, norm (|x|), alpha8 and rnorm8 (|r|), as an
 * index stores a centroid's: x's projection on r, |x| alpha8 r / |r|, the
 * nearest vector to x in r's direction, |x| sqrt(1 - alpha8^2) from it.
 * Zeros for codes with no length.
 */
extern void lionvec_decode8(const uint8 *codes, int dimsp, float norm,
							float alpha8, float rnorm8, float *x);

/*
 * A sample of the rows, for k-means: each one rotated, not centred (the
 * mean is not known when the sample is taken), and held as its 8-bit codes,
 * so that a sample of 1536 dimensions takes 1.5 KB a row, not 6.  Row i is
 * scale[i] * lionvec_recon8[codes[i * dimsp + j]], j < dimsp.
 */
typedef struct LionVecSample
{
	int			n;
	int			dimsp;
	const uint8 *codes;			/* [n][dimsp] */
	const float *scale;			/* [n]: |x| / |r| */
} LionVecSample;

extern void lionvec_sample_row(const LionVecSample *s, int i, float *x);

/*
 * The sample k-means is given (lion_vec.c, DESIGN.md §44.3) is at most a
 * fixed number of rows a partition of the reservoir's, which may hold many
 * more: m of the slots [0, n), 0 <= m <= n, chosen uniformly at random by a
 * partial Fisher-Yates shuffle from a pg_prng seeded with seed, into
 * slot[0 .. m) in ascending order - so that the rows they name, moved down
 * in place, keep the reservoir's order, which k-means' update sums them in.
 * slot must hold n; past m it is scratch.
 */
extern void lionvec_subsample(int n, int m, uint64 seed, uint32 *slot);

/*
 * How many rows of a reservoir of sn k-means is given, for nparts
 * partitions (DESIGN.md §44.3): LIONVEC_SAMPLE_PER_PART_MAX a partition at
 * most - an iteration costs its rows, and past some rows a partition more
 * of them move the centroids less than the iterations do (FAISS warns below
 * 39 and cuts above 256; on DBpedia's 115,386 rows in 679 partitions a cut
 * to 64 lost a hundredth of the true ten at a margin of 3, 0.899 over three
 * draws against 0.909 from every row, and 128 none, 0.917) - but never
 * fewer than LIONVEC_SAMPLE_MIN, so that a table of fewer rows than that
 * is built from every row it sampled, whatever its partitions, as it was
 * before the cut: k-means over that many rows is seconds at most, and the
 * regression tests' expected outputs (the partitions a query probes, the
 * pages it reads) rest on small tables' centroids.  At the default
 * 8 sqrt(n) partitions the cut is 1,024 sqrt(n) rows, past the floor from
 * 4,096 rows and every row up to a million, so the floor changes nothing
 * there; it holds where the partitions reloption is set low on a larger
 * table.  The cut is sn when there is none, as with no partitions.
 */
#define LIONVEC_SAMPLE_PER_PART_MAX	128
#define LIONVEC_SAMPLE_MIN			65536

static inline int
lionvec_kmeans_rows(int sn, int nparts)
{
	int64		m = (int64) LIONVEC_SAMPLE_PER_PART_MAX * nparts;

	if (m < LIONVEC_SAMPLE_MIN)
		m = LIONVEC_SAMPLE_MIN;
	return m < sn ? (int) m : sn;
}

/*
 * Regions (DESIGN.md §44.3): the build's k centroids, when there are
 * LIONVEC_REGION_MIN of them or more, gathered by k-means over the centroids
 * into lionvec_region_count(k) regions, about sqrt(k), so that the centroid
 * nearest a row is looked for among the centroids of the regions nearest it
 * alone (lionvec_route()): an estimate of each region's centroid and of the
 * beam regions' sqrt(k) members each, not of all k, so that a row costs
 * (beam + 1) sqrt(k) estimates, not k.  k-means' iterations find each
 * sample row's centroid so, the centroids regrouped at the start of each;
 * and so do the build's second pass and inserts, by the regions of the
 * final centroids, which the index keeps (its REGION chain).  A row whose
 * nearest centroid is in none of its beam's regions goes to the nearest
 * that is, where a scan finds it later.  Scans rank every centroid still:
 * ranked through the regions, the partitions near a query came out in an
 * order that read a third more rows for the same recall in the simulation.
 *
 * The beams are constants, not a share of the regions, as they must be for
 * that cost to hold: on DBpedia (DESIGN.md §44.9) five regions found the
 * nearest centroid exact assignment did for 98.8% of the rows in 53
 * regions and 99.5% in 100, where four found it for 98.3% in 53; a
 * spilling build's nine nearest, of which SOAR picks the copy's partition,
 * lie farther out and take sixteen (the copy's partition for 99.5% of the
 * rows in 27 regions, 98.6% in 53 and 97.9% in 100, where five gave 88% in
 * 100 and thirteen 96.95%).  k-means wants the nearest alone; the second
 * pass of a build that spills, once over the rows, and an insert into one,
 * the nine.
 */
#define LIONVEC_REGION_MIN		64	/* the fewest centroids put in regions */
#define LIONVEC_REGION_ITERS	5	/* k-means' iterations over the centroids */
#define LIONVEC_BEAM			5	/* regions a row's nearest is looked for in */
#define LIONVEC_BEAM_SOAR		16	/* ... and its LIONVEC_SOAR_CANDS + 1 nearest */
/* the seed of the regions' k-means, from that of the rows' */
#define LIONVEC_REGION_SEED(seed)	((seed) + 1)

/* the regions of k centroids: none below LIONVEC_REGION_MIN, else ceil(sqrt(k)) */
static inline int
lionvec_region_count(int k)
{
	int			n;

	if (k < LIONVEC_REGION_MIN)
		return 0;
	n = (int) ceil(sqrt((double) k));
	return n < k ? n : k;
}

/*
 * ... and how many of n a row is routed to, for its m nearest centroids:
 * LIONVEC_BEAM for the nearest alone, LIONVEC_BEAM_SOAR for more, never more
 * than there are.  The build and an insert ask with the same n and m, and so
 * look in the same regions.
 */
static inline int
lionvec_region_beam(int n, int m)
{
	int			beam = m > 1 ? LIONVEC_BEAM_SOAR : LIONVEC_BEAM;

	return beam < n ? beam : n;
}

/*
 * k centroids in n regions: region g's centroid, rotated and coded as a row
 * is, centred on the rows' mean - plane 1 alone - and its members,
 * member[first[g]] .. member[first[g + 1] - 1], ascending; a region may have
 * none.  A row is routed to beam of them (lionvec_region_beam(n)).  The
 * arrays are the caller's: a parallel build's are in shared memory, by
 * which every process routes alike.
 */
typedef struct LionVecRegions
{
	int			n;				/* regions; 0: none */
	int			k;				/* the centroids they gather */
	int			dimsp;
	uint64	   *bits;			/* [n][dimsp / 64]: their centroids' plane 1 */
	uint16	   *popcount;		/* [n] */
	float	   *norm;			/* [n] */
	float	   *alpha1;			/* [n] */
	int		   *first;			/* [n + 1] */
	int		   *member;			/* [k] */
} LionVecRegions;

/* What lionvec_route() works in, a process's own, for n regions of k centroids. */
typedef struct LionVecRoute
{
	double	   *key;			/* [n]: each region's key for the row */
	int		   *order;			/* [n] */
	int		   *pick;			/* [n]: the regions it looks in */
	int		   *idx;			/* [k]: ... and their members */
} LionVecRoute;

/*
 * What lionvec_kmeans() works in: the caller's arrays, of the lengths given,
 * so that it allocates nothing.
 */
typedef struct LionVecKmeansWork
{
	int		   *count;			/* [k] */
	int		   *assign;			/* [n] */
	uint64	   *bits;			/* [k][dimsp / 64] */
	uint16	   *popcount;		/* [k] */
	float	   *norm;			/* [k] */
	float	   *alpha1;			/* [k] */
	double	   *lo;				/* [k] */
	double	   *hi;				/* [k] */
	int		   *cand;			/* [k] */
	float	   *x;				/* [dimsp] */
	float	   *y;				/* [dimsp] */
	uint8	   *codes;			/* [dimsp] */
	float	   *qc;				/* [dimsp] */
	uint64	   *planes;			/* [dimsp / 64][LIONVEC_QBITS] */
	LionVecRoute route;			/* with regions: each row's routing; else unused */
} LionVecKmeansWork;

/*
 * A region's key for the row q was made from (LIONVEC_L2): plane 1's
 * estimate of the squared distance of the region's centroid (bits,
 * popcount, norm and alpha1) from the row, less |x|^2, which is the same
 * for every region; infinite when that is not a number.
 */
extern double lionvec_region_key(const LionVecQuery *q, const uint64 *bits,
								 uint16 popcount, float norm, float alpha1);

/*
 * The regions a row looks in, of n of keys key[n] and members first[g] ..
 * first[g + 1] - 1: the beam nearest it that have members, least key first,
 * ties to the lower region (lionvec_rank()), and then more, nearer first,
 * until they have need members between them or none are left - so that a
 * spilling build's m nearest are there to be ranked however the centroids
 * fell into regions.  Into pick[], ascending, so that their members come in
 * the centroids' order; returns how many.  order[] is scratch; both hold n.
 */
extern int	lionvec_regions_pick(const double *key, const int *first, int n,
								 int beam, int need, int *order, int *pick);

/*
 * The candidates that can be among the m nearest of the row q was made from
 * among the members of its regions (lionvec_regions_pick(), need m;
 * lionvec_candidates_among()), into cand, and their bounds into lo and hi
 * by their places among those members; returns their number.  rt is scratch
 * for r's regions and centroids.
 */
extern int	lionvec_route(const LionVecRegions *r, const LionVecCentroids *c,
						  const LionVecQuery *q, int m, LionVecRoute *rt,
						  double *lo, double *hi, int *cand);

/*
 * What lionvec_regions_make() works in, for k centroids and n regions: the
 * k-means over the centroids, their 8-bit codes its sample, and the regions'
 * centroids it finds.
 */
typedef struct LionVecRegionMaker
{
	LionVecKmeansWork w;		/* for k rows and n centroids; no route */
	uint8	   *codes;			/* [k][dimsp] */
	float	   *scale;			/* [k] */
	float	   *cent;			/* [n][dimsp]: rotated, not centred */
} LionVecRegionMaker;

/*
 * Lloyd's k-means of the sample's n rows into k <= n centroids cent[k][dimsp]
 * (rotated, not centred), over iters iterations from k distinct rows the
 * seed picks; unit, the centroids are scaled to unit length after each
 * (spherical k-means, for cosine).  Each row goes to its nearest centroid by
 * L2, found among lionvec_candidates() by the exact distance - or, given
 * regions r of the k centroids (and mk to make them in), among
 * lionvec_route()'s, the centroids regrouped (lionvec_regions_make(),
 * LIONVEC_REGION_SEED(seed)) at the start of each iteration; a centroid
 * left with no rows takes half of the largest's (FAISS's way).  mean, the
 * rows' (rotated; NULL: zero), centres the codes the candidates are found
 * by, which only makes their bounds tighter.  Returns the iterations made:
 * fewer than iters once an iteration moves no row.
 */
extern int	lionvec_kmeans(const LionVecSample *s, int k, int iters, bool unit,
						   uint64 seed, const float *mean, float *cent,
						   LionVecKmeansWork *w, LionVecRegions *r,
						   LionVecRegionMaker *mk);

/*
 * Gather the r->k centroids cent[k][dimsp] (rotated, not centred) into r->n
 * <= k regions: Lloyd's k-means of the centroids, coded to 8 bits as a
 * sample's rows are, LIONVEC_REGION_ITERS iterations from the centroids the
 * seed picks, spherical when unit; then each centroid into the region whose
 * centroid is nearest it after the last update (as lionvec_kmeans_assign()
 * finds a row's), and the regions' centroids coded against mean.  The same
 * centroids and seed make the same regions, to the bit.
 */
extern void lionvec_regions_make(LionVecRegions *r, const float *cent,
								 bool unit, uint64 seed, const float *mean,
								 LionVecRegionMaker *mk);

/*
 * lionvec_kmeans()'s steps, for a build that shares an iteration's rows
 * between processes (DESIGN.md §44.12): it is start(), then until iters are
 * made or assign() moves no row, codes(), with regions
 * lionvec_regions_make(), assign() over every row and update().  Each row's
 * assignment depends on the centroids (and their regions, made of them)
 * alone, and update() sums the rows in their order, so that the centroids
 * come out the same however assign()'s rows are shared out.
 *
 * start: the k rows the seed picks into cent, unit length when unit, and
 * every assign[n] -1.  codes: the centroids' plane 1 into w's bits,
 * popcount, norm and alpha1, centred by mean (w->x and w->codes its
 * scratch).  assign: rows [from, to) each to its nearest centroid, into
 * assign[], routed through the regions r unless r is NULL; returns how many
 * moved.  It reads w's bits, popcount, norm and alpha1 and r, and works in
 * w's lo, hi, cand, y, qc, planes and route, none of which another assign()
 * may be using at the same time.  update: each centroid
 * the mean of its rows (w->count, w->y its scratch), an empty one half of
 * the largest's, unit length when unit.
 */
extern void lionvec_kmeans_start(const LionVecSample *s, int k, bool unit,
								 uint64 seed, float *cent, int *assign);
extern void lionvec_kmeans_codes(int k, int dimsp, const float *mean,
								 const float *cent, LionVecKmeansWork *w);
extern int	lionvec_kmeans_assign(const LionVecSample *s, int from, int to,
								  int k, const float *mean, const float *cent,
								  int *assign, LionVecKmeansWork *w,
								  const LionVecRegions *r);
extern void lionvec_kmeans_update(const LionVecSample *s, int k, bool unit,
								  const int *assign, float *cent,
								  LionVecKmeansWork *w);

/*
 * The scan orders rows by a KEY that is the distance up to a constant of the
 * query: what changes from row to row of an estimate of <x, qc>.
 *
 *	L2		|x - q|^2 = |x|^2 - 2 <x, qc> + |qc|^2		key = |x|^2 - 2 <x, qc>
 *	IP		-<x + m, q> = -<m, q> - <x, m> - <x, qc>	key = -<x, m> - <x, qc>
 *	cosine	the same, of q / |q| and of the rows' unit vectors, minus 1
 *
 * x the centred row, m the mean.  lionvec_key_bounds() gives the key's
 * bounds from an estimate of <x, qc> and its error, and lionvec_key_exact()
 * the key of a distance computed exactly, given the query's offset
 * (lionvec_query_init()): |qc|^2 for L2, <m, q> for IP, <m, q> - 1 for
 * cosine.  A bound that is not a number - a vector so large its norm
 * overflowed - is no bound at all, and is returned as an infinite one.
 */
static inline void
lionvec_key_bounds(int metric, double norm, double xm, double est, double err,
				   double *lo, double *hi)
{
	if (metric == LIONVEC_L2)
	{
		double		base = norm * norm;

		*lo = base - 2.0 * (est + err);
		*hi = base - 2.0 * (est - err);
	}
	else
	{
		*lo = -xm - est - err;
		*hi = -xm - est + err;
	}
	if (isnan(*lo) || isnan(*hi))
	{
		*lo = -INFINITY;
		*hi = INFINITY;
	}
}

static inline double
lionvec_key_exact(int metric, double dist, double offset)
{
	return metric == LIONVEC_L2 ? dist * dist - offset : dist + offset;
}

/*
 * An inner product's lower bound on the keys of a partition's rows, from a
 * lower bound lo on its centroid c's key, as a scan ranks the partitions
 * (DESIGN.md §44.4).  Each row x is lifted by one more coordinate,
 * sqrt(maxnorm2 - |x|^2), maxnorm2 the partition's greatest |x|^2: the rows
 * then lie at one distance from the origin, and the nearer one lies to the
 * unit query (q / |q|, 0), the greater its <q, x> (Bachrach and others,
 * RecSys 2014).  The rows' centroid lies inside that sphere by spread, their
 * mean |x - c|^2, at squared distance 1 + maxnorm2 - spread - 2 <q / |q|, c>
 * from the query; the bound is the key of a row margin nearer than that, so
 * a margin as large as the lifted rows' greatest distance from their
 * centroid makes it a true bound.  offset and qnorm are the query's offset
 * and |q|; a zero query, whose every key is the same, gets lo back.
 */
static inline double
lionvec_lift_key(double lo, double offset, double qnorm, double maxnorm2,
				 double spread, double margin)
{
	double		qc;
	double		d;

	if (isinf(lo) || !(qnorm > 0.0))
		return lo;
	qc = -(lo - offset) / qnorm;	/* <q / |q|, c>, at most */
	d = sqrt(Max(1.0 + maxnorm2 - spread - 2.0 * qc, 0.0)) - margin;
	if (d < 0.0)
		d = 0.0;
	return qnorm * (d * d - 1.0 - maxnorm2) / 2.0 + offset;
}

#endif							/* LION_VEC_MATH_H */
