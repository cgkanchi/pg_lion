/*-------------------------------------------------------------------------
 *
 * lion_vec.h
 *		lionvec: a nearest-neighbour index access method for pgvector's
 *		`vector` type, shipped beside lion (DESIGN.md section 44).
 *
 *		Every vector is rotated by a fixed random rotation, centred on the
 *		table's mean and quantized into eight nested bit-planes.  The rows
 *		are partitioned by k-means, and a scan reads the partitions whose
 *		centroids are nearest the query, nearest first, until the next
 *		partition's rows are taken to lie beyond the rows it has.  The first
 *		plane (one bit per dimension) is all a scan reads of a partition's
 *		rows; it gives each row's distance to the query with an error bound
 *		that holds with a chosen probability whatever the data, so only the
 *		rows that bound cannot rule out are refined from the other seven
 *		planes, and only the few those cannot order are compared exactly,
 *		from the heap.
 *
 *		The index is created by the pg_lion_vector extension, which needs
 *		pgvector; this library reads pgvector's on-disk format itself and
 *		calls its distance functions only through the operator class, so
 *		pg_lion loads without pgvector installed.
 *
 *		lion_vec.c holds the format, the build, inserts, VACUUM, costs and
 *		the handler; lion_vec_scan.c the scan; lion_vec_math.c (behind
 *		lion_vec_math.h, which the unit test builds without a server) the
 *		rotation, the codes and the bounds.  The rest of pg_lion sees only
 *		the functions at the end: LionOrdered (lion_ordered.c) uses them to
 *		filter a lionvec scan by lion's exact TID set (§44.6).
 *
 *-------------------------------------------------------------------------
 */
#ifndef LION_VEC_H
#define LION_VEC_H

#include "access/genam.h"
#include "access/relscan.h"
#include "storage/bufmgr.h"
#include "nodes/pathnodes.h"
#include "storage/bufpage.h"
#include "utils/relcache.h"

#include "lion_container.h"
#include "lion_vec_math.h"

/* ---------------------------------------------------------------------
 * Format (DESIGN.md §44.2)
 * --------------------------------------------------------------------- */

#define LIONVEC_MAGIC			0x4c56454d	/* "LVEM" */
#define LIONVEC_VERSION			2
#define LIONVEC_METAPAGE		0

/* The rotation's seed.  Kept in the meta page, so it can change later. */
#define LIONVEC_SEED			0x6c696f6e	/* "lion" */

/* Page types, in the special space of every page. */
#define LIONVEC_PAGE_META		0
#define LIONVEC_PAGE_MEAN		1
#define LIONVEC_PAGE_CODE		2
#define LIONVEC_PAGE_REFINE		3
#define LIONVEC_PAGE_PART		4

/* Identifies a lionvec page beside other AMs' (as GIN's and GiST's do). */
#define LIONVEC_PAGE_ID			0xFF8C

typedef struct LionVecPageOpaqueData
{
	BlockNumber next;			/* the next page of this chain */
	uint32		minkey;			/* CODE: least container key of its rows */
	uint32		maxkey;			/* CODE: greatest */
	uint16		type;			/* LIONVEC_PAGE_* */
	uint16		page_id;		/* LIONVEC_PAGE_ID */
} LionVecPageOpaqueData;

typedef LionVecPageOpaqueData *LionVecPageOpaque;

#define LionVecPageGetOpaque(page) \
	((LionVecPageOpaque) PageGetSpecialPointer(page))

typedef struct LionVecMetaData
{
	uint32		magic;
	uint32		version;
	uint16		metric;			/* LIONVEC_L2 / _IP / _COSINE */
	uint16		flags;
	int32		dims;			/* 0: no vector seen yet */
	int32		dimsp;			/* dims padded to a multiple of 64 */
	uint32		seed;			/* the rotation's */
	BlockNumber mean_head;		/* InvalidBlockNumber: the mean is zero */
	BlockNumber part_head;		/* the PART chain, or InvalidBlockNumber */
	uint32		nparts;			/* the partitions it holds */
	BlockNumber rest_head;		/* the CODE chain of rows in no partition */
	BlockNumber rest_tail;
	BlockNumber null_head;		/* the CODE chain of NULL rows and cosine zeros */
	BlockNumber null_tail;
	BlockNumber refine_tail;	/* where the next REFINE item goes */
	float4		scale;			/* the rows' standard deviation about their
								 * centroids along one dimension, as built: the
								 * unit of pg_lion.vector_margin */
	double		ntuples;		/* rows as of the build */
} LionVecMetaData;

typedef LionVecMetaData *LionVecMeta;

#define LionVecPageGetMeta(page) ((LionVecMeta) PageGetContents(page))

/*
 * A row's first plane: what every scan reads.  NULL rows and the zero vectors
 * of a cosine index carry the header alone.
 */
#define LIONVEC_NULL			0x0001	/* the column is NULL */
#define LIONVEC_ZERO			0x0002	/* cosine: a zero vector, distance NaN */

typedef struct LionVecCode
{
	ItemPointerData tid;
	uint16		flags;
	float4		norm;			/* |x|, x the row padded, rotated and centred */
	float4		alpha1;			/* <sign(x) / sqrt(dimsp), x / |x|> */
	float4		xm;				/* <x, mean>: the IP and cosine keys' */
	BlockNumber rblk;			/* the REFINE item */
	OffsetNumber roff;
	uint16		popcount;		/* set bits of the plane */
	uint64		bits[FLEXIBLE_ARRAY_MEMBER];	/* dimsp / 64 words */
} LionVecCode;

#define LIONVEC_CODE_HDRSZ		offsetof(LionVecCode, bits)
#define LIONVEC_CODE_SIZE(dimsp) (LIONVEC_CODE_HDRSZ + (Size) (dimsp) / 8)

/* ... and the rest of its planes, read only for the rows plane 1 keeps. */
typedef struct LionVecRefine
{
	ItemPointerData tid;		/* the row's, to tell a reused slot (§44.5) */
	uint16		pad;
	float4		alpha8;			/* <r / |r|, x / |x|>, r the codes' vector */
	float4		rnorm8;			/* |r| */
	uint8		codes[FLEXIBLE_ARRAY_MEMBER];	/* dimsp 8-bit codes */
} LionVecRefine;

#define LIONVEC_REFINE_SIZE(dimsp) (offsetof(LionVecRefine, codes) + (Size) (dimsp))

/*
 * A partition (DESIGN.md §44.2), an item of the PART chain: where its CODE
 * chain starts and ends, how many rows the build put in it, and the centroid
 * itself, coded as a row is - its plane 1 here and its 8-bit codes in a
 * REFINE item, whose TID is (partno, 0), which no heap row's is.  The build
 * writes the PART chain last; an insert changes only a partition's tail.
 */
typedef struct LionVecPart
{
	uint32		partno;			/* its place in the chain, from 0 */
	BlockNumber head;			/* its CODE chain */
	BlockNumber tail;			/* ... whose last page inserts append to */
	uint32		nrows;			/* rows the build put in it */
	LionVecCode cent;			/* the centroid's plane 1, as a row's */
} LionVecPart;

#define LIONVEC_PART_SIZE(dimsp) \
	(offsetof(LionVecPart, cent) + LIONVEC_CODE_SIZE(dimsp))

/* the TID of a centroid's REFINE item */
#define LionVecCentTid(tid, partno) \
	ItemPointerSet((tid), (BlockNumber) (partno), InvalidOffsetNumber)

/*
 * The most dimensions an index takes: as many whole words as leave a REFINE
 * item, its line pointer and the special space room on a page (8128 at 8K).
 * pgvector's `vector` goes to 16000, its HNSW and IVFFlat indexes to 2000.
 */
#define LIONVEC_MAX_DIM \
	((int) ((BLCKSZ - MAXALIGN(SizeOfPageHeaderData + sizeof(ItemIdData)) - \
			 MAXALIGN(sizeof(LionVecPageOpaqueData)) - \
			 MAXALIGN(offsetof(LionVecRefine, codes))) / 64 * 64))

/* ---------------------------------------------------------------------
 * What a backend keeps per index (rd_amcache) and per encoded vector
 * --------------------------------------------------------------------- */

typedef struct LionVecInfo
{
	int			metric;
	int			dims;			/* 0: no vector indexed yet */
	int			dimsp;
	uint32		seed;
	LionVecRotation rot;		/* its signs follow this struct */
	float	   *mean;			/* [dimsp], rotated; NULL when zero */
} LionVecInfo;

/*
 * The container key of a heap TID, as lion's sets are keyed (lion_tid.h),
 * from its block alone: what a CODE page's minkey and maxkey hold.
 */
static inline uint32
lionvec_ckey(ItemPointer tid)
{
	return (uint32) (ItemPointerGetBlockNumber(tid) >>
					 (LION_CONTAINER_BITS - LION_OFFSET_BITS));
}

/*
 * One more page of a chain: a chain longer than the relation has pages has
 * a cycle in it, which would otherwise be walked forever.  The relation may
 * have grown since *nblocks was read, so it is read again before saying so.
 */
static inline void
lionvec_chain_step(Relation index, uint64 *visited, BlockNumber *nblocks)
{
	if (++(*visited) > *nblocks)
	{
		*nblocks = RelationGetNumberOfBlocks(index);
		if (*visited > *nblocks)
			ereport(ERROR,
					(errcode(ERRCODE_INDEX_CORRUPTED),
					 errmsg("lionvec index \"%s\" has a cycle in a page chain",
							RelationGetRelationName(index))));
	}
}

/*
 * Settings.  The margin is in standard deviations of the rows about their
 * centroids (LionVecMetaData's scale); a restriction narrows it by
 * LIONVEC_MARGIN_NARROW for each tenfold it thins the rows by
 * (lion_vec_scan.c, lv_margin()).
 */
#define LIONVEC_MARGIN_DEFAULT	2.0
#define LIONVEC_MARGIN_NARROW	1.3
#define LIONVEC_MAX_PARTS		1000000

/*
 * A scan restricted to this many members or fewer computes every member's
 * distance from the heap rather than probe partitions (DESIGN.md §44.6).
 */
#define LIONVEC_EXACT_MAX		2000
extern double lionvec_epsilon;
extern int	lionvec_batch;
extern double lionvec_margin;

/* lion_vec.c */
extern void lion_vec_init(void);
extern LionVecInfo *lionvec_get_info(Relation index);
extern const float *lionvec_datum_floats(Datum d, int *dim, Pointer *tofree);
extern void lionvec_check_dims(const LionVecInfo *info, int dim);
extern void lionvec_read_meta(Relation index, LionVecMetaData *meta);
extern void lionvec_check_page(Relation index, Page page, BlockNumber blk,
							   uint16 type);
extern LionVecCode *lionvec_code_at(Relation index, Page page, BlockNumber blk,
									OffsetNumber off, int dimsp);
extern LionVecRefine *lionvec_refine_at(Relation index, Page page,
										BlockNumber blk, OffsetNumber off,
										int dimsp, ItemPointer tid);
extern LionVecPart *lionvec_part_at(Relation index, Page page, BlockNumber blk,
									OffsetNumber off, int dimsp);
extern BlockNumber *lionvec_chain_heads(Relation index,
										const LionVecMetaData *meta, int *n);

/* lion_vec_scan.c: the scan callbacks */
extern IndexScanDesc lionvecbeginscan(Relation index, int nkeys, int norderbys);
extern void lionvecrescan(IndexScanDesc scan, ScanKey keys, int nkeys,
						  ScanKey orderbys, int norderbys);
extern bool lionvecgettuple(IndexScanDesc scan, ScanDirection dir);
extern void lionvecendscan(IndexScanDesc scan);

/*
 * For LionOrdered (DESIGN.md §44.6): is this a lionvec index - asked of the
 * planner's IndexOptInfo or of the open relation?  By the access method's own
 * routines, not its name, so that an access method of that name some other
 * handler serves is never taken for one.
 */
extern bool lion_vec_index(IndexOptInfo *index);
extern bool lion_vec_relation(Relation index);

/*
 * ... and restrict a lionvec scan, after its
 * rescan and before its first tuple, to the members of a set of heap TIDs -
 * n container keys in ascending order and their containers, a NULL
 * container meaning every TID of that key's heap blocks.  The arrays must
 * outlive the scan, or the next rescan.
 */
extern void lion_vec_restrict(IndexScanDesc scan, int n, const uint32 *keys,
							  LionContainer *const *conts);

/*
 * ... and what a scan of it restricted to `members` of its `tuples` rows
 * costs before its first row, for the planner (§44.7).
 */
extern Cost lion_vec_scan_cost(IndexOptInfo *index, double members);

#endif							/* LION_VEC_H */
