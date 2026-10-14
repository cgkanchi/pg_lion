/*-------------------------------------------------------------------------
 *
 * lion_vec.c
 *		lionvec, the nearest-neighbour index of pgvector's `vector`: its
 *		handler, settings, pages, build, inserts, VACUUM and costs.  The scan
 *		is lion_vec_scan.c, the arithmetic lion_vec_math.c (DESIGN.md §44).
 *
 * The index is chains of pages under a meta page:
 *
 *	MEAN	the indexed rows' mean, rotated, as floats; written by the build
 *			and never changed
 *	PART	one item a partition: its centroid's plane 1 and where its CODE
 *			chain is.  The build finds the centroids by k-means over a sample
 *			of the rows, and puts each row in the partition of the nearest.
 *	CODE	one item a row: its TID, plane 1 and the numbers its estimate
 *			needs; a chain of them a partition, which the build writes in one
 *			run of blocks, in heap order, so that a scan reads a partition in
 *			block order.  An index that spills (the `spill` option) has a
 *			second item, a copy, of every row with a vector, in the chain of
 *			a second partition SOAR chooses.  Two more chains hold the rows in
 *			no partition: the NULL rows and cosine's zero vectors, and, in an
 *			index built with no partitions, every other row.
 *	REFINE	one item a row with a vector, and one a centroid: its 8-bit
 *			codes, which only the rows plane 1 cannot rule out are read for
 *
 * Inserts append to a partition's tail (and a copy to a second's),
 * serialized by the meta page's lock.  VACUUM removes CODE items, which
 * nothing points at, and then their REFINE items, which a scan finds through
 * the CODE item it copied and checks by TID.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <math.h>

#include "access/amapi.h"
#include "access/amvalidate.h"
#include "access/generic_xlog.h"
#include "access/htup_details.h"
#include "access/reloptions.h"
#include "access/tableam.h"
#include "access/tupdesc.h"
#include "access/xloginsert.h"
#include "catalog/index.h"
#include "catalog/pg_amop.h"
#include "catalog/pg_amproc.h"
#include "catalog/pg_opclass.h"
#include "catalog/pg_operator_d.h"
#include "catalog/pg_type_d.h"
#include "commands/defrem.h"
#include "commands/vacuum.h"
#include "common/pg_prng.h"
#include "executor/tuptable.h"
#include "miscadmin.h"
#include "optimizer/cost.h"
#include "optimizer/optimizer.h"
#include "storage/bufmgr.h"
#include "storage/predicate.h"
#include "utils/float.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/selfuncs.h"
#include "utils/spccache.h"
#include "utils/syscache.h"
#include "utils/tuplesort.h"

#include "lion_compat.h"
#include "lion_vec.h"

PG_FUNCTION_INFO_V1(lionvec_handler);

/* pg_lion.vector_epsilon, _batch, _margin and _patience (§44.8) */
double		lionvec_epsilon = 4.0;
int			lionvec_batch = 100;
double		lionvec_margin = LIONVEC_MARGIN_DEFAULT;
int			lionvec_patience = LIONVEC_PATIENCE_DEFAULT;

static relopt_kind lionvec_relopt_kind;

static void lionveccostestimate(PlannerInfo *root, IndexPath *path,
								double loop_count, Cost *indexStartupCost,
								Cost *indexTotalCost,
								Selectivity *indexSelectivity,
								double *indexCorrelation, double *indexPages);

typedef struct LionVecOptions
{
	int32		vl_len_;		/* varlena header (do not touch directly!) */
	int			partitions;		/* 0: by the rows' count (lionvec_partitions()) */
	bool		spill;			/* a copy of each row in a second partition */
} LionVecOptions;

/*
 * pgvector's `vector`, as it lays it out: a varlena of a 16-bit dimension
 * count, 16 unused bits and the floats.  pg_lion reads it here rather than
 * linking pgvector, so that pg_lion itself loads without pgvector.
 */
typedef struct LionVecPgVector
{
	int32		vl_len_;
	int16		dim;
	int16		unused;
	float		x[FLEXIBLE_ARRAY_MEMBER];
} LionVecPgVector;

StaticAssertDecl(LIONVEC_CODE_HDRSZ == 32,
				 "lionvec: the CODE item header is four words");
StaticAssertDecl(offsetof(LionVecRefine, codes) == 16,
				 "lionvec: the REFINE item header is two words");
StaticAssertDecl(LIONVEC_MAX_DIM >= 2000,
				 "lionvec: the page size takes the dimensions pgvector's own indexes do");

/* ----------------------------------------------------------------
 * Settings
 * ----------------------------------------------------------------
 */

void
lion_vec_init(void)
{
	lionvec_tables_init();
	lionvec_kernels_init();

	lionvec_relopt_kind = add_reloption_kind();

	DefineCustomRealVariable("pg_lion.vector_epsilon",
							 "Width of a lionvec scan's error bounds, in standard deviations.",
							 "A row whose bound leaves it out when it belongs is missed; "
							 "each bound fails with the Gaussian's probability of this many standard deviations, "
							 "and wider bounds make the scan read and compare more rows.",
							 &lionvec_epsilon,
							 4.0, 1.0, 10.0,
							 PGC_USERSET,
							 0,
							 NULL, NULL, NULL);

	DefineCustomIntVariable("pg_lion.vector_batch",
							"Rows a lionvec scan gathers candidates for in a pass over its partitions.",
							"A query that wants more rows than a pass gathered, or than work_mem holds, "
							"makes another, gathering four times as many.",
							&lionvec_batch,
							100, 1, 100000,
							PGC_USERSET,
							0,
							NULL, NULL, NULL);

	DefineCustomRealVariable("pg_lion.vector_margin",
							 "How much nearer than its centroid a lionvec scan takes a partition's rows "
							 "to come, in standard deviations of the rows about their centroids.",
							 "A scan reads the partitions in the order of their centroids' distance to the "
							 "query, and returns a row once the next centroid, less this margin, lies farther. "
							 "Larger reads more partitions and misses fewer of the nearest rows; a scan "
							 "filtered through lion takes less of it the fewer rows the filter leaves.",
							 &lionvec_margin,
							 LIONVEC_MARGIN_DEFAULT, -10.0, 1000.0,
							 PGC_USERSET,
							 0,
							 NULL, NULL, NULL);

	DefineCustomIntVariable("pg_lion.vector_patience",
							"How many partitions' worth of rows a lionvec scan scores without finding a "
							"nearer one before it stops probing.",
							"A scan that has scored this many partitions' worth of rows (of the members, "
							"under a lion filter) since one last entered its pg_lion.vector_batch nearest "
							"stops probing partitions until it has returned the rows it holds.  Smaller "
							"stops sooner and misses more of the nearest rows; 0 never stops.",
							&lionvec_patience,
							LIONVEC_PATIENCE_DEFAULT, 0, 1000000,
							PGC_USERSET,
							0,
							NULL, NULL, NULL);

	add_int_reloption(lionvec_relopt_kind, "partitions",
					  "Partitions a lionvec index's build makes; 0 chooses by the rows' count",
					  0, 0, LIONVEC_MAX_PARTS,
					  AccessExclusiveLock);

	add_bool_reloption(lionvec_relopt_kind, "spill",
					   "Whether a lionvec index's build stores each row in a second partition too",
					   true, AccessExclusiveLock);
}

bool
lion_vec_relation(Relation index)
{
	return index->rd_indam != NULL &&
		index->rd_indam->ambeginscan == lionvecbeginscan;
}

/* ----------------------------------------------------------------
 * Vectors
 * ----------------------------------------------------------------
 */

/*
 * The floats of a `vector` datum and their count; *tofree is the detoasted
 * copy to pfree, or NULL.
 */
const float *
lionvec_datum_floats(Datum d, int *dim, Pointer *tofree)
{
	struct varlena *raw = (struct varlena *) DatumGetPointer(d);
	struct varlena *v = pg_detoast_datum(raw);
	LionVecPgVector *vec = (LionVecPgVector *) v;
	Size		len = VARSIZE(v);

	*tofree = v != raw ? (Pointer) v : NULL;
	if (len < offsetof(LionVecPgVector, x) || vec->dim < 0 ||
		len < offsetof(LionVecPgVector, x) + sizeof(float) * (Size) vec->dim)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("lionvec was given a malformed vector of %zu bytes", len)));
	*dim = vec->dim;
	return vec->x;
}

/* pgvector's own error, so a query reads the same with or without the index */
void
lionvec_check_dims(const LionVecInfo *info, int dim)
{
	if (dim != info->dims)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_EXCEPTION),
				 errmsg("expected %d dimensions, not %d", info->dims, dim)));
}

static void
lionvec_check_max_dims(int dims)
{
	if (dims < 1)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_EXCEPTION),
				 errmsg("vector must have at least 1 dimension")));
	if (dims > LIONVEC_MAX_DIM)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("column cannot have more than %d dimensions for lionvec index",
						LIONVEC_MAX_DIM)));
}

/* The metric of the index's operator class: its one ORDER BY strategy. */
static int
lionvec_index_metric(Relation index)
{
	Oid			family = index->rd_opfamily[0];
	Oid			type = index->rd_opcintype[0];
	int			s;

	for (s = 1; s <= LIONVEC_NSTRATEGIES; s++)
		if (OidIsValid(get_opfamily_member(family, type, type, s)))
			return s;
	ereport(ERROR,
			(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
			 errmsg("operator family %u of lionvec index \"%s\" has no distance operator",
					family, RelationGetRelationName(index))));
	return 0;					/* keep the compiler quiet */
}

/* ----------------------------------------------------------------
 * Pages
 * ----------------------------------------------------------------
 */

static void
lionvec_init_page(Page page, uint16 type)
{
	LionVecPageOpaque op;

	PageInit(page, BLCKSZ, sizeof(LionVecPageOpaqueData));
	op = LionVecPageGetOpaque(page);
	op->next = InvalidBlockNumber;
	op->minkey = PG_UINT32_MAX;
	op->maxkey = 0;
	op->type = type;
	op->page_id = LIONVEC_PAGE_ID;
}

/* What a page of a chain must look like before anything on it is read. */
void
lionvec_check_page(Relation index, Page page, BlockNumber blk, uint16 type)
{
	LionVecPageOpaque op;

	if (PageIsNew(page) ||
		PageGetSpecialSize(page) != MAXALIGN(sizeof(LionVecPageOpaqueData)))
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg("lionvec index \"%s\" has an uninitialized or foreign page at block %u",
						RelationGetRelationName(index), blk)));
	op = LionVecPageGetOpaque(page);
	if (op->page_id != LIONVEC_PAGE_ID || op->type != type)
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg("lionvec index \"%s\" has a page of type %u where one of type %u belongs, at block %u",
						RelationGetRelationName(index), op->type, type, blk)));
}

/*
 * A CODE item, checked against the length a row of its kind has: NULL rows
 * and cosine zero vectors carry the header alone, every other row its plane.
 * copies says whether the chain may hold a row's copy (LIONVEC_SPILL): it is
 * a partition's, of an index that spills.  A copy is of a row with a vector.
 */
LionVecCode *
lionvec_code_at(Relation index, Page page, BlockNumber blk, OffsetNumber off,
				int dimsp, bool copies)
{
	ItemId		iid = PageGetItemId(page, off);
	LionVecCode *code;
	Size		len;

	if (!ItemIdIsNormal(iid) || ItemIdGetLength(iid) < LIONVEC_CODE_HDRSZ)
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg("lionvec index \"%s\" has a malformed item at (%u,%u)",
						RelationGetRelationName(index), blk, off)));
	code = (LionVecCode *) PageGetItem(page, iid);
	len = ItemIdGetLength(iid);
	if ((code->flags & ~(LIONVEC_NULL | LIONVEC_ZERO | LIONVEC_SPILL)) != 0 ||
		((code->flags & LIONVEC_SPILL) != 0 &&
		 (code->flags & (LIONVEC_NULL | LIONVEC_ZERO)) != 0) ||
		((code->flags & (LIONVEC_NULL | LIONVEC_ZERO)) != 0 ?
		 len != LIONVEC_CODE_HDRSZ :
		 (dimsp <= 0 || len != LIONVEC_CODE_SIZE(dimsp))))
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg("lionvec index \"%s\" has an item of %zu bytes and flags 0x%x at (%u,%u)",
						RelationGetRelationName(index), len, code->flags, blk, off)));
	if ((code->flags & LIONVEC_SPILL) != 0 && !copies)
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg("lionvec index \"%s\" has a row's copy where none belongs, at (%u,%u)",
						RelationGetRelationName(index), blk, off)));
	return code;
}

/*
 * The REFINE item of the row tid at (blk, off), or NULL when it is not there
 * any more: VACUUM removed it, and the slot may hold another row's since
 * (§44.5).  A row VACUUM removes is dead to every snapshot, so a scan that
 * finds it gone loses nothing.
 */
LionVecRefine *
lionvec_refine_at(Relation index, Page page, BlockNumber blk, OffsetNumber off,
				  int dimsp, ItemPointer tid)
{
	ItemId		iid;
	LionVecRefine *ref;

	if (off < FirstOffsetNumber || off > PageGetMaxOffsetNumber(page))
		return NULL;
	iid = PageGetItemId(page, off);
	if (!ItemIdIsNormal(iid))
		return NULL;
	if (ItemIdGetLength(iid) != LIONVEC_REFINE_SIZE(dimsp))
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg("lionvec index \"%s\" has a refine item of %u bytes at (%u,%u)",
						RelationGetRelationName(index), ItemIdGetLength(iid), blk, off)));
	ref = (LionVecRefine *) PageGetItem(page, iid);
	/* not ItemPointerEquals(), which asserts against a centroid's TID */
	return ItemPointerGetBlockNumberNoCheck(&ref->tid) == ItemPointerGetBlockNumberNoCheck(tid) &&
		ItemPointerGetOffsetNumberNoCheck(&ref->tid) == ItemPointerGetOffsetNumberNoCheck(tid) ?
		ref : NULL;
}

/*
 * A PART item, checked: its length, its centroid's (which always has a
 * vector), its chain's ends, and its numbers - a length or a spread that is
 * negative or not finite would make a bound whose lower end lies past its
 * upper.
 */
LionVecPart *
lionvec_part_at(Relation index, Page page, BlockNumber blk, OffsetNumber off,
				int dimsp)
{
	ItemId		iid = PageGetItemId(page, off);
	LionVecPart *part;

	if (dimsp <= 0 || !ItemIdIsNormal(iid) ||
		ItemIdGetLength(iid) != LIONVEC_PART_SIZE(dimsp))
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg("lionvec index \"%s\" has a malformed partition at (%u,%u)",
						RelationGetRelationName(index), blk, off)));
	part = (LionVecPart *) PageGetItem(page, iid);
	if (part->cent.flags != 0 || !BlockNumberIsValid(part->head) ||
		!BlockNumberIsValid(part->tail) || !BlockNumberIsValid(part->cent.rblk) ||
		!(part->cent.norm >= 0.0f) || isinf(part->cent.norm) ||
		isnan(part->cent.alpha1) || isinf(part->cent.alpha1) ||
		!(part->maxnorm >= 0.0f) || isinf(part->maxnorm) ||
		!(part->spread >= 0.0f) || isinf(part->spread))
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg("lionvec index \"%s\" has a malformed partition at (%u,%u)",
						RelationGetRelationName(index), blk, off)));
	return part;
}

static void
lionvec_check_meta(Relation index, Page page)
{
	LionVecMeta meta = LionVecPageGetMeta(page);

	lionvec_check_page(index, page, LIONVEC_METAPAGE, LIONVEC_PAGE_META);
	if (meta->magic != LIONVEC_MAGIC)
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg("index \"%s\" is not a lionvec index",
						RelationGetRelationName(index))));
	if (meta->version != LIONVEC_VERSION)
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg("lionvec index \"%s\" has format version %u, and this pg_lion reads version %d",
						RelationGetRelationName(index), meta->version, LIONVEC_VERSION),
				 errhint("REINDEX it.")));
	if (meta->metric < 1 || meta->metric > LIONVEC_NSTRATEGIES ||
		meta->dims < 0 || meta->dims > LIONVEC_MAX_DIM ||
		meta->dimsp != LIONVEC_PADDED(meta->dims) ||
		!BlockNumberIsValid(meta->rest_head) || !BlockNumberIsValid(meta->rest_tail) ||
		!BlockNumberIsValid(meta->null_head) || !BlockNumberIsValid(meta->null_tail) ||
		(meta->nparts > 0) != BlockNumberIsValid(meta->part_head) ||
		meta->nparts > LIONVEC_MAX_PARTS ||
		(meta->nparts > 0 && meta->dims == 0) ||
		(meta->flags & ~LIONVEC_META_SPILL) != 0 ||
		((meta->flags & LIONVEC_META_SPILL) != 0 && meta->nparts < 2))
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg("lionvec index \"%s\" has a malformed meta page",
						RelationGetRelationName(index))));
}

/* A copy of the meta page, read under a share lock. */
void
lionvec_read_meta(Relation index, LionVecMetaData *meta)
{
	Buffer		buf = ReadBuffer(index, LIONVEC_METAPAGE);
	Page		page;

	LockBuffer(buf, BUFFER_LOCK_SHARE);
	page = BufferGetPage(buf);
	lionvec_check_meta(index, page);
	*meta = *LionVecPageGetMeta(page);
	UnlockReleaseBuffer(buf);
}

static void
lionvec_init_meta(Page page, int metric, int dims)
{
	LionVecMeta meta;

	lionvec_init_page(page, LIONVEC_PAGE_META);
	meta = LionVecPageGetMeta(page);
	memset(meta, 0, sizeof(LionVecMetaData));
	meta->magic = LIONVEC_MAGIC;
	meta->version = LIONVEC_VERSION;
	meta->metric = (uint16) metric;
	meta->dims = dims;
	meta->dimsp = LIONVEC_PADDED(dims);
	meta->seed = LIONVEC_SEED;
	meta->mean_head = InvalidBlockNumber;
	meta->part_head = InvalidBlockNumber;
	meta->nparts = 0;
	meta->rest_head = InvalidBlockNumber;
	meta->rest_tail = InvalidBlockNumber;
	meta->null_head = InvalidBlockNumber;
	meta->null_tail = InvalidBlockNumber;
	meta->refine_tail = InvalidBlockNumber;
	((PageHeader) page)->pd_lower =
		((char *) meta + sizeof(LionVecMetaData)) - (char *) page;
}

/* floats a MEAN page holds */
#define LIONVEC_MEAN_PER_PAGE \
	((int) ((BLCKSZ - MAXALIGN(SizeOfPageHeaderData) - \
			 MAXALIGN(sizeof(LionVecPageOpaqueData))) / sizeof(float)))

/* ----------------------------------------------------------------
 * The per-index information a backend keeps (rd_amcache)
 * ----------------------------------------------------------------
 */

static Size
lionvec_info_size(int dimsp)
{
	Size		size = MAXALIGN(sizeof(LionVecInfo));

	if (dimsp > 0)
		size += sizeof(float) * (lionvec_rotation_nsigns(dimsp) + dimsp);
	return size;
}

/* Point a copy's arrays at its own trailing storage. */
static void
lionvec_info_fix(LionVecInfo *info, bool hasmean)
{
	char	   *tail = (char *) info + MAXALIGN(sizeof(LionVecInfo));

	info->rot.signs = NULL;
	info->mean = NULL;
	if (info->dims > 0)
	{
		info->rot.signs = (float *) tail;
		if (hasmean)
			info->mean = (float *) (tail + sizeof(float) *
									lionvec_rotation_nsigns(info->dimsp));
	}
}

static void
lionvec_read_mean(Relation index, BlockNumber blk, float *mean, int dimsp)
{
	int			done = 0;
	uint64		visited = 0;
	BlockNumber nblocks = RelationGetNumberOfBlocks(index);

	while (done < dimsp)
	{
		Buffer		buf;
		Page		page;
		int			n = Min(dimsp - done, LIONVEC_MEAN_PER_PAGE);

		if (!BlockNumberIsValid(blk))
			ereport(ERROR,
					(errcode(ERRCODE_INDEX_CORRUPTED),
					 errmsg("lionvec index \"%s\" has a short mean",
							RelationGetRelationName(index))));
		lionvec_chain_step(index, &visited, &nblocks);
		buf = ReadBuffer(index, blk);
		LockBuffer(buf, BUFFER_LOCK_SHARE);
		page = BufferGetPage(buf);
		lionvec_check_page(index, page, blk, LIONVEC_PAGE_MEAN);
		if (((PageHeader) page)->pd_lower !=
			MAXALIGN(SizeOfPageHeaderData) + sizeof(float) * n)
			ereport(ERROR,
					(errcode(ERRCODE_INDEX_CORRUPTED),
					 errmsg("lionvec index \"%s\" has a malformed mean page at block %u",
							RelationGetRelationName(index), blk)));
		memcpy(mean + done, PageGetContents(page), sizeof(float) * n);
		done += n;
		blk = LionVecPageGetOpaque(page)->next;
		UnlockReleaseBuffer(buf);
	}
}

/*
 * The rotation and mean of the index, as a palloc'd copy the caller owns.
 * The backend keeps one in rd_amcache, made again when the meta page says
 * something else; a copy is returned rather than that one because a relcache
 * invalidation, which can arrive whenever a lock is taken (a TOAST fetch's
 * included), frees it.
 */
LionVecInfo *
lionvec_get_info(Relation index)
{
	LionVecMetaData meta;
	LionVecInfo *cached = (LionVecInfo *) index->rd_amcache;
	LionVecInfo *copy;
	Size		size;

	lionvec_read_meta(index, &meta);
	if (cached == NULL || cached->dims != meta.dims ||
		cached->metric != meta.metric || cached->seed != meta.seed)
	{
		if (cached != NULL)
		{
			index->rd_amcache = NULL;
			pfree(cached);
		}
		size = lionvec_info_size(meta.dimsp);
		cached = MemoryContextAllocZero(index->rd_indexcxt, size);
		cached->metric = meta.metric;
		cached->dims = meta.dims;
		cached->dimsp = meta.dimsp;
		cached->seed = meta.seed;
		lionvec_info_fix(cached, BlockNumberIsValid(meta.mean_head));
		if (cached->dims > 0)
		{
			lionvec_rotation_init(&cached->rot, cached->dimsp, cached->seed,
								  cached->rot.signs);
			if (cached->mean != NULL)
				lionvec_read_mean(index, meta.mean_head, cached->mean,
								  cached->dimsp);
		}
		index->rd_amcache = cached;
	}

	size = lionvec_info_size(cached->dimsp);
	copy = palloc(size);
	memcpy(copy, cached, size);
	lionvec_info_fix(copy, cached->mean != NULL);
	return copy;
}

/*
 * The first vector an index built without a dimension count is given fixes
 * it (§44.2): under the meta page's lock, so that two first inserts agree.
 */
static void
lionvec_set_dims(Relation index, int dims)
{
	Buffer		buf;
	Page		page;
	LionVecMeta meta;

	lionvec_check_max_dims(dims);
	buf = ReadBuffer(index, LIONVEC_METAPAGE);
	LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
	page = BufferGetPage(buf);
	lionvec_check_meta(index, page);
	if (LionVecPageGetMeta(page)->dims == 0 && dims > 0)
	{
		GenericXLogState *state = GenericXLogStart(index);

		page = GenericXLogRegisterBuffer(state, buf, 0);
		meta = LionVecPageGetMeta(page);
		meta->dims = dims;
		meta->dimsp = LIONVEC_PADDED(dims);
		GenericXLogFinish(state);
	}
	UnlockReleaseBuffer(buf);
}

/* ----------------------------------------------------------------
 * Encoding a row into its items
 * ----------------------------------------------------------------
 */

typedef struct LionVecItems
{
	LionVecCode *code;			/* LIONVEC_CODE_SIZE(dimsp) bytes */
	Size		codesize;
	LionVecRefine *ref;			/* LIONVEC_REFINE_SIZE(dimsp) bytes */
	bool		hasref;
	float	   *x;				/* [dimsp] */
} LionVecItems;

static void
lionvec_items_alloc(LionVecItems *it, int dimsp)
{
	it->code = palloc0(LIONVEC_CODE_SIZE(Max(dimsp, 64)));
	it->ref = palloc0(LIONVEC_REFINE_SIZE(Max(dimsp, 64)));
	it->x = palloc(sizeof(float) * Max(dimsp, 64));
}

/*
 * Fill it with the items of the row tid: value its datum, or isnull.  The
 * caller has checked value's dimensions against info.
 */
static void
lionvec_items_fill(const LionVecInfo *info, LionVecItems *it, ItemPointer tid,
				   Datum value, bool isnull)
{
	LionVecCode *code = it->code;
	LionVecEncoded e;

	memset(code, 0, LIONVEC_CODE_HDRSZ);
	code->tid = *tid;
	code->rblk = InvalidBlockNumber;
	code->roff = InvalidOffsetNumber;
	it->codesize = LIONVEC_CODE_HDRSZ;
	it->hasref = false;

	if (isnull)
	{
		code->flags = LIONVEC_NULL;
		return;
	}
	else
	{
		Pointer		tofree;
		int			dim;
		const float *v = lionvec_datum_floats(value, &dim, &tofree);
		bool		nonzero;

		lionvec_check_dims(info, dim);
		if (info->dims == 0)
			elog(ERROR, "lionvec: no dimensions to encode a vector in");
		nonzero = lionvec_prepare(&info->rot, info->metric, v, dim, it->x);
		if (tofree != NULL)
			pfree(tofree);
		if (!nonzero)
		{
			code->flags = LIONVEC_ZERO;
			return;
		}
	}

	e.bits = code->bits;
	e.codes = it->ref->codes;
	lionvec_encode(info->mean, info->dimsp, it->x, &e);
	code->norm = e.norm;
	code->alpha1 = e.alpha1;
	code->xm = e.xm;
	code->popcount = e.popcount;
	it->codesize = LIONVEC_CODE_SIZE(info->dimsp);

	it->ref->tid = *tid;
	it->ref->pad = 0;
	it->ref->alpha8 = e.alpha8;
	it->ref->rnorm8 = e.rnorm8;
	it->hasref = true;
}

static void
lionvec_note_key(Page page, ItemPointer tid)
{
	LionVecPageOpaque op = LionVecPageGetOpaque(page);
	uint32		ckey = lionvec_ckey(tid);

	op->minkey = Min(op->minkey, ckey);
	op->maxkey = Max(op->maxkey, ckey);
}

/* ----------------------------------------------------------------
 * Build
 * ----------------------------------------------------------------
 */

/* k-means' iterations, which stop sooner once no row moves */
#define LIONVEC_KMEANS_ITERS	10

/* the most rows k-means is given, whatever maintenance_work_mem holds */
#define LIONVEC_SAMPLE_MAX		(1 << 20)

/*
 * The fewest sample rows a partition the build chooses by itself, and the
 * fewest rows a table must have for it to make more than one.
 */
#define LIONVEC_SAMPLE_PER_PART	16
#define LIONVEC_MIN_AUTO_ROWS	10000

/*
 * The CODE items go through a sort by partition, then heap order: a key of
 * the group (a partition, then the rows in no partition, then the NULL rows
 * and zeros) above the row's place in the heap scan.  A row's copy goes in
 * at the same place in its second partition's group, so that the sort holds
 * two items a row of an index that spills.
 */
#define LIONVEC_SEQ_BITS		40
#define LIONVEC_SORT_KEY(group, seq) \
	((int64) (((uint64) (group) << LIONVEC_SEQ_BITS) | (uint64) (seq)))
#define LIONVEC_SORT_GROUP(key) ((uint32) ((uint64) (key) >> LIONVEC_SEQ_BITS))

StaticAssertDecl(LIONVEC_MAX_PARTS + 2 < (1 << (63 - LIONVEC_SEQ_BITS)),
				 "lionvec: a partition's number fits the sort key");

typedef struct LionVecBuildState
{
	Relation	index;
	int			metric;
	int			dims;
	/* the first pass: the mean, and a sample (lionvec_build_sample()) */
	double	   *sum;			/* [dims] */
	double		nsum;			/* rows with a vector (cosine: a nonzero one) */
	LionVecRotation srot;
	uint8	   *scodes;			/* [smax][dimsp] */
	float	   *sscale;			/* [smax] */
	int			smax;
	int			sn;
	pg_prng_state sprng;
	/* the second: the items */
	LionVecInfo *info;
	LionVecItems items;
	int			nparts;
	bool		spill;			/* a copy of each row in a second partition */
	float	   *cent;			/* spilling: [nparts][dimsp], the centroids, centred */
	double	   *ckey;			/* [nparts]: the candidates' keys */
	int			crank[LIONVEC_SOAR_CANDS + 1];	/* ... the least of them */
	const float *csoar[LIONVEC_SOAR_CANDS]; /* ... and SOAR's candidates */
	LionVecCentroids cents;		/* the centroids' plane 1 ... */
	uint64	   *cbits;			/* [nparts][dimsp / 64] */
	uint16	   *cpop;			/* [nparts] */
	float	   *cnorm;			/* [nparts] */
	float	   *calpha1;		/* [nparts] */
	float	   *cxm;			/* [nparts] */
	uint8	   *ccodes;			/* [nparts][dimsp]: ... their 8-bit codes */
	float	   *calpha8;		/* [nparts] */
	float	   *crnorm8;		/* [nparts] */
	BlockNumber *crblk;			/* [nparts]: ... and their REFINE items */
	OffsetNumber *croff;		/* [nparts] */
	uint32	   *pcount;			/* [nparts]: the items put in each, copies too */
	float	   *pmaxnorm;		/* [nparts]: the largest |x + m| of their rows */
	double	   *psumsq;			/* [nparts]: their sum of |x - c|^2 */
	double		mean2;			/* |m|^2, m the mean */
	double		sumsq;			/* sum of |x - c|^2, x a row and c its nearest */
	double		nsq;			/* ... over this many rows */
	LionVecQuery q;				/* a row, as its nearest centroid is found by */
	double	   *lo;				/* [nparts] */
	double	   *hi;				/* [nparts] */
	int		   *cand;			/* [nparts] */
	Buffer		rbuf;			/* the REFINE page being filled */
	BlockNumber refine_tail;
	Tuplesortstate *sort;		/* the CODE items, by LIONVEC_SORT_KEY() */
	TupleTableSlot *inslot;
	TupleTableSlot *outslot;
	text	   *sortitem;		/* [LIONVEC_CODE_SIZE(dimsp) + VARHDRSZ] */
	double		indtuples;
	MemoryContext buildcxt;		/* what lasts the build */
	MemoryContext tmpcxt;		/* reset after each row */
} LionVecBuildState;

static Buffer
lionvec_new_buffer(Relation index, ForkNumber fork)
{
	return ExtendBufferedRel(BMR_REL(index), fork, NULL, EB_LOCK_FIRST);
}

/*
 * The first pass's sample (DESIGN.md §44.2): as many rows as half of
 * maintenance_work_mem holds as 8-bit codes, by reservoir sampling (Vitter's
 * algorithm R) - the n-th row with a vector takes a place when it is one of
 * the first smax, and otherwise with probability smax / n, in place of one
 * chosen at random - from a generator seeded alike every build, so that a
 * build of the same rows makes the same partitions.  The rows are rotated
 * but not centred, the mean not being known yet; the codes are within a
 * percent of the rows, which k-means does not notice.
 */
static void
lionvec_build_sample(LionVecBuildState *bs, const float *v, int dim)
{
	int			dimsp = LIONVEC_PADDED(dim);
	int			slot;
	float	   *x;
	LionVecEncoded e;

	if (bs->scodes == NULL)
	{
		Size		budget = (Size) maintenance_work_mem * 1024 / 2;
		Size		per = (Size) dimsp + sizeof(float);
		MemoryContext old = MemoryContextSwitchTo(bs->buildcxt);

		bs->smax = (int) Max(Min(budget / per, (Size) LIONVEC_SAMPLE_MAX), 1);
		bs->scodes = MemoryContextAllocHuge(bs->buildcxt, (Size) bs->smax * dimsp);
		bs->sscale = palloc(sizeof(float) * bs->smax);
		bs->srot.signs = palloc(sizeof(float) * lionvec_rotation_nsigns(dimsp));
		lionvec_rotation_init(&bs->srot, dimsp, LIONVEC_SEED, bs->srot.signs);
		pg_prng_seed(&bs->sprng, (uint64) LIONVEC_SEED);
		MemoryContextSwitchTo(old);
	}

	if (bs->sn < bs->smax)
		slot = bs->sn++;
	else
	{
		/* this row is row nsum, from 1 */
		uint64		r = pg_prng_uint64_range(&bs->sprng, 0, (uint64) bs->nsum - 1);

		if (r >= (uint64) bs->smax)
			return;
		slot = (int) r;
	}

	x = palloc(sizeof(float) * dimsp);
	if (!lionvec_prepare(&bs->srot, bs->metric, v, dim, x))
		elog(ERROR, "lionvec: a zero vector in the sample");
	e.bits = palloc(sizeof(uint64) * (dimsp / 64));
	e.codes = bs->scodes + (Size) slot * dimsp;
	lionvec_encode(NULL, dimsp, x, &e);
	bs->sscale[slot] = e.rnorm8 > 0.0f ? e.norm / e.rnorm8 : 0.0f;
}

static void
lionvec_build_mean_callback(Relation index, ItemPointer tid, Datum *values,
							bool *isnull, bool tupleIsAlive, void *state)
{
	LionVecBuildState *bs = (LionVecBuildState *) state;
	MemoryContext old;
	Pointer		tofree;
	const float *v;
	int			dim;
	int			i;
	double		scale = 1.0;

	if (isnull[0])
		return;
	old = MemoryContextSwitchTo(bs->tmpcxt);
	v = lionvec_datum_floats(values[0], &dim, &tofree);
	if (bs->dims == 0)
	{
		lionvec_check_max_dims(dim);
		bs->dims = dim;
	}
	if (dim != bs->dims)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_EXCEPTION),
				 errmsg("expected %d dimensions, not %d", bs->dims, dim)));
	if (bs->sum == NULL && dim > 0)
		bs->sum = MemoryContextAllocZero(bs->buildcxt, sizeof(double) * dim);
	if (bs->metric == LIONVEC_COSINE)
	{
		double		n2 = 0.0;

		for (i = 0; i < dim; i++)
			n2 += (double) v[i] * (double) v[i];
		scale = n2 > 0.0 ? 1.0 / sqrt(n2) : 0.0;
	}
	if (scale > 0.0 && dim > 0)
	{
		for (i = 0; i < dim; i++)
			bs->sum[i] += v[i] * scale;
		bs->nsum += 1.0;
		lionvec_build_sample(bs, v, dim);
	}
	MemoryContextSwitchTo(old);
	MemoryContextReset(bs->tmpcxt);
}

/*
 * How many partitions the build makes of nrows rows with a vector, sampled
 * sampled: as the index's `partitions` option asks, or else one for a table
 * of fewer than LIONVEC_MIN_AUTO_ROWS of them and twice their square root
 * for a larger (2,000 for a million rows), no more than the sample has
 * LIONVEC_SAMPLE_PER_PART rows for.  Never more than the sample's rows, and
 * none without one.
 */
static int
lionvec_partitions(Relation index, double nrows, int sampled)
{
	LionVecOptions *opts = (LionVecOptions *) index->rd_options;
	double		want;

	if (nrows < 1.0 || sampled < 1)
		return 0;
	if (opts != NULL && opts->partitions > 0)
		want = opts->partitions;
	else if (nrows < LIONVEC_MIN_AUTO_ROWS)
		want = 1.0;
	else
		want = Min(2.0 * sqrt(nrows),
				   Max(1.0, floor((double) sampled / LIONVEC_SAMPLE_PER_PART)));
	want = Min(want, (double) sampled);
	return (int) Max(1.0, Min(want, (double) LIONVEC_MAX_PARTS));
}

/* Put a REFINE item on the page being filled, or a new one. */
static void
lionvec_build_put_refine(LionVecBuildState *bs, LionVecRefine *ref, Size size,
						 BlockNumber *blk, OffsetNumber *off)
{
	Page		page;

	if (BufferIsValid(bs->rbuf) &&
		PageGetFreeSpace(BufferGetPage(bs->rbuf)) < MAXALIGN(size))
	{
		MarkBufferDirty(bs->rbuf);
		UnlockReleaseBuffer(bs->rbuf);
		bs->rbuf = InvalidBuffer;
	}
	if (!BufferIsValid(bs->rbuf))
	{
		bs->rbuf = lionvec_new_buffer(bs->index, MAIN_FORKNUM);
		lionvec_init_page(BufferGetPage(bs->rbuf), LIONVEC_PAGE_REFINE);
		bs->refine_tail = BufferGetBlockNumber(bs->rbuf);
	}
	page = BufferGetPage(bs->rbuf);
	*off = PageAddItemExtended(page, (char *) ref, size, InvalidOffsetNumber, 0);
	if (*off == InvalidOffsetNumber)
		elog(ERROR, "lionvec: could not add a refine item of %zu bytes", size);
	*blk = bs->refine_tail;
}

/*
 * The partitions: k-means over the sample, in the rotated space (which
 * changes no distance), spherical for cosine, whose rows are of unit length;
 * then each centroid coded as a row is, its 8-bit codes in a REFINE item
 * and its plane 1 kept for the PART chain and for finding each row's
 * nearest (lionvec_build_nearest()).  An index that spills keeps the
 * centroids' floats too, centred as the rows are, for SOAR's choice of a
 * row's second partition.  The sample is freed.
 */
static void
lionvec_build_centroids(LionVecBuildState *bs)
{
	int			k = bs->nparts;
	int			dimsp = bs->info->dimsp;
	int			nw = dimsp / 64;
	LionVecSample sample;
	LionVecKmeansWork w;
	float	   *cent;
	LionVecRefine *ref;
	Size		rsize = LIONVEC_REFINE_SIZE(dimsp);
	int			made;
	int			j;

	Assert(k >= 1 && k <= bs->sn);
	sample.n = bs->sn;
	sample.dimsp = dimsp;
	sample.codes = bs->scodes;
	sample.scale = bs->sscale;

	cent = MemoryContextAllocHuge(CurrentMemoryContext, sizeof(float) * (Size) k * dimsp);
	w.count = palloc(sizeof(int) * k);
	w.assign = palloc(sizeof(int) * bs->sn);
	w.bits = MemoryContextAllocHuge(CurrentMemoryContext, sizeof(uint64) * (Size) k * nw);
	w.popcount = palloc(sizeof(uint16) * k);
	w.norm = palloc(sizeof(float) * k);
	w.alpha1 = palloc(sizeof(float) * k);
	w.lo = palloc(sizeof(double) * k);
	w.hi = palloc(sizeof(double) * k);
	w.cand = palloc(sizeof(int) * k);
	w.x = palloc(sizeof(float) * dimsp);
	w.y = palloc(sizeof(float) * dimsp);
	w.codes = palloc(dimsp);
	w.qc = palloc(sizeof(float) * dimsp);
	w.planes = palloc(sizeof(uint64) * nw * LIONVEC_QBITS);

	made = lionvec_kmeans(&sample, k, LIONVEC_KMEANS_ITERS,
						  bs->metric == LIONVEC_COSINE, (uint64) LIONVEC_SEED,
						  bs->info->mean, cent, &w);
	elog(DEBUG1, "lionvec index \"%s\": %d partitions from a sample of %d rows, %d k-means iterations",
		 RelationGetRelationName(bs->index), k, bs->sn, made);

	pfree(w.count);
	pfree(w.assign);
	pfree(w.bits);
	pfree(w.popcount);
	pfree(w.norm);
	pfree(w.alpha1);
	pfree(w.lo);
	pfree(w.hi);
	pfree(w.cand);
	pfree(w.y);
	pfree(w.codes);
	pfree(w.qc);
	pfree(w.planes);
	pfree(bs->scodes);
	pfree(bs->sscale);
	bs->scodes = NULL;
	bs->sscale = NULL;

	bs->cbits = MemoryContextAllocHuge(CurrentMemoryContext, sizeof(uint64) * (Size) k * nw);
	bs->cpop = palloc(sizeof(uint16) * k);
	bs->cnorm = palloc(sizeof(float) * k);
	bs->calpha1 = palloc(sizeof(float) * k);
	bs->cxm = palloc(sizeof(float) * k);
	bs->ccodes = MemoryContextAllocHuge(CurrentMemoryContext, (Size) k * dimsp);
	bs->calpha8 = palloc(sizeof(float) * k);
	bs->crnorm8 = palloc(sizeof(float) * k);
	bs->crblk = palloc(sizeof(BlockNumber) * k);
	bs->croff = palloc(sizeof(OffsetNumber) * k);
	bs->pcount = palloc0(sizeof(uint32) * k);
	bs->pmaxnorm = palloc0(sizeof(float) * k);
	bs->psumsq = palloc0(sizeof(double) * k);
	ref = palloc0(rsize);
	for (j = 0; j < k; j++)
	{
		LionVecEncoded e;

		memcpy(w.x, cent + (Size) j * dimsp, sizeof(float) * dimsp);
		e.bits = bs->cbits + (Size) j * nw;
		e.codes = bs->ccodes + (Size) j * dimsp;
		lionvec_encode(bs->info->mean, dimsp, w.x, &e);
		if (bs->spill)
			memcpy(cent + (Size) j * dimsp, w.x, sizeof(float) * dimsp);	/* centred */
		bs->cpop[j] = e.popcount;
		bs->cnorm[j] = e.norm;
		bs->calpha1[j] = e.alpha1;
		bs->cxm[j] = e.xm;
		bs->calpha8[j] = e.alpha8;
		bs->crnorm8[j] = e.rnorm8;

		LionVecCentTid(&ref->tid, j);
		ref->alpha8 = e.alpha8;
		ref->rnorm8 = e.rnorm8;
		memcpy(ref->codes, e.codes, dimsp);
		lionvec_build_put_refine(bs, ref, rsize, &bs->crblk[j], &bs->croff[j]);
	}
	pfree(ref);
	pfree(w.x);
	if (bs->spill)
		bs->cent = cent;
	else
		pfree(cent);

	bs->cents.k = k;
	bs->cents.dimsp = dimsp;
	bs->cents.bits = bs->cbits;
	bs->cents.popcount = bs->cpop;
	bs->cents.norm = bs->cnorm;
	bs->cents.alpha1 = bs->calpha1;
	bs->q.dimsp = dimsp;
	bs->q.qc = palloc(sizeof(float) * dimsp);
	bs->q.planes = palloc(sizeof(uint64) * nw * LIONVEC_QBITS);
	bs->lo = palloc(sizeof(double) * k);
	bs->hi = palloc(sizeof(double) * k);
	bs->cand = palloc(sizeof(int) * k);
	bs->ckey = palloc(sizeof(double) * k);
}

/*
 * The partition of the row whose centred vector is xc[dimsp], of norm
 * xnorm: the centroid nearest it by L2, among the candidates plane 1 leaves
 * (lionvec_candidates()) by their 8-bit codes, whose error is a seventieth
 * of plane 1's.  *dist2 is the row's squared distance from it.
 *
 * An index that spills takes the candidates that can be among the
 * LIONVEC_SOAR_CANDS + 1 nearest instead, ranks them by their codes, and
 * gives the nearest's partition and, in *second, SOAR's choice among the
 * next (lionvec_soar_second(), DESIGN.md §44.3), by the centroids' floats;
 * *dist2b is the row's squared distance from that one.  Otherwise *second
 * is -1.
 */
static int
lionvec_build_nearest(LionVecBuildState *bs, const float *xc, float xnorm,
					  double *dist2, int *second, double *dist2b)
{
	int			dimsp = bs->info->dimsp;
	int			m = bs->spill ? LIONVEC_SOAR_CANDS + 1 : 1;
	int			nc;
	int			nr;
	int			best;
	int			c;

	*second = -1;
	*dist2b = 0.0;
	lionvec_query_init(&bs->q, LIONVEC_L2, NULL, xc, LIONVEC_ASSIGN_EPS);
	nc = lionvec_candidates_n(&bs->cents, &bs->q, m, bs->lo, bs->hi, bs->cand);
	for (c = 0; c < nc; c++)
	{
		int			j = bs->cand[c];
		double		est;
		double		err;

		lionvec_estimate8(&bs->q, bs->ccodes + (Size) j * dimsp, bs->cnorm[j],
						  bs->calpha8[j], bs->crnorm8[j], &est, &err);
		bs->ckey[c] = (double) bs->cnorm[j] * bs->cnorm[j] - 2.0 * est;
	}
	nr = lionvec_rank(bs->ckey, nc, m, bs->crank);

	/*
	 * The build's own centroids, every number finite, leave min(k, m)
	 * candidates or more (lion_vec_math.h), so two or more when spilling,
	 * which takes two partitions or more.
	 */
	if (nr < Min(bs->cents.k, m))
		elog(ERROR, "lionvec: %d of %d centroids near a row, fewer than %d",
			 nr, bs->cents.k, Min(bs->cents.k, m));
	best = bs->cand[bs->crank[0]];
	*dist2 = Max((double) xnorm * xnorm + bs->ckey[bs->crank[0]], 0.0);

	if (bs->spill)
	{
		int			s;
		int			i;

		for (i = 1; i < nr; i++)
			bs->csoar[i - 1] = bs->cent + (Size) bs->cand[bs->crank[i]] * dimsp;
		s = lionvec_soar_second(xc, bs->cent + (Size) best * dimsp, bs->csoar,
								nr - 1, dimsp, LIONVEC_SOAR_LAMBDA);
		Assert(s >= 0 && s < nr - 1);
		*second = bs->cand[bs->crank[s + 1]];
		*dist2b = Max((double) xnorm * xnorm + bs->ckey[bs->crank[s + 1]], 0.0);
	}
	return best;
}

/* An item into the sort, in group's chain, at the row's place in the heap. */
static void
lionvec_build_put_code(LionVecBuildState *bs, uint32 group, const LionVecCode *code,
					   Size size)
{
	SET_VARSIZE(bs->sortitem, VARHDRSZ + size);
	memcpy(VARDATA(bs->sortitem), code, size);
	ExecClearTuple(bs->inslot);
	bs->inslot->tts_values[0] = Int64GetDatum(LIONVEC_SORT_KEY(group, (uint64) bs->indtuples));
	bs->inslot->tts_isnull[0] = false;
	bs->inslot->tts_values[1] = PointerGetDatum(bs->sortitem);
	bs->inslot->tts_isnull[1] = false;
	ExecStoreVirtualTuple(bs->inslot);
	tuplesort_puttupleslot(bs->sort, bs->inslot);
}

static void
lionvec_build_callback(Relation index, ItemPointer tid, Datum *values,
					   bool *isnull, bool tupleIsAlive, void *state)
{
	LionVecBuildState *bs = (LionVecBuildState *) state;
	LionVecItems *it = &bs->items;
	MemoryContext old = MemoryContextSwitchTo(bs->tmpcxt);
	uint32		group;
	int			second = -1;

	lionvec_items_fill(bs->info, it, tid, values[0], isnull[0]);
	if (!it->hasref)
		group = bs->nparts + 1;		/* a NULL row or a zero */
	else if (bs->nparts == 0)
		group = 0;				/* the rows in no partition */
	else
	{
		double		dist2;
		double		dist2b;
		double		norm = it->code->norm;
		float		raw;

		group = lionvec_build_nearest(bs, it->x, it->code->norm, &dist2,
									  &second, &dist2b);
		/* |x + m|, the row's own norm (a cosine row's 1) */
		raw = (float) sqrt(Max(norm * norm + 2.0 * it->code->xm + bs->mean2, 0.0));
		bs->pcount[group]++;
		bs->psumsq[group] += dist2;
		bs->pmaxnorm[group] = Max(bs->pmaxnorm[group], raw);
		/* the scale (the margin's unit) is of rows about their nearest */
		bs->sumsq += dist2;
		bs->nsq += 1.0;
		if (second >= 0)
		{
			bs->pcount[second]++;
			bs->psumsq[second] += dist2b;
			bs->pmaxnorm[second] = Max(bs->pmaxnorm[second], raw);
		}
	}
	if (it->hasref)
		lionvec_build_put_refine(bs, it->ref, LIONVEC_REFINE_SIZE(bs->info->dimsp),
								 &it->code->rblk, &it->code->roff);

	if (bs->indtuples >= (double) (UINT64CONST(1) << LIONVEC_SEQ_BITS))
		elog(ERROR, "lionvec: too many rows to build an index of");
	lionvec_build_put_code(bs, group, it->code, it->codesize);
	if (second >= 0)
	{
		/* the copy: the same item, pointing at the same REFINE item */
		it->code->flags |= LIONVEC_SPILL;
		lionvec_build_put_code(bs, (uint32) second, it->code, it->codesize);
		it->code->flags &= ~LIONVEC_SPILL;
	}
	bs->indtuples += 1;

	MemoryContextSwitchTo(old);
	MemoryContextReset(bs->tmpcxt);
}

/* Write the mean, rotated, as a chain of MEAN pages; returns its head. */
static BlockNumber
lionvec_build_write_mean(Relation index, const float *mean, int dimsp)
{
	BlockNumber head = InvalidBlockNumber;
	Buffer		prev = InvalidBuffer;
	int			done = 0;

	while (done < dimsp)
	{
		Buffer		buf = lionvec_new_buffer(index, MAIN_FORKNUM);
		Page		page = BufferGetPage(buf);
		int			n = Min(dimsp - done, LIONVEC_MEAN_PER_PAGE);

		lionvec_init_page(page, LIONVEC_PAGE_MEAN);
		memcpy(PageGetContents(page), mean + done, sizeof(float) * n);
		((PageHeader) page)->pd_lower = MAXALIGN(SizeOfPageHeaderData) + sizeof(float) * n;
		done += n;
		if (BufferIsValid(prev))
		{
			LionVecPageGetOpaque(BufferGetPage(prev))->next = BufferGetBlockNumber(buf);
			MarkBufferDirty(prev);
			UnlockReleaseBuffer(prev);
		}
		else
			head = BufferGetBlockNumber(buf);
		prev = buf;
	}
	if (BufferIsValid(prev))
	{
		MarkBufferDirty(prev);
		UnlockReleaseBuffer(prev);
	}
	return head;
}

/*
 * The CODE chains, from the sort: each group's items in heap order, on
 * pages of its own, the groups one after another in one run of blocks -
 * every partition, even an empty one, at least a page, for inserts to
 * append to; then the chain of rows in no partition, then that of the NULL
 * rows and zeros.  heads[] and tails[] get each group's ends, ngroups =
 * nparts + 2 of them.
 */
static void
lionvec_build_write_codes(LionVecBuildState *bs, BlockNumber *heads,
						  BlockNumber *tails)
{
	Relation	index = bs->index;
	uint32		ngroups = (uint32) bs->nparts + 2;
	uint32		group = 0;
	Buffer		buf = InvalidBuffer;
	Page		page = NULL;
	char	   *item = palloc(LIONVEC_CODE_SIZE(Max(bs->info->dimsp, 64)));
	bool		more;

	tuplesort_performsort(bs->sort);
	more = tuplesort_gettupleslot(bs->sort, true, false, bs->outslot, NULL);
	for (group = 0; group < ngroups; group++)
	{
		/* the group's first page */
		Buffer		next = lionvec_new_buffer(index, MAIN_FORKNUM);

		if (BufferIsValid(buf))
		{
			MarkBufferDirty(buf);
			UnlockReleaseBuffer(buf);
		}
		buf = next;
		page = BufferGetPage(buf);
		lionvec_init_page(page, LIONVEC_PAGE_CODE);
		heads[group] = tails[group] = BufferGetBlockNumber(buf);

		while (more)
		{
			bool		isnull;
			int64		key = DatumGetInt64(slot_getattr(bs->outslot, 1, &isnull));
			Datum		d;
			Size		len;

			if (LIONVEC_SORT_GROUP(key) < group)
				elog(ERROR, "lionvec: the build's sort is out of order");
			if (LIONVEC_SORT_GROUP(key) > group)
				break;
			CHECK_FOR_INTERRUPTS();
			d = slot_getattr(bs->outslot, 2, &isnull);
			len = VARSIZE_ANY_EXHDR(DatumGetPointer(d));
			if (len < LIONVEC_CODE_HDRSZ || len > LIONVEC_CODE_SIZE(Max(bs->info->dimsp, 64)))
				elog(ERROR, "lionvec: the build's sort has an item of %zu bytes", len);
			/* aligned, as the TID is read from it */
			memcpy(item, VARDATA_ANY(DatumGetPointer(d)), len);

			if (PageGetFreeSpace(page) < MAXALIGN(len))
			{
				next = lionvec_new_buffer(index, MAIN_FORKNUM);
				LionVecPageGetOpaque(page)->next = BufferGetBlockNumber(next);
				MarkBufferDirty(buf);
				UnlockReleaseBuffer(buf);
				buf = next;
				page = BufferGetPage(buf);
				lionvec_init_page(page, LIONVEC_PAGE_CODE);
				tails[group] = BufferGetBlockNumber(buf);
			}
			if (PageAddItemExtended(page, item, len, InvalidOffsetNumber, 0) ==
				InvalidOffsetNumber)
				elog(ERROR, "lionvec: could not add an item of %zu bytes", len);
			lionvec_note_key(page, &((LionVecCode *) item)->tid);
			more = tuplesort_gettupleslot(bs->sort, true, false, bs->outslot, NULL);
		}
	}
	if (more)
		elog(ERROR, "lionvec: the build's sort has an item past the last group");
	MarkBufferDirty(buf);
	UnlockReleaseBuffer(buf);
	pfree(item);
}

/*
 * The PART chain: an item a partition, in order, its centroid's plane 1
 * and REFINE item, its CODE chain's ends and its rows.  Returns its head.
 */
static BlockNumber
lionvec_build_write_parts(LionVecBuildState *bs, const BlockNumber *heads,
						  const BlockNumber *tails)
{
	Relation	index = bs->index;
	int			dimsp = bs->info->dimsp;
	int			nw = dimsp / 64;
	Size		size = LIONVEC_PART_SIZE(dimsp);
	LionVecPart *part = palloc0(size);
	BlockNumber head = InvalidBlockNumber;
	Buffer		buf = InvalidBuffer;
	Page		page = NULL;
	int			j;

	for (j = 0; j < bs->nparts; j++)
	{
		part->partno = (uint32) j;
		part->head = heads[j];
		part->tail = tails[j];
		part->nrows = bs->pcount[j];
		part->maxnorm = bs->pmaxnorm[j];
		part->spread = bs->pcount[j] > 0 ?
			(float4) (bs->psumsq[j] / bs->pcount[j]) : 0.0f;
		LionVecCentTid(&part->cent.tid, j);
		part->cent.flags = 0;
		part->cent.norm = bs->cnorm[j];
		part->cent.alpha1 = bs->calpha1[j];
		part->cent.xm = bs->cxm[j];
		part->cent.rblk = bs->crblk[j];
		part->cent.roff = bs->croff[j];
		part->cent.popcount = bs->cpop[j];
		memcpy(part->cent.bits, bs->cbits + (Size) j * nw, sizeof(uint64) * nw);

		if (!BufferIsValid(buf) || PageGetFreeSpace(page) < MAXALIGN(size))
		{
			Buffer		next = lionvec_new_buffer(index, MAIN_FORKNUM);

			if (BufferIsValid(buf))
			{
				LionVecPageGetOpaque(page)->next = BufferGetBlockNumber(next);
				MarkBufferDirty(buf);
				UnlockReleaseBuffer(buf);
			}
			else
				head = BufferGetBlockNumber(next);
			buf = next;
			page = BufferGetPage(buf);
			lionvec_init_page(page, LIONVEC_PAGE_PART);
		}
		if (PageAddItemExtended(page, (char *) part, size, InvalidOffsetNumber, 0) ==
			InvalidOffsetNumber)
			elog(ERROR, "lionvec: could not add a partition of %zu bytes", size);
	}
	if (BufferIsValid(buf))
	{
		MarkBufferDirty(buf);
		UnlockReleaseBuffer(buf);
	}
	pfree(part);
	return head;
}

/* The dimensions the column's type modifier fixes, or 0. */
static int
lionvec_typmod_dims(Relation index)
{
	int32		typmod = TupleDescAttr(RelationGetDescr(index), 0)->atttypmod;

	if (typmod > 0)
	{
		lionvec_check_max_dims(typmod);
		return typmod;
	}
	return 0;
}

static IndexBuildResult *
lionvecbuild(Relation heap, Relation index, IndexInfo *indexInfo)
{
	IndexBuildResult *result;
	LionVecBuildState bs;
	Buffer		metabuf;
	LionVecMetaData meta;
	double		reltuples;
	BlockNumber *heads;
	BlockNumber *tails;

	if (RelationGetNumberOfBlocks(index) != 0)
		elog(ERROR, "index \"%s\" already contains data",
			 RelationGetRelationName(index));

	memset(&bs, 0, sizeof(bs));
	bs.index = index;
	bs.metric = lionvec_index_metric(index);
	bs.dims = lionvec_typmod_dims(index);
	bs.rbuf = InvalidBuffer;
	bs.refine_tail = InvalidBlockNumber;
	bs.buildcxt = CurrentMemoryContext;
	bs.tmpcxt = AllocSetContextCreate(CurrentMemoryContext,
									  "lionvec build row",
									  ALLOCSET_DEFAULT_SIZES);

	/* the meta page is block 0, filled in last */
	metabuf = lionvec_new_buffer(index, MAIN_FORKNUM);
	Assert(BufferGetBlockNumber(metabuf) == LIONVEC_METAPAGE);
	lionvec_init_meta(BufferGetPage(metabuf), bs.metric, 0);
	MarkBufferDirty(metabuf);
	UnlockReleaseBuffer(metabuf);

	/*
	 * pass 1: the mean and the sample.  Neither pass joins a synchronized
	 * scan, which could start part-way through the heap: the second must see
	 * the rows in heap order, which each partition's CODE pages keep, and
	 * the sample is the same from build to build.
	 */
	table_index_build_scan(heap, index, indexInfo, false, false,
						   lionvec_build_mean_callback, &bs, NULL);

	/* the rotation, and the mean rotated, in this backend's memory */
	{
		int			dimsp = LIONVEC_PADDED(bs.dims);
		Size		size = lionvec_info_size(dimsp);

		bs.info = palloc0(size);
		bs.info->metric = bs.metric;
		bs.info->dims = bs.dims;
		bs.info->dimsp = dimsp;
		bs.info->seed = LIONVEC_SEED;
		lionvec_info_fix(bs.info, bs.nsum > 0);
		if (bs.dims > 0)
			lionvec_rotation_init(&bs.info->rot, dimsp, LIONVEC_SEED,
								  bs.info->rot.signs);
		if (bs.info->mean != NULL)
		{
			int			i;

			for (i = 0; i < dimsp; i++)
				bs.info->mean[i] = i < bs.dims ? (float) (bs.sum[i] / bs.nsum) : 0.0f;
			lionvec_rotate(&bs.info->rot, bs.info->mean);
			for (i = 0; i < dimsp; i++)
				bs.mean2 += (double) bs.info->mean[i] * bs.info->mean[i];
		}
	}

	memset(&meta, 0, sizeof(meta));
	meta.mean_head = InvalidBlockNumber;
	if (bs.info->mean != NULL)
		meta.mean_head = lionvec_build_write_mean(index, bs.info->mean,
												  bs.info->dimsp);

	/* the partitions, and whether rows spill to a second (two at least) */
	bs.nparts = bs.dims > 0 ? lionvec_partitions(index, bs.nsum, bs.sn) : 0;
	bs.spill = bs.nparts >= 2 &&
		(index->rd_options == NULL || ((LionVecOptions *) index->rd_options)->spill);
	if (bs.nparts > 0)
		lionvec_build_centroids(&bs);
	else if (bs.scodes != NULL)
	{
		pfree(bs.scodes);
		pfree(bs.sscale);
		bs.scodes = NULL;
		bs.sscale = NULL;
	}

	/* pass 2: the REFINE items in place, the CODE items to the sort */
	{
		TupleDesc	desc = CreateTemplateTupleDesc(2);
		AttrNumber	sortcol = 1;
		Oid			sortop = Int8LessOperator;
		Oid			sortcoll = InvalidOid;
		bool		nullsfirst = false;

		TupleDescInitBuiltinEntry(desc, (AttrNumber) 1, "key", INT8OID, -1, 0);
		TupleDescInitBuiltinEntry(desc, (AttrNumber) 2, "item", TEXTOID, -1, 0);
		TupleDescFinalize(desc);
		bs.inslot = MakeSingleTupleTableSlot(desc, &TTSOpsVirtual);
		bs.outslot = MakeSingleTupleTableSlot(desc, &TTSOpsMinimalTuple);
		bs.sort = tuplesort_begin_heap(desc, 1, &sortcol, &sortop, &sortcoll,
									   &nullsfirst, maintenance_work_mem, NULL,
									   TUPLESORT_NONE);
		bs.sortitem = palloc(VARHDRSZ + LIONVEC_CODE_SIZE(Max(bs.info->dimsp, 64)));
	}
	lionvec_items_alloc(&bs.items, bs.info->dimsp);
	reltuples = table_index_build_scan(heap, index, indexInfo, false, true,
									   lionvec_build_callback, &bs, NULL);
	if (BufferIsValid(bs.rbuf))
	{
		MarkBufferDirty(bs.rbuf);
		UnlockReleaseBuffer(bs.rbuf);
	}

	heads = palloc(sizeof(BlockNumber) * (bs.nparts + 2));
	tails = palloc(sizeof(BlockNumber) * (bs.nparts + 2));
	lionvec_build_write_codes(&bs, heads, tails);
	tuplesort_end(bs.sort);
	ExecDropSingleTupleTableSlot(bs.inslot);
	ExecDropSingleTupleTableSlot(bs.outslot);

	meta.part_head = InvalidBlockNumber;
	if (bs.nparts > 0)
		meta.part_head = lionvec_build_write_parts(&bs, heads, tails);

	/* the meta page */
	metabuf = ReadBuffer(index, LIONVEC_METAPAGE);
	LockBuffer(metabuf, BUFFER_LOCK_EXCLUSIVE);
	{
		LionVecMeta m = LionVecPageGetMeta(BufferGetPage(metabuf));

		m->dims = bs.info->dims;
		m->dimsp = bs.info->dimsp;
		m->flags = bs.spill ? LIONVEC_META_SPILL : 0;
		m->mean_head = meta.mean_head;
		m->part_head = meta.part_head;
		m->nparts = (uint32) bs.nparts;
		m->rest_head = heads[bs.nparts];
		m->rest_tail = tails[bs.nparts];
		m->null_head = heads[bs.nparts + 1];
		m->null_tail = tails[bs.nparts + 1];
		m->refine_tail = bs.refine_tail;
		m->scale = bs.nsq > 0.0 ?
			(float4) sqrt(bs.sumsq / bs.nsq / bs.info->dims) : 0.0f;
		m->ntuples = bs.indtuples;
	}
	MarkBufferDirty(metabuf);
	UnlockReleaseBuffer(metabuf);

	if (RelationNeedsWAL(index))
		log_newpage_range(index, MAIN_FORKNUM, 0,
						  RelationGetNumberOfBlocks(index), true);

	MemoryContextDelete(bs.tmpcxt);

	result = palloc(sizeof(IndexBuildResult));
	result->heap_tuples = reltuples;
	result->index_tuples = bs.indtuples;
	return result;
}

/*
 * An unlogged index's init fork: the meta page and the empty chains of rows
 * in no partition and of NULL rows.
 */
static void
lionvecbuildempty(Relation index)
{
	int			metric = lionvec_index_metric(index);
	int			dims = lionvec_typmod_dims(index);
	Buffer		metabuf;
	Buffer		rest;
	Buffer		nulls;
	LionVecMeta meta;

	metabuf = ExtendBufferedRel(BMR_REL(index), INIT_FORKNUM, NULL,
								EB_LOCK_FIRST | EB_SKIP_EXTENSION_LOCK);
	Assert(BufferGetBlockNumber(metabuf) == LIONVEC_METAPAGE);
	rest = ExtendBufferedRel(BMR_REL(index), INIT_FORKNUM, NULL,
							 EB_LOCK_FIRST | EB_SKIP_EXTENSION_LOCK);
	nulls = ExtendBufferedRel(BMR_REL(index), INIT_FORKNUM, NULL,
							  EB_LOCK_FIRST | EB_SKIP_EXTENSION_LOCK);

	START_CRIT_SECTION();
	lionvec_init_page(BufferGetPage(rest), LIONVEC_PAGE_CODE);
	MarkBufferDirty(rest);
	log_newpage_buffer(rest, true);
	lionvec_init_page(BufferGetPage(nulls), LIONVEC_PAGE_CODE);
	MarkBufferDirty(nulls);
	log_newpage_buffer(nulls, true);

	lionvec_init_meta(BufferGetPage(metabuf), metric, dims);
	meta = LionVecPageGetMeta(BufferGetPage(metabuf));
	meta->rest_head = meta->rest_tail = BufferGetBlockNumber(rest);
	meta->null_head = meta->null_tail = BufferGetBlockNumber(nulls);
	MarkBufferDirty(metabuf);
	log_newpage_buffer(metabuf, true);
	END_CRIT_SECTION();

	UnlockReleaseBuffer(nulls);
	UnlockReleaseBuffer(rest);
	UnlockReleaseBuffer(metabuf);
}

/* ----------------------------------------------------------------
 * Insert
 * ----------------------------------------------------------------
 */

/* Where an inserted row's CODE item, or its copy, goes. */
typedef struct LionVecDest
{
	BlockNumber tail;			/* the last page of its chain */
	int			partno;			/* its partition, or -1 */
	BlockNumber pblk;			/* ... whose PART item is here */
	OffsetNumber poff;
} LionVecDest;

/*
 * Partition partno's centroid's REFINE item, at (rblk, roff), share-locked
 * in *buf, which the caller releases.  Its loss is corruption, as are an
 * alpha8 or a length of its codes that is not a number.
 */
static LionVecRefine *
lionvec_cent_refine(Relation index, BlockNumber rblk, OffsetNumber roff,
					int partno, int dimsp, Buffer *buf)
{
	Page		page;
	ItemPointerData tid;
	LionVecRefine *ref;

	*buf = ReadBuffer(index, rblk);
	LockBuffer(*buf, BUFFER_LOCK_SHARE);
	page = BufferGetPage(*buf);
	lionvec_check_page(index, page, rblk, LIONVEC_PAGE_REFINE);
	LionVecCentTid(&tid, partno);
	ref = lionvec_refine_at(index, page, rblk, roff, dimsp, &tid);
	if (ref == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg("lionvec index \"%s\" has lost partition %d's centroid",
						RelationGetRelationName(index), partno)));
	if (isnan(ref->alpha8) || isinf(ref->alpha8) ||
		isnan(ref->rnorm8) || isinf(ref->rnorm8))
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg("lionvec index \"%s\" has a malformed centroid of partition %d at (%u,%u)",
						RelationGetRelationName(index), partno, rblk, roff)));
	return ref;
}

/*
 * The partition of a row with a vector (DESIGN.md §44.2): the centroid
 * nearest it, as the build finds it (lionvec_build_nearest()), from the PART
 * chain and the candidates' REFINE items.  xc[dimsp] is the row centred.
 * In an index that spills, *dest2 gets the partition of the row's copy:
 * SOAR's choice, as the build makes it, but of centroids decoded from their
 * 8-bit codes (lionvec_decode8()), the build's floats being gone - each
 * REFINE item read once, its codes kept for that; otherwise its partno is
 * -1.
 */
static void
lionvec_insert_nearest(Relation index, const LionVecMetaData *meta,
					   const float *xc, LionVecDest *dest, LionVecDest *dest2)
{
	int			dimsp = meta->dimsp;
	int			nw = dimsp / 64;
	int			k = (int) meta->nparts;
	bool		spill = (meta->flags & LIONVEC_META_SPILL) != 0;
	int			m = spill ? LIONVEC_SOAR_CANDS + 1 : 1;
	uint64	   *bits = palloc(sizeof(uint64) * (Size) k * nw);
	uint16	   *pop = palloc(sizeof(uint16) * k);
	float	   *norm = palloc(sizeof(float) * k);
	float	   *alpha1 = palloc(sizeof(float) * k);
	BlockNumber *rblk = palloc(sizeof(BlockNumber) * k);
	OffsetNumber *roff = palloc(sizeof(OffsetNumber) * k);
	BlockNumber *pblk = palloc(sizeof(BlockNumber) * k);
	OffsetNumber *poff = palloc(sizeof(OffsetNumber) * k);
	BlockNumber *tails = palloc(sizeof(BlockNumber) * k);
	double	   *lo = palloc(sizeof(double) * k);
	double	   *hi = palloc(sizeof(double) * k);
	int		   *cand = palloc(sizeof(int) * k);
	double	   *key = palloc(sizeof(double) * k);
	int			rank[LIONVEC_SOAR_CANDS + 1];
	uint8	   *ccodes = NULL;	/* spilling: [nc][dimsp], the candidates' codes */
	float	   *calpha8 = NULL;
	float	   *crnorm8 = NULL;
	LionVecCentroids cents;
	LionVecQuery q;
	BlockNumber blk = meta->part_head;
	BlockNumber nblocks = RelationGetNumberOfBlocks(index);
	uint64		visited = 0;
	int			n = 0;
	int			nc;
	int			nr;
	int			best;
	int			c;

	/* every centroid's plane 1 */
	while (BlockNumberIsValid(blk))
	{
		Buffer		buf;
		Page		page;
		OffsetNumber off;
		OffsetNumber maxoff;

		lionvec_chain_step(index, &visited, &nblocks);
		buf = ReadBuffer(index, blk);
		LockBuffer(buf, BUFFER_LOCK_SHARE);
		page = BufferGetPage(buf);
		lionvec_check_page(index, page, blk, LIONVEC_PAGE_PART);
		maxoff = PageGetMaxOffsetNumber(page);
		for (off = FirstOffsetNumber; off <= maxoff; off = OffsetNumberNext(off))
		{
			LionVecPart *part = lionvec_part_at(index, page, blk, off, dimsp);

			if (n >= k || part->partno != (uint32) n)
				ereport(ERROR,
						(errcode(ERRCODE_INDEX_CORRUPTED),
						 errmsg("lionvec index \"%s\" has partition %u where %d belongs, at (%u,%u)",
								RelationGetRelationName(index), part->partno, n, blk, off)));
			memcpy(bits + (Size) n * nw, part->cent.bits, sizeof(uint64) * nw);
			pop[n] = part->cent.popcount;
			norm[n] = part->cent.norm;
			alpha1[n] = part->cent.alpha1;
			rblk[n] = part->cent.rblk;
			roff[n] = part->cent.roff;
			pblk[n] = blk;
			poff[n] = off;
			tails[n] = part->tail;
			n++;
		}
		blk = LionVecPageGetOpaque(page)->next;
		UnlockReleaseBuffer(buf);
	}
	if (n != k)
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg("lionvec index \"%s\" has %d partitions, and its meta page says %d",
						RelationGetRelationName(index), n, k)));

	/* the candidates, ranked by their 8-bit codes */
	cents.k = k;
	cents.dimsp = dimsp;
	cents.bits = bits;
	cents.popcount = pop;
	cents.norm = norm;
	cents.alpha1 = alpha1;
	q.dimsp = dimsp;
	q.qc = palloc(sizeof(float) * dimsp);
	q.planes = palloc(sizeof(uint64) * nw * LIONVEC_QBITS);
	lionvec_query_init(&q, LIONVEC_L2, NULL, xc, LIONVEC_ASSIGN_EPS);
	nc = lionvec_candidates_n(&cents, &q, m, lo, hi, cand);
	if (spill)
	{
		/* each candidate's codes, for SOAR's decoding of the ranked ones */
		ccodes = palloc(sizeof(uint8) * (Size) nc * dimsp);
		calpha8 = palloc(sizeof(float) * nc);
		crnorm8 = palloc(sizeof(float) * nc);
	}
	for (c = 0; c < nc; c++)
	{
		int			j = cand[c];
		Buffer		buf;
		LionVecRefine *ref = lionvec_cent_refine(index, rblk[j], roff[j], j,
												 dimsp, &buf);
		double		est;
		double		err;

		lionvec_estimate8(&q, ref->codes, norm[j], ref->alpha8, ref->rnorm8,
						  &est, &err);
		if (spill)
		{
			memcpy(ccodes + (Size) c * dimsp, ref->codes, dimsp);
			calpha8[c] = ref->alpha8;
			crnorm8[c] = ref->rnorm8;
		}
		UnlockReleaseBuffer(buf);
		key[c] = (double) norm[j] * norm[j] - 2.0 * est;
	}
	nr = lionvec_rank(key, nc, m, rank);

	/*
	 * Centroids whose numbers lionvec_part_at() and lionvec_cent_refine()
	 * have checked leave min(k, m) candidates or more (lion_vec_math.h), so
	 * two or more in an index that spills, which has two partitions or more
	 * (lionvec_check_meta())
	 */
	if (nr < Min(k, m))
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg("lionvec index \"%s\" has malformed centroids: %d of %d near a row, fewer than %d",
						RelationGetRelationName(index), nr, k, Min(k, m))));

	best = cand[rank[0]];
	dest->partno = best;
	dest->tail = tails[best];
	dest->pblk = pblk[best];
	dest->poff = poff[best];

	/* the copy's: SOAR's, of the nearest's next (lionvec_build_nearest()) */
	dest2->partno = -1;
	dest2->tail = InvalidBlockNumber;
	dest2->pblk = InvalidBlockNumber;
	dest2->poff = InvalidOffsetNumber;
	if (spill)
	{
		float	   *cf = palloc(sizeof(float) * (Size) nr * dimsp);
		const float *cs[LIONVEC_SOAR_CANDS];
		int			s;
		int			i;

		for (i = 0; i < nr; i++)
		{
			int			c8 = rank[i];

			lionvec_decode8(ccodes + (Size) c8 * dimsp, dimsp, norm[cand[c8]],
							calpha8[c8], crnorm8[c8], cf + (Size) i * dimsp);
			if (i > 0)
				cs[i - 1] = cf + (Size) i * dimsp;
		}
		s = lionvec_soar_second(xc, cf, cs, nr - 1, dimsp, LIONVEC_SOAR_LAMBDA);
		Assert(s >= 0 && s < nr - 1);
		best = cand[rank[s + 1]];
		dest2->partno = best;
		dest2->tail = tails[best];
		dest2->pblk = pblk[best];
		dest2->poff = poff[best];
	}
}

/*
 * Append a CODE item to the tail of dest's chain, extended by a new page
 * when the tail is full: one WAL record of the tail page, any new one, and,
 * when the tail moves, the PART item or the meta page that holds it -
 * metabuf, which the caller holds exclusively locked; nullchain says which
 * of the meta page's chains a dest of no partition is.  The pages are locked
 * in that order and released before it returns.
 */
static void
lionvec_append_code(Relation index, Buffer metabuf, int dimsp,
					const LionVecCode *code, Size codesize,
					const LionVecDest *dest, bool nullchain)
{
	Buffer		cbuf;
	Buffer		nbuf = InvalidBuffer;
	Buffer		pbuf = InvalidBuffer;
	Page		page;
	GenericXLogState *state;

	cbuf = ReadBuffer(index, dest->tail);
	LockBuffer(cbuf, BUFFER_LOCK_EXCLUSIVE);
	lionvec_check_page(index, BufferGetPage(cbuf), dest->tail, LIONVEC_PAGE_CODE);
	if (BlockNumberIsValid(LionVecPageGetOpaque(BufferGetPage(cbuf))->next))
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg("lionvec index \"%s\" has a chain whose tail at block %u goes on",
						RelationGetRelationName(index), dest->tail)));
	if (PageGetFreeSpace(BufferGetPage(cbuf)) < MAXALIGN(codesize))
		nbuf = lionvec_new_buffer(index, MAIN_FORKNUM);
	if (dest->partno >= 0 && BufferIsValid(nbuf))
	{
		pbuf = ReadBuffer(index, dest->pblk);
		LockBuffer(pbuf, BUFFER_LOCK_EXCLUSIVE);
		lionvec_check_page(index, BufferGetPage(pbuf), dest->pblk, LIONVEC_PAGE_PART);
	}

	state = GenericXLogStart(index);
	page = GenericXLogRegisterBuffer(state, cbuf, 0);
	if (BufferIsValid(nbuf))
	{
		LionVecPageGetOpaque(page)->next = BufferGetBlockNumber(nbuf);
		page = GenericXLogRegisterBuffer(state, nbuf, GENERIC_XLOG_FULL_IMAGE);
		lionvec_init_page(page, LIONVEC_PAGE_CODE);
	}
	if (PageAddItemExtended(page, (char *) code, codesize,
							InvalidOffsetNumber, 0) == InvalidOffsetNumber)
		elog(ERROR, "lionvec: could not add an item of %zu bytes", codesize);
	lionvec_note_key(page, (ItemPointer) &code->tid);

	if (BufferIsValid(pbuf))
	{
		LionVecPart *part = lionvec_part_at(index, BufferGetPage(pbuf), dest->pblk,
											dest->poff, dimsp);

		if (part->partno != (uint32) dest->partno || part->tail != dest->tail)
			elog(ERROR, "lionvec: partition %d of index \"%s\" changed under an insert",
				 dest->partno, RelationGetRelationName(index));
		page = GenericXLogRegisterBuffer(state, pbuf, 0);
		part = (LionVecPart *) PageGetItem(page, PageGetItemId(page, dest->poff));
		part->tail = BufferGetBlockNumber(nbuf);
	}
	else if (BufferIsValid(nbuf))
	{
		LionVecMeta m;

		page = GenericXLogRegisterBuffer(state, metabuf, 0);
		m = LionVecPageGetMeta(page);
		if (nullchain)
			m->null_tail = BufferGetBlockNumber(nbuf);
		else
			m->rest_tail = BufferGetBlockNumber(nbuf);
	}
	GenericXLogFinish(state);

	if (BufferIsValid(pbuf))
		UnlockReleaseBuffer(pbuf);
	if (BufferIsValid(nbuf))
		UnlockReleaseBuffer(nbuf);
	UnlockReleaseBuffer(cbuf);
}

/*
 * Append a row's items: its REFINE item, and its CODE item to the tail of
 * its chain - its partition's, or that of the rows in no partition, or that
 * of the NULL rows - and, in an index that spills, its copy to the tail of
 * its second partition's.  The meta page's lock is held throughout, which
 * serializes inserts; VACUUM and scans lock one page at a time and never the
 * meta page with another, so the order meta, REFINE, CODE, PART, then the
 * copy's CODE and PART, each released before the next record's are taken,
 * cannot deadlock against them.  A WAL record a step, as a GenericXLog
 * record takes at most four buffers: the REFINE item with the meta page,
 * whose refine_tail it may move; then the CODE item, its page and any new
 * one, and the PART item or the meta page, whichever holds the chain's tail,
 * when the tail moves; then the copy, as the CODE item.  A crash after the
 * first leaves a REFINE item nothing points at, which costs its space and
 * nothing else; one after the second, a row without its copy, which a scan
 * still finds in its nearest partition, and misses only when it would have
 * found it through the copy.
 */
static void
lionvec_append(Relation index, const LionVecInfo *info, LionVecItems *it)
{
	int			dimsp = info->dimsp;
	Buffer		metabuf;
	Page		page;
	LionVecMetaData meta;
	LionVecDest dest;
	LionVecDest dest2;
	GenericXLogState *state;

	metabuf = ReadBuffer(index, LIONVEC_METAPAGE);
	LockBuffer(metabuf, BUFFER_LOCK_EXCLUSIVE);
	lionvec_check_meta(index, BufferGetPage(metabuf));
	meta = *LionVecPageGetMeta(BufferGetPage(metabuf));
	if (it->hasref && meta.dimsp != dimsp)
		elog(ERROR, "lionvec: index \"%s\" changed dimensions under an insert",
			 RelationGetRelationName(index));

	dest.partno = -1;
	dest.pblk = InvalidBlockNumber;
	dest.poff = InvalidOffsetNumber;
	dest2.partno = -1;
	if (!it->hasref)
		dest.tail = meta.null_tail;
	else if (meta.nparts == 0)
		dest.tail = meta.rest_tail;
	else
		lionvec_insert_nearest(index, &meta, it->x, &dest, &dest2);

	/* the REFINE item */
	if (it->hasref)
	{
		Buffer		rbuf = InvalidBuffer;
		bool		rnew = false;
		Size		rsize = LIONVEC_REFINE_SIZE(dimsp);
		OffsetNumber off;

		if (BlockNumberIsValid(meta.refine_tail))
		{
			rbuf = ReadBuffer(index, meta.refine_tail);
			LockBuffer(rbuf, BUFFER_LOCK_EXCLUSIVE);
			lionvec_check_page(index, BufferGetPage(rbuf), meta.refine_tail,
							   LIONVEC_PAGE_REFINE);
			if (PageGetFreeSpace(BufferGetPage(rbuf)) < MAXALIGN(rsize))
			{
				UnlockReleaseBuffer(rbuf);
				rbuf = InvalidBuffer;
			}
		}
		if (!BufferIsValid(rbuf))
		{
			rbuf = lionvec_new_buffer(index, MAIN_FORKNUM);
			rnew = true;
		}
		state = GenericXLogStart(index);
		page = GenericXLogRegisterBuffer(state, rbuf,
										 rnew ? GENERIC_XLOG_FULL_IMAGE : 0);
		if (rnew)
			lionvec_init_page(page, LIONVEC_PAGE_REFINE);
		off = PageAddItemExtended(page, (char *) it->ref, rsize,
								  InvalidOffsetNumber, 0);
		if (off == InvalidOffsetNumber)
			elog(ERROR, "lionvec: could not add a refine item of %zu bytes", rsize);
		it->code->rblk = BufferGetBlockNumber(rbuf);
		it->code->roff = off;
		if (rnew)
		{
			page = GenericXLogRegisterBuffer(state, metabuf, 0);
			LionVecPageGetMeta(page)->refine_tail = BufferGetBlockNumber(rbuf);
		}
		GenericXLogFinish(state);
		UnlockReleaseBuffer(rbuf);
	}

	/* the CODE item, and its copy: the same, pointing at the same REFINE item */
	lionvec_append_code(index, metabuf, dimsp, it->code, it->codesize, &dest,
						!it->hasref);
	if (dest2.partno >= 0)
	{
		Assert(it->hasref && dest2.partno != dest.partno);
		it->code->flags |= LIONVEC_SPILL;
		lionvec_append_code(index, metabuf, dimsp, it->code, it->codesize,
							&dest2, false);
		it->code->flags &= ~LIONVEC_SPILL;
	}
	UnlockReleaseBuffer(metabuf);
}

static bool
lionvecinsert(Relation index, Datum *values, bool *isnull, ItemPointer ht_ctid,
			  Relation heapRel, IndexUniqueCheck checkUnique,
			  bool indexUnchanged, IndexInfo *indexInfo)
{
	MemoryContext cxt = AllocSetContextCreate(CurrentMemoryContext,
											  "lionvec insert",
											  ALLOCSET_DEFAULT_SIZES);
	MemoryContext old = MemoryContextSwitchTo(cxt);
	LionVecInfo *info = lionvec_get_info(index);
	LionVecItems it;

	if (!isnull[0] && info->dims == 0)
	{
		Pointer		tofree;
		int			dim;

		(void) lionvec_datum_floats(values[0], &dim, &tofree);
		lionvec_set_dims(index, dim);
		info = lionvec_get_info(index);
	}

	lionvec_items_alloc(&it, info->dimsp);
	lionvec_items_fill(info, &it, ht_ctid, values[0], isnull[0]);

	/* a serializable reader holds the index's relation lock (§44.5) */
	CheckForSerializableConflictIn(index, NULL, InvalidBlockNumber);
	lionvec_append(index, info, &it);

	MemoryContextSwitchTo(old);
	MemoryContextDelete(cxt);
	return false;
}

/* ----------------------------------------------------------------
 * VACUUM
 * ----------------------------------------------------------------
 */

typedef struct LionVecRefineRef
{
	BlockNumber blk;
	OffsetNumber off;
	ItemPointerData tid;
} LionVecRefineRef;

static int
lionvec_ref_cmp(const void *a, const void *b)
{
	const LionVecRefineRef *ra = (const LionVecRefineRef *) a;
	const LionVecRefineRef *rb = (const LionVecRefineRef *) b;

	if (ra->blk != rb->blk)
		return ra->blk < rb->blk ? -1 : 1;
	if (ra->off != rb->off)
		return ra->off < rb->off ? -1 : 1;
	return 0;
}

/*
 * Remove the REFINE items of rows whose CODE items are gone, a page at a
 * time in block order.  Each is checked by TID before it is removed; one
 * that is not there - a crash between the two steps of an earlier VACUUM -
 * is left alone.  The line pointer stays (unused), so that the offsets the
 * other rows' CODE items hold stay right.
 */
static void
lionvec_vacuum_refines(Relation index, LionVecRefineRef *refs, int nrefs,
					   int dimsp, BufferAccessStrategy strategy)
{
	int			i = 0;

	if (nrefs == 0)
		return;
	qsort(refs, nrefs, sizeof(LionVecRefineRef), lionvec_ref_cmp);
	while (i < nrefs)
	{
		BlockNumber blk = refs[i].blk;
		Buffer		buf;
		Page		page;
		GenericXLogState *state;
		int			ndel = 0;

		lion_vacuum_delay_point();
		buf = ReadBufferExtended(index, MAIN_FORKNUM, blk, RBM_NORMAL, strategy);
		LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
		lionvec_check_page(index, BufferGetPage(buf), blk, LIONVEC_PAGE_REFINE);
		state = GenericXLogStart(index);
		page = GenericXLogRegisterBuffer(state, buf, 0);
		for (; i < nrefs && refs[i].blk == blk; i++)
		{
			if (lionvec_refine_at(index, page, blk, refs[i].off, dimsp,
								  &refs[i].tid) == NULL)
				continue;
			PageIndexTupleDeleteNoCompact(page, refs[i].off);
			ndel++;
		}
		if (ndel > 0)
			GenericXLogFinish(state);
		else
			GenericXLogAbort(state);
		UnlockReleaseBuffer(buf);
	}
}

/* What a VACUUM is gathering as it goes from chain to chain. */
typedef struct LionVecVacuum
{
	IndexVacuumInfo *info;
	IndexBulkDeleteResult *stats;
	IndexBulkDeleteCallback callback;
	void	   *callback_state;
	int			dimsp;
	bool		spill;			/* the index spills (LIONVEC_META_SPILL) */
	LionVecRefineRef *refs;		/* the REFINE items to remove, flushed when full */
	int			nrefs;
	int			maxrefs;
	uint64		visited;		/* lionvec_chain_step()'s, over every chain */
	BlockNumber nblocks;
} LionVecVacuum;

/*
 * Remove the dead rows of one CODE chain, partition says whether it is a
 * partition's.  A row that spills has an item in two chains, and each goes
 * as its chain is walked; its REFINE item, which both point at, is taken
 * from the primary alone, so that it is removed once, and the row is
 * counted once, by its primary.
 */
static void
lionvec_vacuum_chain(LionVecVacuum *vs, BlockNumber blk, bool partition)
{
	Relation	index = vs->info->index;

	while (BlockNumberIsValid(blk))
	{
		Buffer		buf;
		Page		page;
		OffsetNumber dels[MaxOffsetNumber];
		int			ndel = 0;
		OffsetNumber off;
		OffsetNumber maxoff;
		BlockNumber next;

		lion_vacuum_delay_point();
		lionvec_chain_step(index, &vs->visited, &vs->nblocks);
		buf = ReadBufferExtended(index, MAIN_FORKNUM, blk, RBM_NORMAL,
								 vs->info->strategy);
		LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
		page = BufferGetPage(buf);
		lionvec_check_page(index, page, blk, LIONVEC_PAGE_CODE);
		maxoff = PageGetMaxOffsetNumber(page);
		for (off = FirstOffsetNumber; off <= maxoff; off = OffsetNumberNext(off))
		{
			LionVecCode *code = lionvec_code_at(index, page, blk, off, vs->dimsp,
												vs->spill && partition);
			bool		copy = (code->flags & LIONVEC_SPILL) != 0;

			if (vs->callback(&code->tid, vs->callback_state))
			{
				dels[ndel++] = off;
				if (!copy)
					vs->stats->tuples_removed += 1;
				if (!copy && BlockNumberIsValid(code->rblk))
				{
					vs->refs[vs->nrefs].blk = code->rblk;
					vs->refs[vs->nrefs].off = code->roff;
					vs->refs[vs->nrefs].tid = code->tid;
					vs->nrefs++;
				}
			}
			else if (!copy)
				vs->stats->num_index_tuples += 1;
		}
		if (ndel > 0)
		{
			GenericXLogState *state = GenericXLogStart(index);

			page = GenericXLogRegisterBuffer(state, buf, 0);
			PageIndexMultiDelete(page, dels, ndel);
			GenericXLogFinish(state);
		}
		next = LionVecPageGetOpaque(BufferGetPage(buf))->next;
		UnlockReleaseBuffer(buf);

		/* room for the next page's, or flush */
		if (vs->nrefs > vs->maxrefs - MaxOffsetNumber)
		{
			lionvec_vacuum_refines(index, vs->refs, vs->nrefs, vs->dimsp,
								   vs->info->strategy);
			vs->nrefs = 0;
		}
		blk = next;
	}
}

/*
 * The CODE chains' heads: the partitions', from the PART chain, then the
 * rows' in no partition and the NULL rows'.  palloc'd; *n of them.
 */
BlockNumber *
lionvec_chain_heads(Relation index, const LionVecMetaData *meta, int *n)
{
	BlockNumber *heads = palloc(sizeof(BlockNumber) * ((Size) meta->nparts + 2));
	BlockNumber blk = meta->part_head;
	BlockNumber nblocks = RelationGetNumberOfBlocks(index);
	uint64		visited = 0;
	int			k = 0;

	while (BlockNumberIsValid(blk))
	{
		Buffer		buf;
		Page		page;
		OffsetNumber off;
		OffsetNumber maxoff;

		lionvec_chain_step(index, &visited, &nblocks);
		buf = ReadBuffer(index, blk);
		LockBuffer(buf, BUFFER_LOCK_SHARE);
		page = BufferGetPage(buf);
		lionvec_check_page(index, page, blk, LIONVEC_PAGE_PART);
		maxoff = PageGetMaxOffsetNumber(page);
		for (off = FirstOffsetNumber; off <= maxoff; off = OffsetNumberNext(off))
		{
			LionVecPart *part = lionvec_part_at(index, page, blk, off, meta->dimsp);

			if (k >= (int) meta->nparts || part->partno != (uint32) k)
				ereport(ERROR,
						(errcode(ERRCODE_INDEX_CORRUPTED),
						 errmsg("lionvec index \"%s\" has partition %u where %d belongs, at (%u,%u)",
								RelationGetRelationName(index), part->partno, k, blk, off)));
			heads[k++] = part->head;
		}
		blk = LionVecPageGetOpaque(page)->next;
		UnlockReleaseBuffer(buf);
	}
	if (k != (int) meta->nparts)
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg("lionvec index \"%s\" has %d partitions, and its meta page says %u",
						RelationGetRelationName(index), k, meta->nparts)));
	heads[k++] = meta->rest_head;
	heads[k++] = meta->null_head;
	*n = k;
	return heads;
}

static IndexBulkDeleteResult *
lionvecbulkdelete(IndexVacuumInfo *info, IndexBulkDeleteResult *stats,
				  IndexBulkDeleteCallback callback, void *callback_state)
{
	Relation	index = info->index;
	LionVecMetaData meta;
	LionVecVacuum vs;
	BlockNumber *heads;
	int			nheads;
	int			i;

	if (stats == NULL)
		stats = (IndexBulkDeleteResult *) palloc0(sizeof(IndexBulkDeleteResult));

	lionvec_read_meta(index, &meta);
	heads = lionvec_chain_heads(index, &meta, &nheads);

	memset(&vs, 0, sizeof(vs));
	vs.info = info;
	vs.stats = stats;
	vs.callback = callback;
	vs.callback_state = callback_state;
	vs.dimsp = meta.dimsp;
	vs.spill = (meta.flags & LIONVEC_META_SPILL) != 0;
	vs.nblocks = RelationGetNumberOfBlocks(index);
	/* the REFINE items to remove, in maintenance_work_mem */
	vs.maxrefs = (int) Min((Size) maintenance_work_mem * 1024 / sizeof(LionVecRefineRef),
						   MaxAllocSize / sizeof(LionVecRefineRef));
	vs.maxrefs = Max(vs.maxrefs, 2 * MaxOffsetNumber);
	vs.refs = palloc(sizeof(LionVecRefineRef) * vs.maxrefs);

	/* the partitions' chains first (lionvec_chain_heads()), then REST and NULL */
	for (i = 0; i < nheads; i++)
		lionvec_vacuum_chain(&vs, heads[i], i < (int) meta.nparts);
	lionvec_vacuum_refines(index, vs.refs, vs.nrefs, vs.dimsp, info->strategy);
	pfree(vs.refs);
	pfree(heads);

	stats->num_pages = RelationGetNumberOfBlocks(index);
	return stats;
}

static IndexBulkDeleteResult *
lionvecvacuumcleanup(IndexVacuumInfo *info, IndexBulkDeleteResult *stats)
{
	/* nothing to clean up beyond what ambulkdelete did, nor to count without it */
	if (info->analyze_only || stats == NULL)
		return stats;
	stats->num_pages = RelationGetNumberOfBlocks(info->index);
	return stats;
}

/* ----------------------------------------------------------------
 * Costs (§44.7)
 * ----------------------------------------------------------------
 */

/* a row's plane-1 estimate, in cpu_operator_costs: the popcounts and a sqrt */
#define LIONVEC_ROW_OPS			2.0

/*
 * The share of an index's rows a scan reads before its first row, of C
 * partitions, its rows filtered to a share sel: LIONVEC_PROBE_SHARE
 * C^(-1/3), and, filtered, sel^(-LIONVEC_PROBE_SEL) times as much - the k-th
 * nearest member lies farther out, past more partitions.  The power of C is
 * the simulation's, from 256 to 4,096 partitions of a million rows of 1536
 * dimensions; the constants are the benchmark's before the pause, 7.3% of
 * 1,974 partitions at a margin of 2 and 2.6 times as many under a filter
 * of 1%, where the margin was then narrowed by 2.6 (DESIGN.md §44.9).  A scan that pauses (pg_lion.vector_patience) reads
 * LIONVEC_PROBE_PAUSED C^(-1/3) and, filtered, sel^(-LIONVEC_PROBE_SEL_PAUSED)
 * times as much: at the default margin of 3, 3.9% of the benchmark's
 * partitions unfiltered, and 6.3%, 8.9%, 12% and 18% of them for filters of
 * 10%, 3%, 1% and 0.3%.
 */
#define LIONVEC_PROBE_SHARE		0.92
#define LIONVEC_PROBE_SEL		0.2
#define LIONVEC_PROBE_PAUSED	0.46
#define LIONVEC_PROBE_SEL_PAUSED 0.27

/*
 * What each further batch of pg_lion.vector_batch rows costs, as a share of
 * the first's start-up.  Without pauses the rows lie past ever more
 * partitions (7.3% of them probed for 10 rows, 23% for 100 and 61% for
 * 1,000, at a margin of 2).  A scan that pauses probed 4.1%, 4.6% and 50%:
 * the first batch comes from the rows it holds when it pauses, and the
 * deeper ones from probing much as without.  LIONVEC_DEEPER_PAUSED prices
 * the first batches; it keeps a plain scan with a Filter of 3% (333 rows,
 * 32 ms) ahead of LionOrdered (41 ms), and one of 1% (1,000 rows, 256 ms)
 * behind it (24 ms), though it prices the 1,000 rows low.
 */
#define LIONVEC_DEEPER			1.0
#define LIONVEC_DEEPER_PAUSED	0.06

/*
 * An index that spills (§44.3) holds two CODE items a row: its CODE pages
 * are twice as many and each partition twice as long.  At the same margin
 * a scan of it probes LIONVEC_SPILL_PROBES as many partitions as one
 * without, its nearest rows coming sooner: 91% to 94% of them at margins 0
 * to 4 over 115,000 OpenAI embeddings (DESIGN.md §44.9).  That is one data
 * set, not a model.  What spill buys is recall at a given margin, which the
 * costs do not see.
 */
#define LIONVEC_SPILL_PROBES	0.93

/*
 * What the planner needs of the index: the CODE pages' share of its
 * `pages` pages, by the dimensions the meta page records (or 1536 if none
 * yet), how many rows a CODE page holds, the partitions, and the CODE items
 * a row has - two if the build spilled (the meta page's flags), else one.
 */
static double
lionvec_code_pages(Oid indexoid, double pages, double *perpage,
				   double *nparts, double *items)
{
	Relation	index = index_open(indexoid, NoLock);
	LionVecMetaData meta;
	double		code;
	double		ref;
	int			dimsp;

	lionvec_read_meta(index, &meta);
	index_close(index, NoLock);
	*nparts = meta.nparts;
	*items = (meta.flags & LIONVEC_META_SPILL) != 0 ? 2.0 : 1.0;
	dimsp = meta.dims > 0 ? meta.dimsp : 1536;
	code = MAXALIGN(LIONVEC_CODE_SIZE(dimsp)) + sizeof(ItemIdData);
	ref = MAXALIGN(LIONVEC_REFINE_SIZE(dimsp)) + sizeof(ItemIdData);
	*perpage = Max(1.0, floor((BLCKSZ - SizeOfPageHeaderData -
							   MAXALIGN(sizeof(LionVecPageOpaqueData))) / code));
	return Max(1.0, ceil(pages * *items * code / (*items * code + ref)));
}

/*
 * The cost of a scan's first row, which is most of its cost.  Restricted to
 * few enough members, the members' distances from the heap, every one
 * (§44.6).  Otherwise every centroid's estimate, then the partitions a
 * scan probes, a random read each and their CODE pages in order, plane 1 of
 * their members, and a batch's refines and exact distances, each a page
 * read and a heap fetch.  `members` is how many rows a restriction admits,
 * or the index's rows.
 */
static Cost
lionvec_startup_cost(IndexOptInfo *index, double members, double *codepages)
{
	double		tuples = Max(index->tuples, 1.0);
	double		perpage;
	double		nparts;
	double		items;
	double		pages = lionvec_code_pages(index->indexoid, index->pages, &perpage,
										   &nparts, &items);
	double		frac = Min(1.0, Max(members, 0.0) / tuples);
	double		share;
	double		read;
	double		walked;
	double		spc_random;
	double		spc_seq;
	double		batch = Min((double) lionvec_batch, Max(members, 1.0));

	get_tablespace_page_costs(index->reltablespace, &spc_random, &spc_seq);

	if (frac < 1.0 && members <= LIONVEC_EXACT_MAX)
	{
		*codepages = 0.0;
		return Max(members, 1.0) * (spc_random + cpu_index_tuple_cost +
									LIONVEC_ROW_OPS * cpu_operator_cost);
	}

	share = 1.0;
	if (nparts >= 1.0 && lionvec_patience > 0)
		share = Min(1.0, LIONVEC_PROBE_PAUSED * pow(nparts, -1.0 / 3.0) *
					pow(Max(frac, 1e-9), -LIONVEC_PROBE_SEL_PAUSED));
	else if (nparts >= 1.0)
		share = Min(1.0, LIONVEC_PROBE_SHARE * pow(nparts, -1.0 / 3.0) *
					pow(Max(frac, 1e-9), -LIONVEC_PROBE_SEL));
	if (items > 1.0)
		share = Min(1.0, share * LIONVEC_SPILL_PROBES);
	read = Max(1.0, pages * share);
	walked = tuples * items * share;
	*codepages = read;
	return nparts * LIONVEC_ROW_OPS * cpu_operator_cost +
		Max(1.0, nparts * share) * spc_random + read * spc_seq +
		walked * cpu_index_tuple_cost * 0.1 +
		walked * frac * LIONVEC_ROW_OPS * cpu_operator_cost +
		batch * 2.0 * (spc_random + cpu_index_tuple_cost);
}

bool
lion_vec_index(IndexOptInfo *index)
{
	return index->amcostestimate == lionveccostestimate;
}

Cost
lion_vec_scan_cost(IndexOptInfo *index, double members)
{
	double		codepages;

	return lionvec_startup_cost(index, members, &codepages);
}

static void
lionveccostestimate(PlannerInfo *root, IndexPath *path, double loop_count,
					Cost *indexStartupCost, Cost *indexTotalCost,
					Selectivity *indexSelectivity, double *indexCorrelation,
					double *indexPages)
{
	IndexOptInfo *index = path->indexinfo;
	double		codepages;
	double		deeper;
	double		spc_random;
	double		spc_seq;

	*indexSelectivity = 1.0;
	*indexCorrelation = 0.0;

	/* nothing to give but an order: never a scan without one */
	if (path->indexorderbys == NIL)
	{
		*indexStartupCost = get_float8_infinity();
		*indexTotalCost = get_float8_infinity();
		*indexPages = index->pages;
		return;
	}

	get_tablespace_page_costs(index->reltablespace, &spc_random, &spc_seq);
	*indexStartupCost = lionvec_startup_cost(index, index->tuples, &codepages);

	/*
	 * Every further batch of pg_lion.vector_batch rows costs a share of the
	 * first's start-up (LIONVEC_DEEPER), and each row at worst a refine and
	 * an exact distance.  The planner prices a LIMIT under a Filter as LIMIT
	 * / selectivity rows of this, so a plain scan that has to walk past the
	 * rows a selective filter rejects costs more than LionOrdered, which
	 * reads the members alone; charged a refine and a fetch alone, a scan
	 * that did not pause tied LionOrdered at 1% and ran 7 times slower.
	 */
	deeper = lionvec_patience > 0 ? LIONVEC_DEEPER_PAUSED : LIONVEC_DEEPER;
	*indexTotalCost = *indexStartupCost +
		Max(index->tuples, 1.0) *
		(*indexStartupCost * deeper / Max(lionvec_batch, 1) +
		 spc_random + cpu_index_tuple_cost);
	*indexPages = codepages;
}

/* ----------------------------------------------------------------
 * Options, validation, the handler
 * ----------------------------------------------------------------
 */

static bytea *
lionvecoptions(Datum reloptions, bool validate)
{
	static const relopt_parse_elt tab[] = {
		{"partitions", RELOPT_TYPE_INT, offsetof(LionVecOptions, partitions)},
		{"spill", RELOPT_TYPE_BOOL, offsetof(LionVecOptions, spill)},
	};

	return (bytea *) build_reloptions(reloptions, validate,
									  lionvec_relopt_kind,
									  sizeof(LionVecOptions),
									  tab, lengthof(tab));
}

/*
 * An operator class of lionvec is one ORDER BY operator of strategy 1 (L2),
 * 2 (negative inner product) or 3 (cosine distance) over its input type, and
 * no support functions.
 */
static bool
lionvecvalidate(Oid opclassoid)
{
	bool		result = true;
	HeapTuple	classtup;
	Form_pg_opclass classform;
	Oid			opfamilyoid;
	Oid			opcintype;
	char	   *opclassname;
	CatCList   *proclist;
	CatCList   *oprlist;
	int			nops = 0;
	int			i;

	classtup = SearchSysCache1(CLAOID, ObjectIdGetDatum(opclassoid));
	if (!HeapTupleIsValid(classtup))
		elog(ERROR, "cache lookup failed for operator class %u", opclassoid);
	classform = (Form_pg_opclass) GETSTRUCT(classtup);
	opfamilyoid = classform->opcfamily;
	opcintype = classform->opcintype;
	opclassname = NameStr(classform->opcname);

	proclist = SearchSysCacheList1(AMPROCNUM, ObjectIdGetDatum(opfamilyoid));
	if (proclist->n_members > 0)
	{
		ereport(INFO,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("lionvec operator class \"%s\" has support functions, and lionvec uses none",
						opclassname)));
		result = false;
	}

	oprlist = SearchSysCacheList1(AMOPSTRATEGY, ObjectIdGetDatum(opfamilyoid));
	for (i = 0; i < oprlist->n_members; i++)
	{
		HeapTuple	oprtup = &oprlist->members[i]->tuple;
		Form_pg_amop oprform = (Form_pg_amop) GETSTRUCT(oprtup);

		if (oprform->amopstrategy < 1 ||
			oprform->amopstrategy > LIONVEC_NSTRATEGIES ||
			oprform->amoppurpose != AMOP_ORDER ||
			oprform->amoplefttype != opcintype ||
			oprform->amoprighttype != opcintype ||
			oprform->amopsortfamily == InvalidOid)
		{
			ereport(INFO,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 errmsg("lionvec operator class \"%s\" has operator %u of strategy %d, and lionvec takes one ORDER BY operator of strategy 1, 2 or 3 over its input type",
							opclassname, oprform->amopopr, oprform->amopstrategy)));
			result = false;
		}
		nops++;
	}
	if (nops != 1)
	{
		ereport(INFO,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("lionvec operator class \"%s\" has %d operators, and lionvec takes exactly one",
						opclassname, nops)));
		result = false;
	}

	ReleaseCatCacheList(oprlist);
	ReleaseCatCacheList(proclist);
	ReleaseSysCache(classtup);
	return result;
}

Datum
lionvec_handler(PG_FUNCTION_ARGS)
{
	static const IndexAmRoutine amroutine = {
		.type = T_IndexAmRoutine,
		.amstrategies = LIONVEC_NSTRATEGIES,
		.amsupport = 0,
		.amoptsprocnum = 0,
		.amcanorder = false,
		.amcanorderbyop = true,
#if PG_VERSION_NUM >= 180000
		.amcanhash = false,
		.amconsistentequality = false,
		.amconsistentordering = false,
#endif
		.amcanbackward = false,
		.amcanunique = false,
		.amcanmulticol = false,
		/* an ORDER BY with no WHERE at all is the whole point */
		.amoptionalkey = true,
		.amsearcharray = false,
		.amsearchnulls = false,
		.amstorage = false,
		.amclusterable = false,
		/* a reader takes a relation lock on the index (index_beginscan()) */
		.ampredlocks = false,
		.amcanparallel = false,
#if PG_VERSION_NUM >= 170000
		.amcanbuildparallel = false,
#endif
		.amcaninclude = false,
		.amusemaintenanceworkmem = false,
		.amsummarizing = false,
		.amparallelvacuumoptions = VACUUM_OPTION_PARALLEL_BULKDEL,
		.amkeytype = InvalidOid,

		.ambuild = lionvecbuild,
		.ambuildempty = lionvecbuildempty,
		.aminsert = lionvecinsert,
#if PG_VERSION_NUM >= 170000
		.aminsertcleanup = NULL,
#endif
		.ambulkdelete = lionvecbulkdelete,
		.amvacuumcleanup = lionvecvacuumcleanup,
		.amcanreturn = NULL,
		.amcostestimate = lionveccostestimate,
#if PG_VERSION_NUM >= 180000
		.amgettreeheight = NULL,
#endif
		.amoptions = lionvecoptions,
		.amproperty = NULL,
		.ambuildphasename = NULL,
		.amvalidate = lionvecvalidate,
		.amadjustmembers = NULL,
		.ambeginscan = lionvecbeginscan,
		.amrescan = lionvecrescan,
		.amgettuple = lionvecgettuple,
		.amgetbitmap = NULL,
		.amendscan = lionvecendscan,
		.ammarkpos = NULL,
		.amrestrpos = NULL,
		.amestimateparallelscan = NULL,
		.aminitparallelscan = NULL,
		.amparallelrescan = NULL,
#if PG_VERSION_NUM >= 180000
		.amtranslatestrategy = NULL,
		.amtranslatecmptype = NULL,
#endif
	};

#if PG_VERSION_NUM >= 190000
	PG_RETURN_POINTER(&amroutine);
#else
	{
		/* before 19 the caller pfree()s the result: see lion_handler() */
		IndexAmRoutine *copy = makeNode(IndexAmRoutine);

		*copy = amroutine;
		PG_RETURN_POINTER(copy);
	}
#endif
}
