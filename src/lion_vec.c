/*-------------------------------------------------------------------------
 *
 * lion_vec.c
 *		lionvec, the nearest-neighbour index of pgvector's `vector` and
 *		`halfvec`: its handler, settings, pages, build, inserts, VACUUM and
 *		costs.  The scan is lion_vec_scan.c, the arithmetic lion_vec_math.c
 *		(DESIGN.md §44).
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
 *	REGION	in an index of 64 partitions or more, one item a region, about
 *			sqrt(C) of them: a run of partitions whose centroids k-means
 *			gathered, the PART item the run starts at, and the region's
 *			centroid's plane 1.  The build routes each row through the
 *			regions nearest it to its partitions, and so does an insert,
 *			which reads only its regions' runs of the PART chain.
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
#include "access/parallel.h"
#include "access/generic_xlog.h"
#include "access/heaptoast.h"
#include "access/htup_details.h"
#include "access/reloptions.h"
#include "access/table.h"
#include "access/tableam.h"
#include "access/tupdesc.h"
#include "access/xloginsert.h"
#include "catalog/index.h"
#include "catalog/partition.h"
#include "catalog/pg_amop.h"
#include "catalog/pg_amproc.h"
#include "catalog/pg_opclass.h"
#include "catalog/pg_operator_d.h"
#include "catalog/pg_type.h"
#include "commands/defrem.h"
#include "commands/vacuum.h"
#include "common/pg_prng.h"
#include "executor/instrument.h"
#include "executor/tuptable.h"
#include "miscadmin.h"
#include "optimizer/clauses.h"
#include "optimizer/cost.h"
#include "optimizer/optimizer.h"
#include "pgstat.h"
#include "port/atomics.h"
#include "portability/instr_time.h"
#include "storage/bufmgr.h"
#include "storage/condition_variable.h"
#include "storage/predicate.h"
#include "storage/spin.h"
#include "tcop/tcopprot.h"
#include "utils/builtins.h"
#include "utils/float.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/regproc.h"
#include "utils/rel.h"
#include "utils/selfuncs.h"
#include "utils/snapmgr.h"
#include "utils/spccache.h"
#include "utils/syscache.h"
#include "utils/tuplesort.h"
#include "utils/wait_event.h"
#include "varatt.h"

#include "lion_compat.h"
#include "lion_readahead.h"
#include "lion_vec.h"

PG_FUNCTION_INFO_V1(lionvec_handler);

/* pg_lion.vector_epsilon, _batch, _margin, _patience, ... (§44.8) */
double		lionvec_epsilon = 4.0;
int			lionvec_batch = 100;
double		lionvec_margin = LIONVEC_MARGIN_DEFAULT;
int			lionvec_patience = LIONVEC_PATIENCE_DEFAULT;
int			lionvec_exact_rows = LIONVEC_EXACT_DEFAULT;
double		lionvec_exact_per_part = LIONVEC_EXACT_PER_PART_DEFAULT;
int			lionvec_overflow = LIONVEC_OVERFLOW_ERROR;
int			lionvec_postfilter = LIONVEC_POSTFILTER_ERROR;
int			lionvec_prefetch = LION_READAHEAD_DEFAULT;
bool		lionvec_toast_direct = true;
bool		lionvec_parallel_scan = false;

static const struct config_enum_entry lionvec_overflow_options[] = {
	{"error", LIONVEC_OVERFLOW_ERROR, false},
	{"stop", LIONVEC_OVERFLOW_STOP, false},
	{NULL, 0, false}
};

/*
 * pg_lion.vector_prefetch's (lion_readahead.h): stream where the server has
 * read streams, and on 16, which has none, a name for fadvise, so that one
 * setting serves every major; and on and off as the boolean it was until
 * 2026-10-17 took them, on the default.  The value a name stands for is what
 * SHOW prints, by the first entry that has it: on 16 fadvise for stream.
 */
static const struct config_enum_entry lionvec_prefetch_options[] = {
	{"off", LION_READAHEAD_OFF, false},
	{"fadvise", LION_READAHEAD_FADVISE, false},
#if PG_VERSION_NUM >= 170000
	{"stream", LION_READAHEAD_STREAM, false},
#else
	{"stream", LION_READAHEAD_FADVISE, true},
#endif
	{"on", LION_READAHEAD_DEFAULT, true},
	{"true", LION_READAHEAD_DEFAULT, true},
	{"yes", LION_READAHEAD_DEFAULT, true},
	{"1", LION_READAHEAD_DEFAULT, true},
	{"false", LION_READAHEAD_OFF, true},
	{"no", LION_READAHEAD_OFF, true},
	{"0", LION_READAHEAD_OFF, true},
	{NULL, 0, false}
};

static const struct config_enum_entry lionvec_postfilter_options[] = {
	{"exact", LIONVEC_POSTFILTER_EXACT, false},
	{"error", LIONVEC_POSTFILTER_ERROR, false},
	{"allow", LIONVEC_POSTFILTER_ALLOW, false},
	{NULL, 0, false}
};

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

StaticAssertDecl(LIONVEC_CODE_HDRSZ == 32,
				 "lionvec: the CODE item header is four words");
StaticAssertDecl(offsetof(LionVecRefine, codes) == 16,
				 "lionvec: the REFINE item header is two words");
StaticAssertDecl(LIONVEC_MAX_DIM >= 2000,
				 "lionvec: the page size takes the dimensions pgvector's own indexes do");
StaticAssertDecl(offsetof(LionVecRegion, cent) == 24,
				 "lionvec: the REGION item header is three words, its pads included");

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
							"How many units of rows a lionvec scan scores without finding a nearer one "
							"before it stops probing.",
							"A unit is the rows an average partition would hold were there 2 sqrt(n) of "
							"them, n the rows the index was built with: sqrt(n) / 2 rows, or that share "
							"of the members under a lion filter, whatever the partitions' number, and "
							"twice that in an index that spills, whose probes see each row twice.  A scan "
							"that has scored this many units since one last entered its "
							"pg_lion.vector_batch nearest stops probing partitions until it has returned "
							"the rows it holds.  Smaller stops sooner and misses more of the nearest rows; "
							"0 never stops.",
							&lionvec_patience,
							LIONVEC_PATIENCE_DEFAULT, 0, 1000000,
							PGC_USERSET,
							0,
							NULL, NULL, NULL);
	DefineCustomIntVariable("pg_lion.vector_exact_rows",
							"How many rows a lion filter may admit for a lionvec scan under it to "
							"compute every one's distance exactly instead of probing partitions.",
							"A scan restricted to this many rows or fewer, or to no more than "
							"pg_lion.vector_exact_per_partition rows a partition of the index, reads "
							"each one's vector from the table and returns the exact order, reading no "
							"code page; restricted to more, it probes the partitions nearest the query "
							"as an unfiltered scan does.  Larger makes more filters exact, at a table "
							"read a row; 0 leaves the choice to pg_lion.vector_exact_per_partition.",
							&lionvec_exact_rows,
							LIONVEC_EXACT_DEFAULT, 0, 100000000,
							PGC_USERSET,
							0,
							NULL, NULL, NULL);
	DefineCustomRealVariable("pg_lion.vector_exact_per_partition",
							 "How many rows a lion filter may admit for each of a lionvec index's "
							 "partitions, on average, for a scan under it to compute every one's "
							 "distance exactly instead of probing partitions.",
							 "Where the filter's rows are this few a partition, a probe reads a "
							 "partition's code pages for about one of them, and reading each one's "
							 "vector from the table costs less; a scan restricted to more rows than "
							 "this and pg_lion.vector_exact_rows allow probes.  Larger makes more "
							 "filters exact, at a table read a row; 0 leaves the choice to "
							 "pg_lion.vector_exact_rows.",
							 &lionvec_exact_per_part,
							 LIONVEC_EXACT_PER_PART_DEFAULT, 0.0, LIONVEC_EXACT_PER_PART_MAX,
							 PGC_USERSET,
							 0,
							 NULL, NULL, NULL);

	/*
	 * DESIGN.md §44.6, "Post-filtering": a lionvec scan under a Filter
	 * pauses and orders on the rows it returns, most of which the Filter
	 * rejects, so it finds other rows than the restricted scan LionOrdered
	 * runs at the same margin and patience (on 1M rows under a 1% filter at
	 * a margin of 8, 0.993 of the true ten for 29,075 pages against 0.980
	 * for 3,483).  The plan must not decide the answer, so by default no
	 * plan filters a lionvec scan's rows: lion_ordered.c takes the plain
	 * scan out of a restricted relation's paths, and a query LionOrdered
	 * cannot answer then fails at plan time (error, the default since
	 * 2026-10-17), its hint the subquery that makes the approximation
	 * explicit; exact plans such a query exactly instead.
	 */
	DefineCustomEnumVariable("pg_lion.vector_postfilter",
							 "What the planner does with a lionvec scan whose rows a filter would test "
							 "after the scan returns them.",
							 "A filter lion indexes answer restricts the scan through LionOrdered.  Any other "
							 "would reject rows the scan has returned, and those rows decide where it stops "
							 "and which rows it skips, so the answer would depend on the plan.  error, the "
							 "default, fails such a query when it is planned, and suggests a subquery that "
							 "filters an unfiltered scan's first rows; exact plans it exactly instead, by a "
							 "sort of the filter's rows; allow keeps the filtered scan as a plan to choose "
							 "by cost.  A clause of the vector column alone (IS NOT NULL, a distance "
							 "bound) leaves the scan's first rows as they are, and filters its rows "
							 "under every value.",
							 &lionvec_postfilter,
							 LIONVEC_POSTFILTER_ERROR,
							 lionvec_postfilter_options,
							 PGC_USERSET,
							 0,
							 NULL, NULL, NULL);

	/*
	 * DESIGN.md §44.4, "Prefetch" and "Read streams": a cold scan waits on
	 * one page read at a time, its probes' CODE chains, refines and heap
	 * fetches each a read the next cannot start before; read ahead by
	 * effective_io_concurrency pages, through a read stream or by asking the
	 * kernel, they come in meanwhile.  It changes no row returned, nor their
	 * order: which pages come first, never which are read.
	 */
	DefineCustomEnumVariable("pg_lion.vector_prefetch",
							 "How a lionvec scan reads ahead the pages it will read next.",
							 "A scan reads its partitions' code pages in runs the build wrote, and refines "
							 "and fetches rows from the top of its queue.  stream reads the next of those "
							 "pages through a read stream (PostgreSQL 17 and later), fadvise asks the kernel "
							 "for them (PrefetchBuffer), off reads each when it comes; as many ahead as the "
							 "tablespace's effective_io_concurrency, up to 64.  The rows returned are the "
							 "same whichever it is.",
							 &lionvec_prefetch,
							 LION_READAHEAD_DEFAULT,
							 lionvec_prefetch_options,
							 PGC_USERSET,
							 0,
							 NULL, NULL, NULL);

	/*
	 * DESIGN.md §44.13: a vector in TOAST read from its chunks' pages, by
	 * the places the index keeps, instead of through the heap page and the
	 * TOAST index; every place checked, and a row read so returned only once
	 * its heap tuple shows the same value.  Off reads every row as before,
	 * which the regression tests compare against.
	 */
	DefineCustomBoolVariable("pg_lion.vector_toast_direct",
							 "Whether a lionvec scan reads a vector stored in TOAST from its chunks' "
							 "pages, by the places its index keeps.",
							 "An index of vectors that may lie in TOAST keeps where each row's chunks are; "
							 "a scan then reads a row's vector there, checking every chunk, rather than "
							 "through the row's heap page and the TOAST index.  The rows returned are the "
							 "same either way.",
							 &lionvec_toast_direct,
							 true,
							 PGC_USERSET,
							 0,
							 NULL, NULL, NULL);

	/*
	 * DESIGN.md §44.4, "A parallel scan": correct, but slower than the serial
	 * scan on every query measured (each participant probes about as much as
	 * one scan does, and Gather Merge waits on the slowest), so its partial
	 * paths are offered only when asked for (lo_knn_partial()).
	 */
	DefineCustomBoolVariable("pg_lion.vector_parallel_scan",
							 "Whether the planner may scan a lionvec index in parallel.",
							 "Off by default: every participant of a parallel scan ranks all the "
							 "centroids and probes until its own margin and pause stop it, so on the "
							 "queries measured the parallel scan was slower than the serial one.",
							 &lionvec_parallel_scan,
							 false,
							 PGC_USERSET,
							 0,
							 NULL, NULL, NULL);

	DefineCustomEnumVariable("pg_lion.vector_overflow",
							 "What a lionvec scan does when what it knows of its rows outgrows hash_mem.",
							 "A scan with no LIMIT, or a large one, over rows its codes cannot tell apart "
							 "may need to know of more rows at once than hash_mem holds.  error fails the "
							 "query; stop returns no more rows, with a NOTICE, those returned being in "
							 "order, each once, and the nearest the scan found.",
							 &lionvec_overflow,
							 LIONVEC_OVERFLOW_ERROR,
							 lionvec_overflow_options,
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

/*
 * The most members a restriction may have for a scan of an index of nparts
 * partitions to compute every member's distance from the heap instead of
 * probing (lion_vec_scan.c, lv_exact(); DESIGN.md §44.6), which the scan
 * and its cost both ask, so that the planner prices what the scan does:
 * pg_lion.vector_exact_rows, or pg_lion.vector_exact_per_partition members
 * a partition if that is more.  The exact path costs a heap fetch a member,
 * whatever the partitions.  A probe reads a partition's CODE pages for the
 * members among them, and where those are about one a partition it reads a
 * page or two for each, and probes hundreds of partitions before the
 * pause, counted in members, comes.  So the members a partition decide,
 * not their number: on DBpedia's 115,186 rows in 2,715 partitions, warm,
 * the 3% filter's 3,472 members, 1.28 a partition, took 7.8 ms exact for
 * all ten nearest and 9.9 ms probed for 0.977, and the 10% filter's
 * 11,377, 4.2 a partition, 26 ms exact and 10.2 ms probed for 0.974; at a
 * million rows in 7,999 partitions the 1% filter's 10,063, 1.26 a
 * partition, took 11 ms exact and 13 to 20 ms probed for 0.956 to 0.98
 * (the maintainer's measurement).  Warm the two crossed at two to three
 * members a partition; the default ratio, 1.5, keeps below that for the
 * cold cost, a random heap read a member (183 ms exact against 67 probed
 * at that 3%).  The floor, 2,000, is about where the cold costs crossed
 * whatever the partitions, and all an index of few partitions has (§44.8,
 * §44.9).  Either at 0 leaves the other to decide.
 */
double
lionvec_exact_limit(double nparts)
{
	double		perpart = 0.0;

	if (nparts > 0.0 && lionvec_exact_per_part > 0.0)
		perpart = lionvec_exact_per_part * nparts;
	return Max((double) lionvec_exact_rows, perpart);
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
 * Whether a type is pgvector's `vector` or its `halfvec` (DESIGN.md §44.1):
 * by its name, as pg_lion knows neither type's OID, and its being a varlena,
 * which every read of a row or a query takes it for - a hand-made operator
 * class over another type would otherwise have lionvec detoast an int as a
 * pointer.  *half says which.
 */
static bool
lionvec_type_ok(Oid type, bool *half)
{
	HeapTuple	tup = SearchSysCache1(TYPEOID, ObjectIdGetDatum(type));
	Form_pg_type form;
	bool		ok;

	if (!HeapTupleIsValid(tup))
		elog(ERROR, "cache lookup failed for type %u", type);
	form = (Form_pg_type) GETSTRUCT(tup);
	*half = strcmp(NameStr(form->typname), "halfvec") == 0;
	ok = (*half || strcmp(NameStr(form->typname), "vector") == 0) &&
		form->typlen == -1 && !form->typbyval;
	ReleaseSysCache(tup);
	return ok;
}

/*
 * Whether the index's column is a `halfvec` rather than a `vector`: by its
 * operator class's input type.  The query an index's scan is given is of
 * that type too, as both sides of its operator are.  Any other type is
 * refused, before a row or a query of it is read.
 */
static bool
lionvec_index_half(Relation index)
{
	Oid			type = index->rd_opcintype[0];
	bool		half;

	if (!lionvec_type_ok(type, &half))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("lionvec index \"%s\" is of type %s, and lionvec indexes pgvector's vector and halfvec",
						RelationGetRelationName(index), format_type_be(type))));
	return half;
}

/*
 * The floats of a `vector` datum, or of a `halfvec` one if half, and their
 * count.  A vector's are its own, and *tofree is the detoasted copy to
 * pfree, or NULL.  A halfvec's are converted into buf, if it holds bufdim
 * floats and they fit, or else into an array palloc'd here, which *tofree
 * is then; its detoasted copy is freed here.  Either way the caller pfrees
 * *tofree, if not NULL, when done with the floats.
 */
const float *
lionvec_datum_floats(Datum d, bool half, float *buf, int bufdim, int *dim,
					 Pointer *tofree)
{
	struct varlena *raw = (struct varlena *) DatumGetPointer(d);
	struct varlena *v = pg_detoast_datum(raw);
	Size		len = VARSIZE(v);
	int			n = lionvec_datum_dim(v, len, half);

	if (n < 0)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("lionvec was given a malformed %s of %zu bytes",
						lionvec_type_name(half), len)));
	*dim = n;
	if (!half)
	{
		*tofree = v != raw ? (Pointer) v : NULL;
		return ((LionVecPgVector *) v)->x;
	}

	*tofree = NULL;
	if (buf == NULL || n > bufdim)
	{
		buf = palloc(sizeof(float) * Max(n, 1));
		*tofree = (Pointer) buf;
	}
	lionvec_halves(((LionVecPgHalfVector *) v)->x, n, buf);
	if (v != raw)
		pfree(v);
	return buf;
}

/*
 * Whether a row's vector lies in its heap page, so that a fetch reads the
 * block its TID names and little else: the column's storage keeps it there
 * (PLAIN, or MAIN, which moves a value out only when its row outgrows the
 * page), or its size does - dims dimensions of a vector, or of a halfvec if
 * half; 0 when not yet known, which this takes to be past any threshold.
 * The heap moves a row's EXTERNAL (pgvector's) and EXTENDED values to TOAST,
 * which a fetch reaches through the TOAST index, the heap page the least of
 * what it reads, only once the row is past TOAST_TUPLE_THRESHOLD, and then
 * only until it is within the table's toast_tuple_target
 * (TOAST_TUPLE_TARGET, the same 2,032 bytes at 8K pages, unless the table
 * sets it): a vector within the larger of the two stays, 506 dimensions of
 * vector and 1,012 of halfvec at the defaults, all 2,000 a vector index
 * takes under toast_tuple_target = 8160, which the DBpedia benchmark loads
 * its rows with (bench/vector/knn_dbpedia.py, KNN_INLINE).  Judged by the
 * threshold alone, as it was until 2026-10-17, those rows were taken to be
 * in TOAST and their pages were never asked for.  The rest of a row counts
 * too, which this leaves out: a vector just within may still go.  An
 * expression's vector is judged by its size alone.  The scan asks for heap
 * pages ahead only where this holds (lion_vec_scan.c, lv_start_prefetch();
 * DESIGN.md §44.4, "Prefetch"), and the build keeps a locator only where it
 * does not, or where it met a row in TOAST all the same (§44.13).
 */
bool
lionvec_vector_inline(Relation index, Relation heap, int dims, bool half)
{
	AttrNumber	attno = index->rd_index->indkey.values[0];
	Size		sz = 8 + (Size) dims * (half ? 2 : 4);
	int			target = RelationGetToastTupleTarget(heap, (int) TOAST_TUPLE_TARGET);
	Size		within = Max((Size) TOAST_TUPLE_THRESHOLD, (Size) target);

	if (attno > 0 && attno <= RelationGetDescr(heap)->natts)
	{
		char		storage = TupleDescAttr(RelationGetDescr(heap), attno - 1)->attstorage;

		if (storage == TYPSTORAGE_PLAIN || storage == TYPSTORAGE_MAIN)
			return true;
	}
	return dims > 0 && sz <= within;
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
lionvec_check_max_dims(int dims, bool half)
{
	if (dims < 1)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_EXCEPTION),
				 errmsg("%s must have at least 1 dimension",
						lionvec_type_name(half))));
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

void
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
 * finds it gone loses nothing.  reflocruns is the meta page's: the runs of
 * the TOAST place every REFINE item of the index ends with, 0 for none
 * (§44.13), of which an item uses no more.
 */
LionVecRefine *
lionvec_refine_at(Relation index, Page page, BlockNumber blk, OffsetNumber off,
				  int dimsp, int reflocruns, ItemPointer tid)
{
	ItemId		iid;
	LionVecRefine *ref;

	if (off < FirstOffsetNumber || off > PageGetMaxOffsetNumber(page))
		return NULL;
	iid = PageGetItemId(page, off);
	if (!ItemIdIsNormal(iid))
		return NULL;
	if (ItemIdGetLength(iid) != LIONVEC_REFINE_ITEM_SIZE(dimsp, reflocruns))
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg("lionvec index \"%s\" has a refine item of %u bytes at (%u,%u)",
						RelationGetRelationName(index), ItemIdGetLength(iid), blk, off)));
	ref = (LionVecRefine *) PageGetItem(page, iid);
	if (ref->nloc > reflocruns)
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg("lionvec index \"%s\" has a refine item of %u places at (%u,%u)",
						RelationGetRelationName(index), ref->nloc, blk, off)));
	/* not ItemPointerEquals(), which asserts against a centroid's TID */
	return ItemPointerGetBlockNumberNoCheck(&ref->tid) == ItemPointerGetBlockNumberNoCheck(tid) &&
		ItemPointerGetOffsetNumberNoCheck(&ref->tid) == ItemPointerGetOffsetNumberNoCheck(tid) ?
		ref : NULL;
}

/*
 * Whether off names a line pointer of the page, and that pointer an item of
 * size bytes lying inside the page's data: what a PART or REGION item's
 * reader checks before it reads a byte of it, as an insert follows a REGION
 * item's offset to a PART item without walking the page up to it.
 */
static bool
lionvec_item_ok(Page page, OffsetNumber off, Size size)
{
	ItemId		iid;

	if (off < FirstOffsetNumber || off > PageGetMaxOffsetNumber(page))
		return false;
	iid = PageGetItemId(page, off);
	return ItemIdIsNormal(iid) && ItemIdGetLength(iid) == size &&
		ItemIdGetOffset(iid) >= ((PageHeader) page)->pd_upper &&
		(Size) ItemIdGetOffset(iid) + size <= ((PageHeader) page)->pd_special;
}

/*
 * A PART item, checked: its place and length (lionvec_item_ok()), its
 * centroid's (which always has a vector), its chain's ends, and its
 * numbers - a length or a spread that is negative or not finite would make
 * a bound whose lower end lies past its upper.
 */
LionVecPart *
lionvec_part_at(Relation index, Page page, BlockNumber blk, OffsetNumber off,
				int dimsp)
{
	LionVecPart *part;

	if (dimsp <= 0 || !lionvec_item_ok(page, off, LIONVEC_PART_SIZE(dimsp)))
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg("lionvec index \"%s\" has a malformed partition at (%u,%u)",
						RelationGetRelationName(index), blk, off)));
	part = (LionVecPart *) PageGetItem(page, PageGetItemId(page, off));
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

/*
 * A REGION item, checked as a PART item is: its place and length, a
 * centroid with plane 1 and no REFINE item, and numbers a bound can be made
 * of; and a PART item to start at just when it has partitions.  Its numbers
 * against the rest of the chain are the reader's to check.
 */
LionVecRegion *
lionvec_region_at(Relation index, Page page, BlockNumber blk, OffsetNumber off,
				  int dimsp)
{
	LionVecRegion *reg;

	if (dimsp <= 0 || !lionvec_item_ok(page, off, LIONVEC_REGION_SIZE(dimsp)))
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg("lionvec index \"%s\" has a malformed region at (%u,%u)",
						RelationGetRelationName(index), blk, off)));
	reg = (LionVecRegion *) PageGetItem(page, PageGetItemId(page, off));
	if (reg->cent.flags != 0 || BlockNumberIsValid(reg->cent.rblk) ||
		(reg->nparts > 0) != BlockNumberIsValid(reg->pblk) ||
		(reg->nparts > 0 && reg->poff < FirstOffsetNumber) ||
		!(reg->cent.norm >= 0.0f) || isinf(reg->cent.norm) ||
		isnan(reg->cent.alpha1) || isinf(reg->cent.alpha1))
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg("lionvec index \"%s\" has a malformed region at (%u,%u)",
						RelationGetRelationName(index), blk, off)));
	return reg;
}

/* The meta page checked. */
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
	/*
	 * The locator's fields: all of them with the flag, none without.
	 * loc_root is not read (§44.13, "A second level"), and a later format
	 * may set it.
	 */
	if ((meta->flags & LIONVEC_META_LOCATOR) != 0 ?
		(!BlockNumberIsValid(meta->loc_head) ||
		 !BlockNumberIsValid(meta->loc_dtail) ||
		 !BlockNumberIsValid(meta->loc_tail) ||
		 !OidIsValid(meta->toastrelid) ||
		 meta->loc_bpages > meta->loc_built ||
		 (meta->loc_built > 0) != (meta->loc_bpages > 0) ||
		 meta->refloc_runs > LIONVEC_LOC_MAXRUNS ||
		 (meta->refloc_runs > 0 && meta->dims == 0)) :
		(BlockNumberIsValid(meta->loc_head) ||
		 BlockNumberIsValid(meta->loc_dtail) ||
		 BlockNumberIsValid(meta->loc_tail) ||
		 meta->refloc_runs != 0))
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg("lionvec index \"%s\" has a malformed meta page",
						RelationGetRelationName(index))));
	if (meta->metric < 1 || meta->metric > LIONVEC_NSTRATEGIES ||
		meta->dims < 0 || meta->dims > LIONVEC_MAX_DIM ||
		meta->dimsp != LIONVEC_PADDED(meta->dims) ||
		!BlockNumberIsValid(meta->rest_head) || !BlockNumberIsValid(meta->rest_tail) ||
		!BlockNumberIsValid(meta->null_head) || !BlockNumberIsValid(meta->null_tail) ||
		(meta->nparts > 0) != BlockNumberIsValid(meta->part_head) ||
		meta->nparts > LIONVEC_MAX_PARTS ||
		(meta->nregions > 0) != BlockNumberIsValid(meta->region_head) ||
		meta->nregions > meta->nparts ||
		(meta->nparts > 0 && meta->dims == 0) ||
		(meta->flags & ~(LIONVEC_META_SPILL | LIONVEC_META_LOCATOR)) != 0 ||
		((meta->flags & LIONVEC_META_SPILL) != 0 && meta->nparts < 2))
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg("lionvec index \"%s\" has a malformed meta page",
						RelationGetRelationName(index))));
}

/* A copy of the meta page, checked, the caller holding a lock on it. */
void
lionvec_meta_copy(Relation index, Page page, LionVecMetaData *meta)
{
	lionvec_check_meta(index, page);
	*meta = *LionVecPageGetMeta(page);
}

/* A copy of the meta page, read under a share lock. */
void
lionvec_read_meta(Relation index, LionVecMetaData *meta)
{
	Buffer		buf = ReadBuffer(index, LIONVEC_METAPAGE);

	LockBuffer(buf, BUFFER_LOCK_SHARE);
	lionvec_meta_copy(index, BufferGetPage(buf), meta);
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
	meta->region_head = InvalidBlockNumber;
	meta->nregions = 0;
	meta->loc_head = InvalidBlockNumber;
	meta->loc_dtail = InvalidBlockNumber;
	meta->loc_tail = InvalidBlockNumber;
	meta->toastrelid = InvalidOid;
	meta->loc_root = InvalidBlockNumber;
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
 * The rotation and mean of the index, and its column's type, as a palloc'd
 * copy the caller owns.  The backend keeps one in rd_amcache, made again
 * when the meta page says something else; a copy is returned rather than
 * that one because a relcache invalidation, which can arrive whenever a lock
 * is taken (a TOAST fetch's included), frees it.  The type is looked up
 * while rd_amcache is empty, for the lookup may take a lock.
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
		cached->metric != meta.metric || cached->seed != meta.seed ||
		cached->locator != ((meta.flags & LIONVEC_META_LOCATOR) != 0) ||
		cached->reflocruns != meta.refloc_runs ||
		cached->toastrelid != meta.toastrelid)
	{
		bool		half;

		if (cached != NULL)
		{
			index->rd_amcache = NULL;
			pfree(cached);
		}
		half = lionvec_index_half(index);
		size = lionvec_info_size(meta.dimsp);
		cached = MemoryContextAllocZero(index->rd_indexcxt, size);
		cached->metric = meta.metric;
		cached->half = half;
		cached->dims = meta.dims;
		cached->dimsp = meta.dimsp;
		cached->seed = meta.seed;
		cached->locator = (meta.flags & LIONVEC_META_LOCATOR) != 0;
		cached->reflocruns = meta.refloc_runs;
		cached->toastrelid = meta.toastrelid;
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
lionvec_set_dims(Relation index, int dims, bool half)
{
	Buffer		buf;
	Page		page;
	LionVecMeta meta;

	lionvec_check_max_dims(dims, half);
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
	LionVecRefine *ref;			/* refsize bytes */
	Size		refsize;		/* LIONVEC_REFINE_ITEM_SIZE(dimsp, reflocruns) */
	int			reflocruns;		/* the index's (LionVecInfo) */
	bool		hasref;
	LionVecLoc *loc;			/* its locator entry, if hasloc (§44.13) */
	bool		hasloc;
	float	   *x;				/* [dimsp] */
	float	   *h;				/* [hdim]: a halfvec's floats, or NULL */
	int			hdim;
} LionVecItems;

static void
lionvec_items_alloc(LionVecItems *it, const LionVecInfo *info)
{
	int			dimsp = info->dimsp;

	it->code = palloc0(LIONVEC_CODE_SIZE(Max(dimsp, 64)));
	it->reflocruns = info->reflocruns;
	it->refsize = LIONVEC_REFINE_ITEM_SIZE(dimsp, it->reflocruns);
	it->ref = palloc0(LIONVEC_REFINE_ITEM_SIZE(Max(dimsp, 64), it->reflocruns));
	it->loc = palloc0(LIONVEC_LOC_SIZE(LIONVEC_LOC_MAXRUNS));
	it->hasloc = false;
	it->x = palloc(sizeof(float) * Max(dimsp, 64));
	it->hdim = info->half ? info->dims : 0;
	it->h = it->hdim > 0 ? palloc(sizeof(float) * it->hdim) : NULL;
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
		const float *v = lionvec_datum_floats(value, info->half, it->h,
											  it->hdim, &dim, &tofree);
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
	it->ref->nloc = 0;
	it->ref->alpha8 = e.alpha8;
	it->ref->rnorm8 = e.rnorm8;
	if (it->reflocruns > 0)
		memset(LionVecRefineGetLoc(it->ref, info->dimsp), 0,
			   LIONVEC_REFLOC_SIZE(it->reflocruns));
	it->hasref = true;
}

/*
 * The row's place in TOAST, it->loc, into its REFINE item's slot, in an
 * index whose REFINE items have one and when its runs fit (§44.13); a row
 * whose place does not fit, or that has none, keeps an empty slot, and its
 * rerank reads it as it always did.
 */
static void
lionvec_items_place(LionVecItems *it, int dimsp)
{
	LionVecRefineLoc *rl;

	if (!it->hasref || it->reflocruns == 0 || !it->hasloc ||
		it->loc->nruns > it->reflocruns)
		return;
	rl = LionVecRefineGetLoc(it->ref, dimsp);
	rl->valueid = it->loc->valueid;
	memcpy(rl->runs, it->loc->runs, sizeof(LionVecLocRun) * it->loc->nruns);
	it->ref->nloc = it->loc->nruns;
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

/*
 * The most rows the reservoir holds, whatever maintenance_work_mem does: 64
 * a partition at LIONVEC_MAX_PARTS, half of what k-means is given a
 * partition at most (LIONVEC_SAMPLE_PER_PART_MAX).  It holds rows x
 * (dimsp + 4) bytes, 1,540 a row at 1536 dimensions, so 25.8 GB at this cap,
 * which a maintenance_work_mem of 52 GB reaches; it starts at
 * LIONVEC_SAMPLE_FIRST rows and doubles as it fills, so that a small table's
 * sample takes what its rows do and not the half of maintenance_work_mem it
 * may.
 */
#define LIONVEC_SAMPLE_MAX		(1 << 24)
#define LIONVEC_SAMPLE_FIRST	1024

/*
 * The fewest sample rows a partition the build chooses by itself, and the
 * fewest rows a table must have for it to make more than one.
 */
#define LIONVEC_SAMPLE_PER_PART	16
#define LIONVEC_MIN_AUTO_ROWS	10000

/*
 * The reservoir keeps what half of maintenance_work_mem holds, and the
 * partitions are chosen from all of it; k-means is then given
 * lionvec_kmeans_rows() of it (lion_vec_math.h, DESIGN.md §44.3), a
 * uniformly random choice of its rows kept in the reservoir's order
 * (lionvec_build_subsample()), from this seed (the reservoir's is
 * LIONVEC_SEED, the regions' one past it).
 */
#define LIONVEC_SEED_SUBSAMPLE	((uint64) LIONVEC_SEED + 2)

/*
 * The partitions the build chooses by itself, per square root of the rows:
 * 8 sqrt(n), 8,000 for a million rows, where it was 2 sqrt(n).  Partitions
 * a quarter the size read 1.55 to 1.78 times fewer rows for the same recall
 * in the numpy model of DBpedia's text-embedding-3-large rows, and 1.5
 * times fewer CODE pages for 0.95 of the true ten on an index of 115,186 of
 * them - no faster there, where ranking four times the centroids costs what
 * the pages save (DESIGN.md §44.3, §44.9).  The build's time grows with
 * the partitions.
 */
#define LIONVEC_PARTS_PER_ROOT	8.0

/*
 * The CODE items go through a sort by group - a partition, then the rows in
 * no partition, then the NULL rows and zeros - and then by the row's TID,
 * its place in the heap: the order a serial scan delivers the rows in, and
 * one a parallel build's, shared out among processes, comes back to.  A
 * row's copy goes in under the same TID in its second partition's group.
 * Each item carries the row's squared distance from its partition's
 * centroid, which the partitions' figures are summed from as the items come
 * out of the sort (lionvec_build_write_codes()): in the same order however
 * the rows were shared out.  The columns of the sort's tuples:
 */
#define LIONVEC_SORT_GROUP		1	/* int4 */
#define LIONVEC_SORT_TID		2	/* int8: the block above the offset */
#define LIONVEC_SORT_DIST2		3	/* float8 */
#define LIONVEC_SORT_ITEM		4	/* text: the CODE item's bytes */
#define LIONVEC_SORT_NATTS		4

/*
 * ... and after every chain's group, the locator's entries (§44.13), its
 * ITEM a LionVecLoc, keyed by TID too, so that its LOC pages come out of a
 * serial build and a parallel one alike, in heap order.
 */
#define LIONVEC_SORT_LOC_GROUP(nparts)	((uint32) (nparts) + 2)

StaticAssertDecl(LIONVEC_MAX_PARTS + 3 < PG_INT32_MAX,
				 "lionvec: a group's number fits the sort's int4");

/*
 * What k-means and the second pass share among a parallel build's
 * processes (DESIGN.md §44.12), and what a serial build keeps in the same
 * arrays, so that both run the same code over them: k-means' centroids and
 * its assignment of the sample, then the partitions' centroids - plane 1,
 * which k-means' iterations use for theirs too, their 8-bit codes and,
 * spilling, their floats - the rows' mean, and the centroids' regions, by
 * which both k-means and the second pass route a row (§44.3), each
 * iteration's and then the final centroids'.
 */
typedef struct LionVecBuildArrays
{
	float	   *cent;			/* [nparts][dimsp]: k-means' centroids, then
								 * (spilling) the partitions', centred */
	uint64	   *cbits;			/* [nparts][dimsp / 64]: plane 1 */
	uint16	   *cpop;			/* [nparts] */
	float	   *cnorm;			/* [nparts] */
	float	   *calpha1;		/* [nparts] */
	float	   *cxm;			/* [nparts] */
	uint8	   *ccodes;			/* [nparts][dimsp]: the 8-bit codes */
	float	   *calpha8;		/* [nparts] */
	float	   *crnorm8;		/* [nparts] */
	int		   *assign;			/* [sn]: k-means' partition of each sample row */
	float	   *mean;			/* [dimsp]: the rows' mean, rotated */
	/* the centroids' regions, lionvec_region_count(nparts) of them */
	uint64	   *rbits;			/* [nregions][dimsp / 64] */
	uint16	   *rpop;			/* [nregions] */
	float	   *rnorm;			/* [nregions] */
	float	   *ralpha1;		/* [nregions] */
	int		   *rfirst;			/* [nregions + 1] */
	int		   *rmember;		/* [nparts], when there are regions */
} LionVecBuildArrays;

typedef struct LionVecBuildLeader LionVecBuildLeader;

typedef struct LionVecBuildState
{
	Relation	index;
	int			metric;
	bool		half;			/* the column is a halfvec */
	int			dims;
	/* the first pass: the mean, and a sample (lionvec_build_sample()) */
	float	   *hbuf;			/* [dims]: a halfvec row's floats */
	double	   *sum;			/* [dims] */
	double		nsum;			/* rows with a vector (cosine: a nonzero one) */
	LionVecRotation srot;
	uint8	   *scodes;			/* [scap][dimsp] */
	float	   *sscale;			/* [scap] */
	int			scap;			/* rows they hold, growing to smax */
	int			smax;
	int			sn;
	int			sdrawn;			/* rows the reservoir drew, before the cut */
	pg_prng_state sprng;
	/* the partitions */
	LionVecInfo *info;
	int			nparts;
	bool		spill;			/* a copy of each row in a second partition */
	LionVecBuildArrays a;
	LionVecCentroids cents;		/* the centroids' plane 1, from a */
	LionVecRegions regions;		/* ... and their regions, from a; n 0: none */
	double		mean2;			/* |m|^2, m the mean */
	BlockNumber *crblk;			/* [nparts]: the centroids' REFINE items */
	OffsetNumber *croff;		/* [nparts] */
	/* the second pass: the items, each row's from its nearest centroids */
	LionVecItems items;
	LionVecQuery q;				/* a row, as its nearest centroid is found by */
	LionVecRoute route;			/* ... through the regions */
	double	   *lo;				/* [nparts] */
	double	   *hi;				/* [nparts] */
	int		   *cand;			/* [nparts] */
	double	   *ckey;			/* [nparts]: the candidates' keys */
	int			crank[LIONVEC_SOAR_CANDS + 1];	/* ... the least of them */
	const float *csoar[LIONVEC_SOAR_CANDS]; /* ... and SOAR's candidates */
	Buffer		rbuf;			/* the REFINE page being filled */
	BlockNumber refine_tail;
	Tuplesortstate *sort;		/* the CODE items in, by LIONVEC_SORT_* */
	TupleTableSlot *inslot;
	text	   *sortitem;		/* [VARHDRSZ + lionvec_build_item_max()] */
	double		indtuples;
	/* summed from the sort's output (lionvec_build_write_codes()) */
	uint32	   *pcount;			/* [nparts]: the items put in each, copies too */
	float	   *pmaxnorm;		/* [nparts]: the largest |x + m| of their rows */
	double	   *psumsq;			/* [nparts]: their sum of |x - c|^2 */
	double		sumsq;			/* sum of |x - c|^2, x a row and c its nearest */
	double		nsq;			/* ... over this many rows */
	BlockNumber *ppblk;			/* [nparts], with regions: each one's PART item */
	OffsetNumber *ppoff;		/* [nparts] */
	/* the locator (§44.13; lionvec_build_locator_plan()) */
	bool		locmay;			/* its vectors may lie in TOAST: keep one */
	bool		lookup;			/* look rows up in TOAST, a heap table's plain
								 * column with a TOAST relation */
	int			reflocruns;		/* runs a REFINE item's place holds, or 0 */
	LionVecToast toast;			/* the heap's TOAST relation, as rows are read */
	bool		keeploc;		/* ... as the build ends: it keeps one */
	LionVecMetaData locmeta;	/* ... and where (lionvec_loc_writer_end()) */
	double		nlocs;			/* entries written */
	LionVecBuildLeader *leader; /* a parallel build's, in its leader */
	MemoryContext buildcxt;		/* what lasts the build */
	MemoryContext tmpcxt;		/* reset after each row */
} LionVecBuildState;

Buffer
lionvec_new_buffer(Relation index, ForkNumber fork)
{
	return ExtendBufferedRel(BMR_REL(index), fork, NULL, EB_LOCK_FIRST);
}

/*
 * Lay a's arrays out from base, for nparts partitions of dimsp dimensions,
 * their lionvec_region_count() regions and a sample of sn rows, each
 * MAXALIGNed; returns the bytes they take.  At LIONVEC_MAX_PARTS partitions
 * of 1536 dimensions they are 2.1 GB, 1.6 GB of it the centroids' floats,
 * outside maintenance_work_mem (DESIGN.md §44.8).
 * With base NULL only the bytes are counted, and a is not touched.  The
 * sizes are checked for overflow (add_size(), mul_size()).
 */
static Size
lionvec_build_arrays(char *base, int nparts, int sn, int dimsp,
					 LionVecBuildArrays *a)
{
	Size		off = 0;
	Size		k = (Size) nparts;
	Size		g;

#define LIONVEC_PLACE(field, type, count) \
	do { \
		if (base != NULL) \
			a->field = (type *) (base + off); \
		off = add_size(off, MAXALIGN(mul_size(sizeof(type), (count)))); \
	} while (0)

	Assert(nparts >= 1 && sn >= nparts && dimsp >= 64 && dimsp % 64 == 0);
	g = (Size) lionvec_region_count(nparts);
	LIONVEC_PLACE(cent, float, mul_size(k, dimsp));
	LIONVEC_PLACE(cbits, uint64, mul_size(k, dimsp / 64));
	LIONVEC_PLACE(cpop, uint16, k);
	LIONVEC_PLACE(cnorm, float, k);
	LIONVEC_PLACE(calpha1, float, k);
	LIONVEC_PLACE(cxm, float, k);
	LIONVEC_PLACE(ccodes, uint8, mul_size(k, dimsp));
	LIONVEC_PLACE(calpha8, float, k);
	LIONVEC_PLACE(crnorm8, float, k);
	LIONVEC_PLACE(assign, int, (Size) sn);
	LIONVEC_PLACE(mean, float, (Size) dimsp);
	LIONVEC_PLACE(rbits, uint64, mul_size(g, dimsp / 64));
	LIONVEC_PLACE(rpop, uint16, g);
	LIONVEC_PLACE(rnorm, float, g);
	LIONVEC_PLACE(ralpha1, float, g);
	LIONVEC_PLACE(rfirst, int, g + 1);
	LIONVEC_PLACE(rmember, int, g > 0 ? k : 0);
#undef LIONVEC_PLACE
	return off;
}

/*
 * The rotation, and the mean rotated, as the build's processes each keep
 * them: of dims dimensions, a metric's and half's, and with the mean of
 * mean[dimsp] when it is not NULL.
 */
static LionVecInfo *
lionvec_build_info(int metric, bool half, int dims, const float *mean)
{
	int			dimsp = LIONVEC_PADDED(dims);
	LionVecInfo *info = palloc0(lionvec_info_size(dimsp));

	info->metric = metric;
	info->half = half;
	info->dims = dims;
	info->dimsp = dimsp;
	info->seed = LIONVEC_SEED;
	lionvec_info_fix(info, mean != NULL);
	if (dims > 0)
		lionvec_rotation_init(&info->rot, dimsp, LIONVEC_SEED, info->rot.signs);
	if (info->mean != NULL)
		memcpy(info->mean, mean, sizeof(float) * dimsp);
	return info;
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
		bs->scap = Min(bs->smax, LIONVEC_SAMPLE_FIRST);
		bs->scodes = MemoryContextAllocHuge(bs->buildcxt, (Size) bs->scap * dimsp);
		bs->sscale = palloc(sizeof(float) * bs->scap);
		bs->srot.signs = palloc(sizeof(float) * lionvec_rotation_nsigns(dimsp));
		lionvec_rotation_init(&bs->srot, dimsp, LIONVEC_SEED, bs->srot.signs);
		pg_prng_seed(&bs->sprng, (uint64) LIONVEC_SEED);
		MemoryContextSwitchTo(old);
	}

	if (bs->sn < bs->smax)
	{
		if (bs->sn == bs->scap)
		{
			/* full below smax: twice the room, the rows drawn the same */
			bs->scap = (int) Min((int64) bs->scap * 2, (int64) bs->smax);
			bs->scodes = repalloc_huge(bs->scodes, (Size) bs->scap * dimsp);
			bs->sscale = repalloc_huge(bs->sscale, sizeof(float) * (Size) bs->scap);
		}
		slot = bs->sn++;
	}
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
	v = lionvec_datum_floats(values[0], bs->half, bs->hbuf,
							 bs->hbuf != NULL ? bs->dims : 0, &dim, &tofree);
	if (bs->dims == 0)
	{
		lionvec_check_max_dims(dim, bs->half);
		bs->dims = dim;
		if (bs->half)
			bs->hbuf = MemoryContextAlloc(bs->buildcxt, sizeof(float) * dim);
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
 * Cut the sample to m < sn rows (lionvec_kmeans_rows()): a uniformly
 * random m of its slots (lionvec_subsample(), from a seed of its own, so
 * that a build of the same rows keeps the same), each moved down in place,
 * so that they keep the reservoir's order, and the arrays shrunk to them -
 * before k-means, and before a parallel build copies the sample into shared
 * memory, which then holds the m alone.
 */
static void
lionvec_build_subsample(LionVecBuildState *bs, int m)
{
	int			dimsp = bs->info->dimsp;
	uint32	   *slot = MemoryContextAllocHuge(CurrentMemoryContext,
											  sizeof(uint32) * (Size) bs->sn);
	int			i;

	Assert(m >= 1 && m < bs->sn);
	lionvec_subsample(bs->sn, m, LIONVEC_SEED_SUBSAMPLE, slot);
	for (i = 0; i < m; i++)
	{
		int			s = (int) slot[i];

		CHECK_FOR_INTERRUPTS();
		/* ascending and distinct, so never below i: row i is not needed now */
		Assert(s >= i && s < bs->sn);
		if (s == i)
			continue;
		memcpy(bs->scodes + (Size) i * dimsp, bs->scodes + (Size) s * dimsp, dimsp);
		bs->sscale[i] = bs->sscale[s];
	}
	pfree(slot);
	bs->sn = m;
	bs->scap = m;
	bs->scodes = repalloc_huge(bs->scodes, (Size) m * dimsp);
	bs->sscale = repalloc_huge(bs->sscale, sizeof(float) * (Size) m);
}

/*
 * How many partitions the build makes of nrows rows with a vector, sampled
 * sampled: as the index's `partitions` option asks, or else one for a table
 * of fewer than LIONVEC_MIN_AUTO_ROWS of them and LIONVEC_PARTS_PER_ROOT
 * times their square root for a larger (8,000 for a million rows), no more
 * than the sample has LIONVEC_SAMPLE_PER_PART rows for.  Never more than the
 * sample's rows, and none without one.
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
		want = Min(LIONVEC_PARTS_PER_ROOT * sqrt(nrows),
				   Max(1.0, floor((double) sampled / LIONVEC_SAMPLE_PER_PART)));
	want = Min(want, (double) sampled);
	return (int) Max(1.0, Min(want, (double) LIONVEC_MAX_PARTS));
}

/*
 * Put a REFINE item on the page being filled, or a new one.  The page stays
 * pinned between items but is locked only while one goes in: a content
 * lock holds off interrupts, and the build's rows are read between items -
 * so that a cancel, or in a parallel build a worker's error reaching the
 * leader or the leader's ending a worker, is seen at the next row.  Nothing
 * else reads the index's pages while it is being built.
 */
static void
lionvec_build_put_refine(LionVecBuildState *bs, LionVecRefine *ref, Size size,
						 BlockNumber *blk, OffsetNumber *off)
{
	Page		page;

	if (BufferIsValid(bs->rbuf))
	{
		LockBuffer(bs->rbuf, BUFFER_LOCK_EXCLUSIVE);
		if (PageGetFreeSpace(BufferGetPage(bs->rbuf)) < MAXALIGN(size))
		{
			UnlockReleaseBuffer(bs->rbuf);
			bs->rbuf = InvalidBuffer;
		}
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
	MarkBufferDirty(bs->rbuf);
	LockBuffer(bs->rbuf, BUFFER_LOCK_UNLOCK);
}

/* Let go of the REFINE page being filled, if there is one. */
static void
lionvec_build_end_refine(LionVecBuildState *bs)
{
	if (BufferIsValid(bs->rbuf))
	{
		ReleaseBuffer(bs->rbuf);
		bs->rbuf = InvalidBuffer;
	}
}

/* What lionvec_route() works in, for bs's regions: a process's own. */
static void
lionvec_build_route(LionVecBuildState *bs, LionVecRoute *rt)
{
	int			n = bs->regions.n;

	memset(rt, 0, sizeof(*rt));
	if (n == 0)
		return;
	rt->key = palloc(sizeof(double) * n);
	rt->order = palloc(sizeof(int) * n);
	rt->pick = palloc(sizeof(int) * n);
	rt->idx = palloc(sizeof(int) * bs->nparts);
}

static void
lionvec_build_route_free(LionVecRoute *rt)
{
	if (rt->key == NULL)
		return;
	pfree(rt->key);
	pfree(rt->order);
	pfree(rt->pick);
	pfree(rt->idx);
}

/*
 * Point bs at the arrays of a and the partitions' count it was laid out
 * for - the centroids and their regions, lionvec_region_count() of them,
 * which every process derives alike - and give it what the second pass
 * works in.
 */
static void
lionvec_build_use_arrays(LionVecBuildState *bs)
{
	int			k = bs->nparts;
	int			dimsp = bs->info->dimsp;
	LionVecRegions *r = &bs->regions;

	bs->cents.k = k;
	bs->cents.dimsp = dimsp;
	bs->cents.bits = bs->a.cbits;
	bs->cents.popcount = bs->a.cpop;
	bs->cents.norm = bs->a.cnorm;
	bs->cents.alpha1 = bs->a.calpha1;
	memset(r, 0, sizeof(*r));
	r->n = lionvec_region_count(k);
	if (r->n > 0)
	{
		r->k = k;
		r->dimsp = dimsp;
		r->bits = bs->a.rbits;
		r->popcount = bs->a.rpop;
		r->norm = bs->a.rnorm;
		r->alpha1 = bs->a.ralpha1;
		r->first = bs->a.rfirst;
		r->member = bs->a.rmember;
	}
	bs->q.dimsp = dimsp;
	bs->q.qc = palloc(sizeof(float) * dimsp);
	bs->q.planes = palloc(sizeof(uint64) * (dimsp / 64) * LIONVEC_QBITS);
	bs->lo = palloc(sizeof(double) * k);
	bs->hi = palloc(sizeof(double) * k);
	bs->cand = palloc(sizeof(int) * k);
	bs->ckey = palloc(sizeof(double) * k);
	lionvec_build_route(bs, &bs->route);
}

/*
 * What lionvec_kmeans() works in, its centroids' plane 1 in bs->a, and its
 * routing through bs's regions, if any, its own.
 */
static void
lionvec_build_kmeans_work(LionVecBuildState *bs, LionVecKmeansWork *w)
{
	int			k = bs->nparts;
	int			dimsp = bs->info->dimsp;

	memset(w, 0, sizeof(*w));
	w->count = palloc(sizeof(int) * k);
	w->assign = bs->a.assign;
	w->bits = bs->a.cbits;
	w->popcount = bs->a.cpop;
	w->norm = bs->a.cnorm;
	w->alpha1 = bs->a.calpha1;
	w->lo = palloc(sizeof(double) * k);
	w->hi = palloc(sizeof(double) * k);
	w->cand = palloc(sizeof(int) * k);
	w->x = palloc(sizeof(float) * dimsp);
	w->y = palloc(sizeof(float) * dimsp);
	w->codes = palloc(dimsp);
	w->qc = palloc(sizeof(float) * dimsp);
	w->planes = palloc(sizeof(uint64) * (dimsp / 64) * LIONVEC_QBITS);
	lionvec_build_route(bs, &w->route);
}

static void
lionvec_build_kmeans_work_free(LionVecKmeansWork *w)
{
	pfree(w->count);
	pfree(w->lo);
	pfree(w->hi);
	pfree(w->cand);
	pfree(w->x);
	pfree(w->y);
	pfree(w->codes);
	pfree(w->qc);
	pfree(w->planes);
	lionvec_build_route_free(&w->route);
}

/*
 * What lionvec_regions_make() works in, for bs's centroids and regions: the
 * leader's or a serial build's alone, the regions themselves being in bs->a.
 * The centroids' 8-bit codes are k dimsp bytes, 403 MB at
 * LIONVEC_MAX_PARTS partitions of 1536 dimensions.
 */
static void
lionvec_build_region_maker(LionVecBuildState *bs, LionVecRegionMaker *mk)
{
	int			k = bs->nparts;
	int			n = bs->regions.n;
	int			dimsp = bs->info->dimsp;
	LionVecKmeansWork *w = &mk->w;

	Assert(n >= 1 && n <= k);
	memset(mk, 0, sizeof(*mk));
	w->count = palloc(sizeof(int) * n);
	w->assign = palloc(sizeof(int) * k);
	w->bits = palloc(sizeof(uint64) * (Size) n * (dimsp / 64));
	w->popcount = palloc(sizeof(uint16) * n);
	w->norm = palloc(sizeof(float) * n);
	w->alpha1 = palloc(sizeof(float) * n);
	w->lo = palloc(sizeof(double) * n);
	w->hi = palloc(sizeof(double) * n);
	w->cand = palloc(sizeof(int) * n);
	w->x = palloc(sizeof(float) * dimsp);
	w->y = palloc(sizeof(float) * dimsp);
	w->codes = palloc(dimsp);
	w->qc = palloc(sizeof(float) * dimsp);
	w->planes = palloc(sizeof(uint64) * (dimsp / 64) * LIONVEC_QBITS);
	mk->codes = MemoryContextAllocHuge(CurrentMemoryContext,
									   mul_size((Size) k, (Size) dimsp));
	mk->scale = palloc(sizeof(float) * k);
	mk->cent = MemoryContextAllocHuge(CurrentMemoryContext,
									  mul_size(sizeof(float) * (Size) n, (Size) dimsp));
}

static void
lionvec_build_region_maker_free(LionVecRegionMaker *mk)
{
	LionVecKmeansWork *w = &mk->w;

	pfree(w->count);
	pfree(w->assign);
	pfree(w->bits);
	pfree(w->popcount);
	pfree(w->norm);
	pfree(w->alpha1);
	pfree(w->lo);
	pfree(w->hi);
	pfree(w->cand);
	pfree(w->x);
	pfree(w->y);
	pfree(w->codes);
	pfree(w->qc);
	pfree(w->planes);
	pfree(mk->codes);
	pfree(mk->scale);
	pfree(mk->cent);
}

/*
 * Number the partitions region by region (DESIGN.md §44.3): centroid
 * member[j] of the regions' member[] becomes partition j, the centroids'
 * floats moved in place along the permutation's cycles, tmp[dimsp] holding
 * a cycle's first, so that region g's partitions are the run first[g] ..
 * first[g + 1] - 1 and member[] is the identity.  Everything else of a
 * centroid is coded from its floats after, and k-means' assignment of the
 * sample, which still names the old numbers, is not read again.
 */
static void
lionvec_build_renumber(LionVecBuildState *bs, float *tmp)
{
	int			k = bs->nparts;
	int			dimsp = bs->info->dimsp;
	int		   *member = bs->regions.member;
	float	   *cent = bs->a.cent;
	bool	   *done = palloc0(sizeof(bool) * k);
	int			s;

	for (s = 0; s < k; s++)
	{
		int			j = s;

		CHECK_FOR_INTERRUPTS();
		if (done[s])
			continue;
		memcpy(tmp, cent + (Size) s * dimsp, sizeof(float) * dimsp);
		for (;;)
		{
			int			src = member[j];

			Assert(src >= 0 && src < k && !done[j]);
			done[j] = true;
			if (src == s)
			{
				memcpy(cent + (Size) j * dimsp, tmp, sizeof(float) * dimsp);
				break;
			}
			memcpy(cent + (Size) j * dimsp, cent + (Size) src * dimsp,
				   sizeof(float) * dimsp);
			j = src;
		}
	}
	for (s = 0; s < k; s++)
		member[s] = s;
	pfree(done);
}

static int	lionvec_parallel_kmeans(LionVecBuildState *bs, const LionVecSample *s,
									LionVecKmeansWork *w, LionVecRegionMaker *mk);

/*
 * The partitions: k-means over the sample, in the rotated space (which
 * changes no distance), spherical for cosine, whose rows are of unit length
 * - its iterations' rows shared out among a parallel build's processes
 * (lionvec_parallel_kmeans()), and routed, from LIONVEC_REGION_MIN
 * partitions, through regions of the centroids regrouped at each one - then
 * the final centroids' regions made and the partitions numbered region by
 * region (DESIGN.md §44.3), and each centroid coded as a row is, its 8-bit
 * codes in a REFINE item and its plane 1 kept for the PART chain and for
 * finding each row's nearest (lionvec_build_nearest()).  An index that
 * spills keeps the centroids' floats too, centred as the rows are, for
 * SOAR's choice of a row's second partition.
 */
static void
lionvec_build_centroids(LionVecBuildState *bs, const LionVecSample *sample)
{
	int			k = bs->nparts;
	int			dimsp = bs->info->dimsp;
	int			nw = dimsp / 64;
	bool		unit = bs->metric == LIONVEC_COSINE;
	bool		routed = bs->regions.n > 0;
	LionVecKmeansWork w;
	LionVecRegionMaker mk;
	LionVecRefine *ref;
	Size		rsize = LIONVEC_REFINE_ITEM_SIZE(dimsp, bs->reflocruns);
	int			made;
	int			j;
	char		regs[32] = "";
	char		drawn[32] = "";

	Assert(k >= 1 && k <= sample->n);
	lionvec_build_kmeans_work(bs, &w);
	if (routed)
		lionvec_build_region_maker(bs, &mk);
	else
		memset(&mk, 0, sizeof(mk));
	if (bs->leader != NULL)
		made = lionvec_parallel_kmeans(bs, sample, &w, routed ? &mk : NULL);
	else
		made = lionvec_kmeans(sample, k, LIONVEC_KMEANS_ITERS, unit,
							  (uint64) LIONVEC_SEED, bs->info->mean, bs->a.cent, &w,
							  routed ? &bs->regions : NULL, routed ? &mk : NULL);
	/* the regions, and the rows drawn when k-means was given fewer */
	if (routed)
		snprintf(regs, sizeof(regs), " in %d regions", bs->regions.n);
	if (bs->sdrawn > sample->n)
		snprintf(drawn, sizeof(drawn), " (%d drawn)", bs->sdrawn);
	elog(DEBUG1, "lionvec index \"%s\": %d partitions%s from a sample of %d rows%s, %d k-means iterations",
		 RelationGetRelationName(bs->index), k, regs, sample->n, drawn, made);

	/*
	 * The final centroids' regions, each partition in its nearest, and the
	 * partitions renumbered so that a region's are one run of the PART chain
	 * an insert reads alone (lionvec_insert_read_regions()).
	 */
	if (routed)
	{
		lionvec_regions_make(&bs->regions, bs->a.cent, unit,
							 LIONVEC_REGION_SEED((uint64) LIONVEC_SEED),
							 bs->info->mean, &mk);
		lionvec_build_renumber(bs, w.x);
		lionvec_build_region_maker_free(&mk);
	}

	bs->crblk = palloc(sizeof(BlockNumber) * k);
	bs->croff = palloc(sizeof(OffsetNumber) * k);
	ref = palloc0(rsize);
	for (j = 0; j < k; j++)
	{
		LionVecEncoded e;
		float	   *c = bs->a.cent + (Size) j * dimsp;

		memcpy(w.x, c, sizeof(float) * dimsp);
		e.bits = bs->a.cbits + (Size) j * nw;
		e.codes = bs->a.ccodes + (Size) j * dimsp;
		lionvec_encode(bs->info->mean, dimsp, w.x, &e);
		if (bs->spill)
			memcpy(c, w.x, sizeof(float) * dimsp);	/* centred */
		bs->a.cpop[j] = e.popcount;
		bs->a.cnorm[j] = e.norm;
		bs->a.calpha1[j] = e.alpha1;
		bs->a.cxm[j] = e.xm;
		bs->a.calpha8[j] = e.alpha8;
		bs->a.crnorm8[j] = e.rnorm8;

		LionVecCentTid(&ref->tid, j);
		ref->alpha8 = e.alpha8;
		ref->rnorm8 = e.rnorm8;
		memcpy(ref->codes, e.codes, dimsp);
		lionvec_build_put_refine(bs, ref, rsize, &bs->crblk[j], &bs->croff[j]);
	}
	pfree(ref);
	lionvec_build_kmeans_work_free(&w);
}

/*
 * The partition of the row whose centred vector is xc[dimsp], of norm
 * xnorm: the centroid nearest it by L2, among the candidates plane 1 leaves
 * (lionvec_candidates()) by their 8-bit codes, whose error is a seventieth
 * of plane 1's.  *dist2 is the row's squared distance from it.  From
 * LIONVEC_REGION_MIN partitions the candidates are only those of the beam
 * of regions nearest the row (lionvec_route(), DESIGN.md §44.3), the
 * partitions plane 1 is reckoned for a few hundred, not thousands.
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
	if (bs->regions.n > 0)
		nc = lionvec_route(&bs->regions, &bs->cents, &bs->q, m, &bs->route,
						   bs->lo, bs->hi, bs->cand);
	else
		nc = lionvec_candidates_n(&bs->cents, &bs->q, m, bs->lo, bs->hi,
								  bs->cand);
	for (c = 0; c < nc; c++)
	{
		int			j = bs->cand[c];
		double		est;
		double		err;

		lionvec_estimate8(&bs->q, bs->a.ccodes + (Size) j * dimsp, bs->a.cnorm[j],
						  bs->a.calpha8[j], bs->a.crnorm8[j], &est, &err);
		bs->ckey[c] = (double) bs->a.cnorm[j] * bs->a.cnorm[j] - 2.0 * est;
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
			bs->csoar[i - 1] = bs->a.cent + (Size) bs->cand[bs->crank[i]] * dimsp;
		s = lionvec_soar_second(xc, bs->a.cent + (Size) best * dimsp, bs->csoar,
								nr - 1, dimsp, LIONVEC_SOAR_LAMBDA);
		Assert(s >= 0 && s < nr - 1);
		*second = bs->cand[bs->crank[s + 1]];
		*dist2b = Max((double) xnorm * xnorm + bs->ckey[bs->crank[s + 1]], 0.0);
	}
	return best;
}

/*
 * Whether the index keeps a locator (DESIGN.md §44.13), as far as the build
 * can tell before its second pass, its dimensions known (or 0, none yet):
 * an index of a plain column - not an expression, whose values the build
 * computes and no TOAST relation holds - of a heap table with a TOAST
 * relation, whose vectors may lie there (lionvec_vector_inline() says not).
 * Of those, its REFINE items carry their rows' places when the dimensions
 * are known and an item with its place fits a page, every REFINE item of
 * the index then one size, the centroids' too.  The second pass looks up
 * the rows of any plain column of a heap table with a TOAST relation: a
 * build that meets one in TOAST all the same keeps a locator
 * (lionvec_build_write_codes()), its REFINE items without places.  An
 * index whose vectors lie inline - and no row of which does not - keeps
 * none: no page, and no work at insert or VACUUM.
 */
static void
lionvec_build_locator_plan(LionVecBuildState *bs, Relation heap)
{
	Relation	index = bs->index;
	AttrNumber	attno = index->rd_index->indkey.values[0];

	bs->lookup = attno > 0 && heap->rd_tableam == GetHeapamTableAmRoutine() &&
		OidIsValid(heap->rd_rel->reltoastrelid);
	bs->locmay = bs->lookup &&
		!lionvec_vector_inline(index, heap, bs->dims, bs->half);
	bs->reflocruns = 0;
	if (bs->locmay && bs->dims > 0)
	{
		int			nchunks = lionvec_toast_nchunks(lionvec_toast_extsize(bs->dims, bs->half),
													LION_TOAST_MAX_CHUNK_SIZE);
		int			runs = lionvec_refloc_runs(nchunks);
		Size		room = BLCKSZ - MAXALIGN(SizeOfPageHeaderData) -
			MAXALIGN(sizeof(LionVecPageOpaqueData));

		if (nchunks <= LIONVEC_LOC_MAXRUNS &&
			MAXALIGN(LIONVEC_REFINE_ITEM_SIZE(LIONVEC_PADDED(bs->dims), runs)) +
			sizeof(ItemIdData) <= room)
			bs->reflocruns = runs;
	}
}

/* The sort's tuples (LIONVEC_SORT_*). */
static TupleDesc
lionvec_build_sort_desc(void)
{
	TupleDesc	desc = CreateTemplateTupleDesc(LIONVEC_SORT_NATTS);

	TupleDescInitBuiltinEntry(desc, (AttrNumber) LIONVEC_SORT_GROUP, "grp", INT4OID, -1, 0);
	TupleDescInitBuiltinEntry(desc, (AttrNumber) LIONVEC_SORT_TID, "tid", INT8OID, -1, 0);
	/* float8 is not among TupleDescInitBuiltinEntry()'s types */
	TupleDescInitEntry(desc, (AttrNumber) LIONVEC_SORT_DIST2, "dist2", FLOAT8OID, -1, 0);
	TupleDescInitBuiltinEntry(desc, (AttrNumber) LIONVEC_SORT_ITEM, "item", TEXTOID, -1, 0);
	TupleDescFinalize(desc);
	return desc;
}

/*
 * A sort of the sort's tuples by group, then TID, of memkb kilobytes: a
 * serial build's, or with coordinate a parallel build's, a participant's own
 * or the leader's over them all.
 */
static Tuplesortstate *
lionvec_build_sort_begin(TupleDesc desc, int memkb, SortCoordinate coordinate)
{
	AttrNumber	cols[2] = {LIONVEC_SORT_GROUP, LIONVEC_SORT_TID};
	Oid			ops[2] = {Int4LessOperator, Int8LessOperator};
	Oid			colls[2] = {InvalidOid, InvalidOid};
	bool		nullsfirst[2] = {false, false};

	return tuplesort_begin_heap(desc, 2, cols, ops, colls, nullsfirst, memkb,
								coordinate, TUPLESORT_NONE);
}

/* The most bytes an item of the sort's takes: a CODE item, or a LOC entry. */
static Size
lionvec_build_item_max(int dimsp)
{
	return Max(LIONVEC_CODE_SIZE(Max(dimsp, 64)),
			   LIONVEC_LOC_SIZE(LIONVEC_LOC_MAXRUNS));
}

/*
 * Ready bs for the second pass, its items into a sort of memkb kilobytes,
 * and the heap's TOAST relation open for its rows' places, where the plan
 * says to look them up (lionvec_build_locator_plan()) - every process of a
 * parallel build its own.
 */
static void
lionvec_build_rows_begin(LionVecBuildState *bs, Relation heap, int memkb,
						 SortCoordinate coordinate)
{
	TupleDesc	desc = lionvec_build_sort_desc();

	bs->inslot = MakeSingleTupleTableSlot(desc, &TTSOpsVirtual);
	bs->sort = lionvec_build_sort_begin(desc, memkb, coordinate);
	bs->sortitem = palloc(VARHDRSZ + lionvec_build_item_max(bs->info->dimsp));
	bs->info->reflocruns = bs->reflocruns;
	lionvec_items_alloc(&bs->items, bs->info);
	memset(&bs->toast, 0, sizeof(bs->toast));
	if (bs->lookup && bs->info->dims > 0)
		(void) lionvec_toast_open(&bs->toast, heap, bs->info->dims, bs->half);
}

/* ... and done with the TOAST relation. */
static void
lionvec_build_rows_end(LionVecBuildState *bs)
{
	lionvec_toast_close(&bs->toast);
}

/*
 * A row's locator entry into the sort, after every chain's group, by its
 * TID (§44.13): the build's LOC pages come out in heap order.
 */
static void
lionvec_build_put_loc(LionVecBuildState *bs, const LionVecLoc *loc)
{
	BlockNumber blk = ItemPointerGetBlockNumber(&loc->tid);
	OffsetNumber off = ItemPointerGetOffsetNumber(&loc->tid);
	Size		size = LIONVEC_LOC_SIZE(loc->nruns);

	SET_VARSIZE(bs->sortitem, VARHDRSZ + size);
	memcpy(VARDATA(bs->sortitem), loc, size);
	ExecClearTuple(bs->inslot);
	bs->inslot->tts_values[LIONVEC_SORT_GROUP - 1] =
		Int32GetDatum((int32) LIONVEC_SORT_LOC_GROUP(bs->nparts));
	bs->inslot->tts_values[LIONVEC_SORT_TID - 1] =
		Int64GetDatum((int64) (((uint64) blk << 16) | off));
	bs->inslot->tts_values[LIONVEC_SORT_DIST2 - 1] = Float8GetDatum(0.0);
	bs->inslot->tts_values[LIONVEC_SORT_ITEM - 1] = PointerGetDatum(bs->sortitem);
	memset(bs->inslot->tts_isnull, 0, sizeof(bool) * LIONVEC_SORT_NATTS);
	ExecStoreVirtualTuple(bs->inslot);
	tuplesort_puttupleslot(bs->sort, bs->inslot);
}

/* An item into the sort, in group's chain, its row's distance dist2. */
static void
lionvec_build_put_code(LionVecBuildState *bs, uint32 group, const LionVecCode *code,
					   Size size, double dist2)
{
	BlockNumber blk = ItemPointerGetBlockNumberNoCheck(&code->tid);
	OffsetNumber off = ItemPointerGetOffsetNumberNoCheck(&code->tid);

	SET_VARSIZE(bs->sortitem, VARHDRSZ + size);
	memcpy(VARDATA(bs->sortitem), code, size);
	ExecClearTuple(bs->inslot);
	bs->inslot->tts_values[LIONVEC_SORT_GROUP - 1] = Int32GetDatum((int32) group);
	bs->inslot->tts_values[LIONVEC_SORT_TID - 1] =
		Int64GetDatum((int64) (((uint64) blk << 16) | off));
	bs->inslot->tts_values[LIONVEC_SORT_DIST2 - 1] = Float8GetDatum(dist2);
	bs->inslot->tts_values[LIONVEC_SORT_ITEM - 1] = PointerGetDatum(bs->sortitem);
	memset(bs->inslot->tts_isnull, 0, sizeof(bool) * LIONVEC_SORT_NATTS);
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
	double		dist2 = 0.0;
	double		dist2b = 0.0;
	Datum		value = values[0];
	LionToastPointer ptr;

	/*
	 * A row whose vector lies in TOAST as an entry's must (§44.13): its
	 * chunks found through the TOAST index as a detoast finds them, and the
	 * vector assembled from them - the bytes the index encodes, so that
	 * the entry names exactly what the codes were made of - with their
	 * places.  Anything irregular leaves the row without an entry, its
	 * vector detoasted as always.
	 */
	it->hasloc = false;
	if (!isnull[0] && bs->toast.toastrel != NULL &&
		lionvec_toast_value(&bs->toast, value, &ptr))
	{
		struct varlena *v = palloc(VARHDRSZ + bs->toast.extsize);

		if (lionvec_toast_locate(&bs->toast, &ptr, VARDATA(v), it->loc))
		{
			SET_VARSIZE(v, VARHDRSZ + bs->toast.extsize);
			value = PointerGetDatum(v);
			it->loc->tid = *tid;
			it->hasloc = true;
		}
	}

	lionvec_items_fill(bs->info, it, tid, value, isnull[0]);
	if (!it->hasref)
		group = bs->nparts + 1;		/* a NULL row or a zero */
	else if (bs->nparts == 0)
		group = 0;				/* the rows in no partition */
	else
		group = lionvec_build_nearest(bs, it->x, it->code->norm, &dist2,
									  &second, &dist2b);
	if (it->hasref)
	{
		lionvec_items_place(it, bs->info->dimsp);
		lionvec_build_put_refine(bs, it->ref, it->refsize,
								 &it->code->rblk, &it->code->roff);
	}

	lionvec_build_put_code(bs, group, it->code, it->codesize, dist2);
	if (second >= 0)
	{
		/* the copy: the same item, pointing at the same REFINE item */
		it->code->flags |= LIONVEC_SPILL;
		lionvec_build_put_code(bs, (uint32) second, it->code, it->codesize, dist2b);
		it->code->flags &= ~LIONVEC_SPILL;
	}
	if (it->hasloc)
		lionvec_build_put_loc(bs, it->loc);
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
 * The CODE chains, from the sort, sorted: each group's items in heap order,
 * on pages of its own, the groups one after another in one run of blocks -
 * every partition, even an empty one, at least a page, for inserts to
 * append to; then the chain of rows in no partition, then that of the NULL
 * rows and zeros.  heads[] and tails[] get each group's ends, ngroups =
 * nparts + 2 of them.  The partitions' figures are summed as their items
 * come: their items, the largest |x + m| of their rows and the sum of
 * their squared distances from the centroid, and for the rows' own items
 * (not the copies) that sum over every partition.
 */
static void
lionvec_build_write_codes(LionVecBuildState *bs, Tuplesortstate *sort,
						  BlockNumber *heads, BlockNumber *tails)
{
	Relation	index = bs->index;
	uint32		ngroups = (uint32) bs->nparts + 2;
	uint32		group = 0;
	Buffer		buf = InvalidBuffer;
	Page		page = NULL;
	Size		maxlen = LIONVEC_CODE_SIZE(Max(bs->info->dimsp, 64));
	char	   *item = palloc(maxlen);
	TupleTableSlot *slot = MakeSingleTupleTableSlot(lionvec_build_sort_desc(),
													&TTSOpsMinimalTuple);
	int64		last = -1;
	bool		more;

	if (bs->nparts > 0)
	{
		bs->pcount = palloc0(sizeof(uint32) * bs->nparts);
		bs->pmaxnorm = palloc0(sizeof(float) * bs->nparts);
		bs->psumsq = palloc0(sizeof(double) * bs->nparts);
	}
	bs->sumsq = 0.0;
	bs->nsq = 0.0;

	more = tuplesort_gettupleslot(sort, true, false, slot, NULL);
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
		last = -1;

		while (more)
		{
			bool		isnull;
			int32		g = DatumGetInt32(slot_getattr(slot, LIONVEC_SORT_GROUP, &isnull));
			int64		key;
			double		dist2;
			Datum		d;
			Size		len;
			LionVecCode *code = (LionVecCode *) item;

			if (g < 0 || (uint32) g < group)
				elog(ERROR, "lionvec: the build's sort is out of order");
			if ((uint32) g > group)
				break;
			CHECK_FOR_INTERRUPTS();
			key = DatumGetInt64(slot_getattr(slot, LIONVEC_SORT_TID, &isnull));
			if (key < last)
				elog(ERROR, "lionvec: the build's sort is out of order");
			last = key;
			dist2 = DatumGetFloat8(slot_getattr(slot, LIONVEC_SORT_DIST2, &isnull));
			d = slot_getattr(slot, LIONVEC_SORT_ITEM, &isnull);
			len = VARSIZE_ANY_EXHDR(DatumGetPointer(d));
			if (len < LIONVEC_CODE_HDRSZ || len > maxlen)
				elog(ERROR, "lionvec: the build's sort has an item of %zu bytes", len);
			/* aligned, as the TID is read from it */
			memcpy(item, VARDATA_ANY(DatumGetPointer(d)), len);

			if (group < (uint32) bs->nparts)
			{
				/* |x + m|, the row's own norm (a cosine row's 1) */
				double		norm = code->norm;
				float		raw = (float) sqrt(Max(norm * norm + 2.0 * code->xm +
												   bs->mean2, 0.0));

				bs->pcount[group]++;
				bs->psumsq[group] += dist2;
				bs->pmaxnorm[group] = Max(bs->pmaxnorm[group], raw);
				/* the scale (the margin's unit) is of rows about their nearest */
				if ((code->flags & LIONVEC_SPILL) == 0)
				{
					bs->sumsq += dist2;
					bs->nsq += 1.0;
				}
			}

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
			lionvec_note_key(page, &code->tid);
			more = tuplesort_gettupleslot(sort, true, false, slot, NULL);
		}
	}
	MarkBufferDirty(buf);
	UnlockReleaseBuffer(buf);

	/*
	 * The locator (§44.13): kept where the plan said, or where the second
	 * pass met a row in TOAST all the same - whose entries follow the chains'
	 * - written in heap order, then the LOCDIR.
	 */
	bs->keeploc = bs->locmay;
	if (more)
	{
		bool		isnull;
		int32		g = DatumGetInt32(slot_getattr(slot, LIONVEC_SORT_GROUP, &isnull));

		if (g < 0 || (uint32) g != LIONVEC_SORT_LOC_GROUP(bs->nparts))
			elog(ERROR, "lionvec: the build's sort has an item past the last group");
		bs->keeploc = true;
	}
	if (bs->keeploc)
	{
		LionVecLocWriter w;
		Size		locmax = LIONVEC_LOC_SIZE(LIONVEC_LOC_MAXRUNS);
		LionVecLoc *loc = palloc(locmax);

		lionvec_loc_writer_begin(&w, index);
		while (more)
		{
			bool		isnull;
			int32		g = DatumGetInt32(slot_getattr(slot, LIONVEC_SORT_GROUP, &isnull));
			Datum		d;
			Size		len;

			if (g < 0 || (uint32) g != LIONVEC_SORT_LOC_GROUP(bs->nparts))
				elog(ERROR, "lionvec: the build's sort has an item past the last group");
			CHECK_FOR_INTERRUPTS();
			d = slot_getattr(slot, LIONVEC_SORT_ITEM, &isnull);
			len = VARSIZE_ANY_EXHDR(DatumGetPointer(d));
			if (len < LIONVEC_LOC_SIZE(1) || len > locmax)
				elog(ERROR, "lionvec: the build's sort has a locator entry of %zu bytes", len);
			memcpy(loc, VARDATA_ANY(DatumGetPointer(d)), len);
			if (len != LIONVEC_LOC_SIZE(loc->nruns))
				elog(ERROR, "lionvec: the build's sort has a locator entry of %zu bytes", len);
			lionvec_loc_writer_put(&w, loc);
			more = tuplesort_gettupleslot(sort, true, false, slot, NULL);
		}
		bs->nlocs = w.nentries;
		memset(&bs->locmeta, 0, sizeof(bs->locmeta));
		lionvec_loc_writer_end(&w, &bs->locmeta);
		pfree(loc);
	}
	ExecDropSingleTupleTableSlot(slot);
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
		OffsetNumber off;

		CHECK_FOR_INTERRUPTS();
		part->partno = (uint32) j;
		part->head = heads[j];
		part->tail = tails[j];
		part->nrows = bs->pcount[j];
		part->maxnorm = bs->pmaxnorm[j];
		part->spread = bs->pcount[j] > 0 ?
			(float4) (bs->psumsq[j] / bs->pcount[j]) : 0.0f;
		LionVecCentTid(&part->cent.tid, j);
		part->cent.flags = 0;
		part->cent.norm = bs->a.cnorm[j];
		part->cent.alpha1 = bs->a.calpha1[j];
		part->cent.xm = bs->a.cxm[j];
		part->cent.rblk = bs->crblk[j];
		part->cent.roff = bs->croff[j];
		part->cent.popcount = bs->a.cpop[j];
		memcpy(part->cent.bits, bs->a.cbits + (Size) j * nw, sizeof(uint64) * nw);

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
		off = PageAddItemExtended(page, (char *) part, size, InvalidOffsetNumber, 0);
		if (off == InvalidOffsetNumber)
			elog(ERROR, "lionvec: could not add a partition of %zu bytes", size);
		if (bs->ppblk != NULL)
		{
			bs->ppblk[j] = BufferGetBlockNumber(buf);
			bs->ppoff[j] = off;
		}
	}
	if (BufferIsValid(buf))
	{
		MarkBufferDirty(buf);
		UnlockReleaseBuffer(buf);
	}
	pfree(part);
	return head;
}

/*
 * The REGION chain (DESIGN.md §44.2), written after the PART chain, whose
 * items' places it names: each region's number, the run of partitions it
 * holds and where the run's first PART item is, and its centroid coded as a
 * row is, plane 1 alone, which is all an insert routes by.  An empty
 * region, which k-means may leave, is kept, with no run, so that the count
 * stays lionvec_region_count() of the partitions and the beam the same as
 * the build's.  Returns the chain's head.
 */
static BlockNumber
lionvec_build_write_regions(LionVecBuildState *bs)
{
	Relation	index = bs->index;
	const LionVecRegions *r = &bs->regions;
	int			nw = bs->info->dimsp / 64;
	Size		size = LIONVEC_REGION_SIZE(bs->info->dimsp);
	LionVecRegion *reg = palloc0(size);
	BlockNumber head = InvalidBlockNumber;
	Buffer		buf = InvalidBuffer;
	Page		page = NULL;
	int			g;

	Assert(r->n > 0 && bs->ppblk != NULL);
	for (g = 0; g < r->n; g++)
	{
		int			first = r->first[g];
		int			count = r->first[g + 1] - first;

		CHECK_FOR_INTERRUPTS();
		Assert(count >= 0);
		reg->regno = (uint32) g;
		reg->first = (uint32) first;
		reg->nparts = (uint32) count;
		reg->pblk = count > 0 ? bs->ppblk[first] : InvalidBlockNumber;
		reg->poff = count > 0 ? bs->ppoff[first] : InvalidOffsetNumber;
		ItemPointerSetInvalid(&reg->cent.tid);
		reg->cent.flags = 0;
		reg->cent.norm = r->norm[g];
		reg->cent.alpha1 = r->alpha1[g];
		reg->cent.xm = 0.0f;
		reg->cent.rblk = InvalidBlockNumber;
		reg->cent.roff = InvalidOffsetNumber;
		reg->cent.popcount = r->popcount[g];
		memcpy(reg->cent.bits, r->bits + (Size) g * nw, sizeof(uint64) * nw);

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
			lionvec_init_page(page, LIONVEC_PAGE_REGION);
		}
		if (PageAddItemExtended(page, (char *) reg, size, InvalidOffsetNumber, 0) ==
			InvalidOffsetNumber)
			elog(ERROR, "lionvec: could not add a region of %zu bytes", size);
	}
	if (BufferIsValid(buf))
	{
		MarkBufferDirty(buf);
		UnlockReleaseBuffer(buf);
	}
	pfree(reg);
	return head;
}

/* The dimensions the column's type modifier fixes, or 0. */
static int
lionvec_typmod_dims(Relation index, bool half)
{
	int32		typmod = TupleDescAttr(RelationGetDescr(index), 0)->atttypmod;

	if (typmod > 0)
	{
		lionvec_check_max_dims(typmod, half);
		return typmod;
	}
	return 0;
}

/* ----------------------------------------------------------------
 * Parallel build (DESIGN.md §44.12)
 *
 * What a build costs is arithmetic: k-means' iterations and the second
 * pass each find the nearest of the centroids for every row, by a plane-1
 * estimate of each.  The first pass, the mean and the sample, is a fiftieth
 * of the time and stays serial: the sample it draws must be the same
 * whatever order the rows come in.  Then the leader sets up shared memory -
 * the sample, the arrays of LionVecBuildArrays, a parallel heap scan and a
 * shared sort - launches the workers, and posts them work, which the
 * leader takes part in:
 *
 *	KMEANS	each of k-means' iterations: the leader codes the centroids,
 *			regroups them into their regions if there are any
 *			(lionvec_regions_make(), into the shared arrays), and posts the
 *			iteration; every process takes the sample's rows a
 *			LIONVEC_KMEANS_CHUNK at a time, and assigns them
 *			(lionvec_kmeans_assign(), through the regions); the leader waits
 *			for every row, then moves the centroids to their rows' means
 *			(lionvec_kmeans_update(), serially, in the sample's order).  Each
 *			row's assignment depends on the centroids and their regions
 *			alone, which are made as a serial build makes them, so the
 *			centroids come out as a serial build's do, to the bit, however
 *			the rows were shared out.
 *	SCAN	the second pass, over a parallel heap scan: every process puts
 *			each row's CODE items into a sort of its own, and its REFINE
 *			item on pages of its own; the leader merges the sorts and writes
 *			the CODE chains, the PART chain, the REGION chain and the meta
 *			page, as a serial build does.  The leader made the final regions
 *			and renumbered the partitions before posting it.
 *
 * The index a parallel build makes is a serial build's but for where the
 * rows' REFINE items lie, which only their CODE items point at.
 * ---------------------------------------------------------------- */

#define PARALLEL_KEY_LIONVEC_SHARED		UINT64CONST(0xC200000000000001)
#define PARALLEL_KEY_LIONVEC_ARRAYS		UINT64CONST(0xC200000000000002)
#define PARALLEL_KEY_LIONVEC_SAMPLE		UINT64CONST(0xC200000000000003)
#define PARALLEL_KEY_LIONVEC_SORT		UINT64CONST(0xC200000000000004)
#define PARALLEL_KEY_LIONVEC_QUERY		UINT64CONST(0xC200000000000005)
#define PARALLEL_KEY_LIONVEC_WAL		UINT64CONST(0xC200000000000006)
#define PARALLEL_KEY_LIONVEC_BUFFERS	UINT64CONST(0xC200000000000007)

/* the sample's rows a process takes at a time in k-means */
#define LIONVEC_KMEANS_CHUNK	32

/* what the leader posts */
#define LIONVEC_PHASE_NONE		0
#define LIONVEC_PHASE_KMEANS	1
#define LIONVEC_PHASE_SCAN		2

typedef struct LionVecBuildShared
{
	/* Set by the leader before any worker starts. */
	Oid			heaprelid;
	Oid			indexrelid;
	bool		isconcurrent;
	uint64		queryid;
	int			nrequested;		/* workers asked for, plus the leader */
	int			metric;
	bool		half;
	int			dims;
	bool		hasmean;
	int			nparts;
	bool		spill;
	int			sn;				/* the sample's rows */
	bool		lookup;			/* the locator's plan (§44.13) */
	bool		locmay;
	int			reflocruns;

	/* The leader's posts, under mutex; workcv is signalled at each. */
	slock_t		mutex;
	ConditionVariable workcv;
	int			phase;			/* LIONVEC_PHASE_* */
	uint32		generation;		/* one more at each post */

	/*
	 * KMEANS: the next sample row to take, below the generation it is of, so
	 * that a process still in the last iteration takes none of this one's;
	 * the rows assigned and how many moved, under mutex.  donecv is
	 * signalled when the last row is.
	 */
	pg_atomic_uint64 next;
	int			kdone;
	int			kmoved;
	ConditionVariable donecv;

	/* SCAN: each process reports here, under mutex, when its sort is done */
	int			nparticipantsdone;
	double		reltuples;
	double		indtuples;
	bool		brokenhotchain;
	BlockNumber refine_tail;	/* the last REFINE page any of them filled */

	/*
	 * ParallelTableScanDescData follows, BUFFERALIGNed as shm_toc_allocate()
	 * aligns: a table AM's scan descriptor may need that.
	 */
} LionVecBuildShared;

#define ParallelTableScanFromLionVecBuildShared(shared) \
	((ParallelTableScanDesc) ((char *) (shared) + BUFFERALIGN(sizeof(LionVecBuildShared))))

struct LionVecBuildLeader
{
	ParallelContext *pcxt;
	int			nparticipants;	/* workers launched, plus the leader */
	LionVecBuildShared *shared;
	Sharedsort *sharedsort;
	Snapshot	snapshot;
	WalUsage   *walusage;
	BufferUsage *bufferusage;
};

extern PGDLLEXPORT void lionvec_parallel_build_main(dsm_segment *seg,
													shm_toc *toc);

/*
 * Whether the index's expressions and predicate are parallel safe: the
 * check plan_create_index_workers() makes of them before it sizes a build
 * by the heap.  is_parallel_safe() walks them for a function that is not,
 * and needs no more of a planner than this, there being no Params.
 */
static bool
lionvec_build_parallel_safe(Relation index)
{
	PlannerInfo *root = makeNode(PlannerInfo);

	root->glob = makeNode(PlannerGlobal);
	return is_parallel_safe(root, (Node *) RelationGetIndexExpressions(index)) &&
		is_parallel_safe(root, (Node *) RelationGetIndexPredicate(index));
}

/*
 * The workers a build asks for, of nrows rows with a vector:
 * max_parallel_maintenance_workers, or the table's own `parallel_workers`
 * when it sets it, at most max_parallel_maintenance_workers, as for core's
 * builds.  Core also sizes a build by the heap's main fork, which holds no
 * vector of more than about 500 dimensions - pgvector's are stored out of
 * line, in the TOAST table - so that DBpedia's 115,186 rows of 1536
 * dimensions, a main fork of 7.6MB, under min_parallel_table_scan_size,
 * would get none; and by maintenance_work_mem, 32MB a process.  A
 * lionvec build's cost is arithmetic over the rows, whatever their bytes,
 * and what a process holds of maintenance_work_mem is its share of the
 * sort, which spills, the sample being shared: so neither applies.  None
 * below LIONVEC_MIN_AUTO_ROWS rows unless `parallel_workers` is set (one
 * partition by default, and quick), nor for an index whose expressions or
 * predicate are not parallel safe, in a temporary table, which workers
 * cannot read, or outside a postmaster.
 */
static int
lionvec_build_workers(Relation heap, Relation index, double nrows)
{
	int			workers;

	if (!IsUnderPostmaster || IsInParallelMode() ||
		max_parallel_maintenance_workers <= 0 ||
		heap->rd_rel->relpersistence == RELPERSISTENCE_TEMP ||
		!lionvec_build_parallel_safe(index))
		return 0;
	workers = RelationGetParallelWorkers(heap, -1);
	if (workers >= 0)
		return Min(workers, max_parallel_maintenance_workers);
	if (nrows < LIONVEC_MIN_AUTO_ROWS)
		return 0;
	return max_parallel_maintenance_workers;
}

/*
 * A process's share of an iteration of k-means, generation gen: the
 * sample's rows, LIONVEC_KMEANS_CHUNK at a time, until there are none left
 * of this iteration.
 */
static void
lionvec_kmeans_share(LionVecBuildShared *shared, const LionVecSample *s,
					 int k, const float *mean, const float *cent, int *assign,
					 LionVecKmeansWork *w, const LionVecRegions *r, uint32 gen)
{
	for (;;)
	{
		uint64		v = pg_atomic_read_u64(&shared->next);
		uint32		from;
		uint32		to;
		int			moved;
		bool		last;

		for (;;)
		{
			if ((uint32) (v >> 32) != gen || (uint32) v >= (uint32) s->n)
				return;
			if (pg_atomic_compare_exchange_u64(&shared->next, &v,
											   v + LIONVEC_KMEANS_CHUNK))
				break;
		}
		from = (uint32) v;
		to = Min(from + LIONVEC_KMEANS_CHUNK, (uint32) s->n);
		CHECK_FOR_INTERRUPTS();
		moved = lionvec_kmeans_assign(s, (int) from, (int) to, k, mean, cent,
									  assign, w, r);

		SpinLockAcquire(&shared->mutex);
		shared->kdone += (int) (to - from);
		shared->kmoved += moved;
		last = shared->kdone == s->n;
		SpinLockRelease(&shared->mutex);
		if (last)
			ConditionVariableSignal(&shared->donecv);
	}
}

/* The leader: post phase to the workers, a new generation of it. */
static uint32
lionvec_parallel_post(LionVecBuildShared *shared, int phase)
{
	uint32		gen;

	SpinLockAcquire(&shared->mutex);
	gen = ++shared->generation;
	shared->phase = phase;
	if (phase == LIONVEC_PHASE_KMEANS)
	{
		shared->kdone = 0;
		shared->kmoved = 0;
		pg_atomic_write_u64(&shared->next, (uint64) gen << 32);
	}
	SpinLockRelease(&shared->mutex);
	ConditionVariableBroadcast(&shared->workcv);
	return gen;
}

/*
 * A worker: wait for the leader's next post after generation *seen, and
 * return its phase.
 */
static int
lionvec_parallel_wait(LionVecBuildShared *shared, uint32 *seen)
{
	int			phase;

	for (;;)
	{
		SpinLockAcquire(&shared->mutex);
		phase = shared->phase;
		if (shared->generation != *seen)
		{
			*seen = shared->generation;
			SpinLockRelease(&shared->mutex);
			break;
		}
		SpinLockRelease(&shared->mutex);
		ConditionVariableSleep(&shared->workcv,
							   WAIT_EVENT_PARALLEL_CREATE_INDEX_SCAN);
	}
	ConditionVariableCancelSleep();
	return phase;
}

/*
 * The leader's k-means: lionvec_kmeans(), each iteration's rows shared out
 * (lionvec_kmeans_share()) among the workers and the leader, and, given mk,
 * routed through the regions the leader regroups the centroids into at each
 * iteration before posting it.
 */
static int
lionvec_parallel_kmeans(LionVecBuildState *bs, const LionVecSample *s,
						LionVecKmeansWork *w, LionVecRegionMaker *mk)
{
	LionVecBuildShared *shared = bs->leader->shared;
	int			k = bs->nparts;
	bool		unit = bs->metric == LIONVEC_COSINE;
	const float *mean = bs->info->mean;
	LionVecRegions *r = mk != NULL ? &bs->regions : NULL;
	int			made = 0;

	Assert(mk == NULL || bs->regions.n > 0);
	lionvec_kmeans_start(s, k, unit, (uint64) LIONVEC_SEED, bs->a.cent, bs->a.assign);
	while (made < LIONVEC_KMEANS_ITERS)
	{
		uint32		gen;
		int			moved;

		made++;
		/* the centroids' codes, which the candidates are found by */
		lionvec_kmeans_codes(k, s->dimsp, mean, bs->a.cent, w);
		/* ... and their regions, as lionvec_kmeans() makes them */
		if (r != NULL)
			lionvec_regions_make(r, bs->a.cent, unit,
								 LIONVEC_REGION_SEED((uint64) LIONVEC_SEED), mean, mk);
		gen = lionvec_parallel_post(shared, LIONVEC_PHASE_KMEANS);
		lionvec_kmeans_share(shared, s, k, mean, bs->a.cent, bs->a.assign, w, r,
							 gen);
		for (;;)
		{
			bool		done;

			SpinLockAcquire(&shared->mutex);
			done = shared->kdone == s->n;
			moved = shared->kmoved;
			SpinLockRelease(&shared->mutex);
			if (done)
				break;
			ConditionVariableSleep(&shared->donecv,
								   WAIT_EVENT_PARALLEL_CREATE_INDEX_SCAN);
		}
		ConditionVariableCancelSleep();
		/* no row moved: the centroids are already their rows' means */
		if (moved == 0)
			break;
		lionvec_kmeans_update(s, k, unit, bs->a.assign, bs->a.cent, w);
	}
	return made;
}

/*
 * A process's share of the second pass, the leader's included: its share of
 * the heap's rows into a sort of its own, then reported.
 */
static void
lionvec_parallel_rows(LionVecBuildState *bs, LionVecBuildShared *shared,
					  Sharedsort *sharedsort, Relation heap, int memkb,
					  bool progress)
{
	SortCoordinate coordinate = palloc0(sizeof(SortCoordinateData));
	IndexInfo  *indexInfo = BuildIndexInfo(bs->index);
	TableScanDesc scan;
	double		reltuples;

	coordinate->isWorker = true;
	coordinate->nParticipants = -1;
	coordinate->sharedsort = sharedsort;
	lionvec_build_rows_begin(bs, heap, memkb, coordinate);

	indexInfo->ii_Concurrent = shared->isconcurrent;
	scan = lion_table_beginscan_parallel(heap,
										 ParallelTableScanFromLionVecBuildShared(shared));
	reltuples = table_index_build_scan(heap, bs->index, indexInfo, true, progress,
									   lionvec_build_callback, bs, scan);
	lionvec_build_end_refine(bs);
	lionvec_build_rows_end(bs);
	tuplesort_performsort(bs->sort);

	SpinLockAcquire(&shared->mutex);
	shared->nparticipantsdone++;
	shared->reltuples += reltuples;
	shared->indtuples += bs->indtuples;
	if (indexInfo->ii_BrokenHotChain)
		shared->brokenhotchain = true;
	if (BlockNumberIsValid(bs->refine_tail) &&
		(!BlockNumberIsValid(shared->refine_tail) ||
		 bs->refine_tail > shared->refine_tail))
		shared->refine_tail = bs->refine_tail;
	SpinLockRelease(&shared->mutex);
	ConditionVariableSignal(&shared->donecv);

	tuplesort_end(bs->sort);
	bs->sort = NULL;
	ExecDropSingleTupleTableSlot(bs->inslot);
	bs->inslot = NULL;
}

static void
lionvec_end_parallel(LionVecBuildLeader *leader)
{
	int			i;

	WaitForParallelWorkersToFinish(leader->pcxt);
	for (i = 0; i < leader->pcxt->nworkers_launched; i++)
		InstrAccumParallelQuery(&leader->bufferusage[i], &leader->walusage[i]);
	if (LION_IS_MVCC_LIKE(leader->snapshot))
		UnregisterSnapshot(leader->snapshot);
	DestroyParallelContext(leader->pcxt);
	ExitParallelMode();
}

/*
 * Set up a parallel build of request workers once the first pass is done
 * and the partitions known, and launch them: the shared memory, the sample
 * copied into it, and bs's arrays laid out in it.  bs->leader is set only
 * when at least one worker was launched; otherwise everything is undone and
 * the caller builds serially.
 */
static void
lionvec_begin_parallel(LionVecBuildState *bs, Relation heap, bool isconcurrent,
					   int request)
{
	Relation	index = bs->index;
	int			dimsp = bs->info->dimsp;
	LionVecBuildLeader *leader;
	ParallelContext *pcxt;
	Snapshot	snapshot;
	Size		estshared;
	Size		estarrays;
	Size		estsample;
	Size		estsort;
	LionVecBuildShared *shared;
	LionVecBuildArrays a;
	char	   *arrays;
	char	   *sample;
	int			querylen = 0;

	Assert(request > 0 && bs->nparts >= 1 && bs->sn >= bs->nparts);
	EnterParallelMode();
	pcxt = CreateParallelContext("pg_lion", "lionvec_parallel_build_main",
								 request);

	/*
	 * A normal build reads with SnapshotAny and decides what to index itself
	 * (RECENTLY_DEAD rows included); a concurrent one indexes what an MVCC
	 * snapshot sees.
	 */
	snapshot = isconcurrent ? RegisterSnapshot(GetTransactionSnapshot()) :
		SnapshotAny;

	estshared = add_size(BUFFERALIGN(sizeof(LionVecBuildShared)),
						 table_parallelscan_estimate(heap, snapshot));
	estarrays = lionvec_build_arrays(NULL, bs->nparts, bs->sn, dimsp, NULL);
	estsample = add_size(MAXALIGN(mul_size((Size) bs->sn, dimsp)),
						 mul_size((Size) bs->sn, sizeof(float)));
	estsort = tuplesort_estimate_shared(request + 1);
	shm_toc_estimate_chunk(&pcxt->estimator, estshared);
	shm_toc_estimate_chunk(&pcxt->estimator, estarrays);
	shm_toc_estimate_chunk(&pcxt->estimator, estsample);
	shm_toc_estimate_chunk(&pcxt->estimator, estsort);
	shm_toc_estimate_keys(&pcxt->estimator, 4);
	shm_toc_estimate_chunk(&pcxt->estimator,
						   mul_size(sizeof(WalUsage), pcxt->nworkers));
	shm_toc_estimate_chunk(&pcxt->estimator,
						   mul_size(sizeof(BufferUsage), pcxt->nworkers));
	shm_toc_estimate_keys(&pcxt->estimator, 2);
	if (debug_query_string)
	{
		querylen = strlen(debug_query_string);
		shm_toc_estimate_chunk(&pcxt->estimator, querylen + 1);
		shm_toc_estimate_keys(&pcxt->estimator, 1);
	}

	InitializeParallelDSM(pcxt);
	if (pcxt->seg == NULL)
	{
		if (LION_IS_MVCC_LIKE(snapshot))
			UnregisterSnapshot(snapshot);
		DestroyParallelContext(pcxt);
		ExitParallelMode();
		return;
	}

	shared = (LionVecBuildShared *) shm_toc_allocate(pcxt->toc, estshared);
	memset(shared, 0, BUFFERALIGN(sizeof(LionVecBuildShared)));
	shared->heaprelid = RelationGetRelid(heap);
	shared->indexrelid = RelationGetRelid(index);
	shared->isconcurrent = isconcurrent;
	shared->queryid = pgstat_get_my_query_id();
	shared->nrequested = request + 1;
	shared->metric = bs->metric;
	shared->half = bs->half;
	shared->dims = bs->dims;
	shared->hasmean = bs->info->mean != NULL;
	shared->nparts = bs->nparts;
	shared->spill = bs->spill;
	shared->sn = bs->sn;
	shared->lookup = bs->lookup;
	shared->locmay = bs->locmay;
	shared->reflocruns = bs->reflocruns;
	SpinLockInit(&shared->mutex);
	ConditionVariableInit(&shared->workcv);
	ConditionVariableInit(&shared->donecv);
	shared->phase = LIONVEC_PHASE_NONE;
	shared->generation = 0;
	pg_atomic_init_u64(&shared->next, 0);
	shared->refine_tail = InvalidBlockNumber;
	table_parallelscan_initialize(heap,
								  ParallelTableScanFromLionVecBuildShared(shared),
								  snapshot);
	/* start at the first block, as a serial build's scans do */
	ParallelTableScanFromLionVecBuildShared(shared)->phs_syncscan = false;
	shm_toc_insert(pcxt->toc, PARALLEL_KEY_LIONVEC_SHARED, shared);

	/*
	 * The arrays, with the mean, which a worker reads as it starts; the
	 * sample is copied in once workers are known to have been launched, as
	 * no worker reads it before the leader's first post.
	 */
	arrays = shm_toc_allocate(pcxt->toc, estarrays);
	memset(arrays, 0, estarrays);
	(void) lionvec_build_arrays(arrays, bs->nparts, bs->sn, dimsp, &a);
	if (bs->info->mean != NULL)
		memcpy(a.mean, bs->info->mean, sizeof(float) * dimsp);
	shm_toc_insert(pcxt->toc, PARALLEL_KEY_LIONVEC_ARRAYS, arrays);
	sample = shm_toc_allocate(pcxt->toc, estsample);
	shm_toc_insert(pcxt->toc, PARALLEL_KEY_LIONVEC_SAMPLE, sample);

	leader = palloc0(sizeof(LionVecBuildLeader));
	leader->sharedsort = (Sharedsort *) shm_toc_allocate(pcxt->toc, estsort);
	tuplesort_initialize_shared(leader->sharedsort, request + 1, pcxt->seg);
	shm_toc_insert(pcxt->toc, PARALLEL_KEY_LIONVEC_SORT, leader->sharedsort);

	if (debug_query_string)
	{
		char	   *sharedquery = shm_toc_allocate(pcxt->toc, querylen + 1);

		memcpy(sharedquery, debug_query_string, querylen + 1);
		shm_toc_insert(pcxt->toc, PARALLEL_KEY_LIONVEC_QUERY, sharedquery);
	}
	leader->walusage = shm_toc_allocate(pcxt->toc,
										mul_size(sizeof(WalUsage), pcxt->nworkers));
	shm_toc_insert(pcxt->toc, PARALLEL_KEY_LIONVEC_WAL, leader->walusage);
	leader->bufferusage = shm_toc_allocate(pcxt->toc,
										   mul_size(sizeof(BufferUsage), pcxt->nworkers));
	shm_toc_insert(pcxt->toc, PARALLEL_KEY_LIONVEC_BUFFERS, leader->bufferusage);

	LaunchParallelWorkers(pcxt);
	leader->pcxt = pcxt;
	leader->nparticipants = pcxt->nworkers_launched + 1;
	leader->shared = shared;
	leader->snapshot = snapshot;
	if (pcxt->nworkers_launched == 0)
	{
		lionvec_end_parallel(leader);
		pfree(leader);
		return;
	}

	/* the sample, which bs gives up for the copy, and the arrays */
	memcpy(sample, bs->scodes, (Size) bs->sn * dimsp);
	memcpy(sample + MAXALIGN((Size) bs->sn * dimsp), bs->sscale,
		   sizeof(float) * bs->sn);
	pfree(bs->scodes);
	pfree(bs->sscale);
	bs->scodes = (uint8 *) sample;
	bs->sscale = (float *) (sample + MAXALIGN((Size) bs->sn * dimsp));
	bs->a = a;

	/* a worker that failed to start cannot leave the leader waiting */
	WaitForParallelWorkersToAttach(pcxt);
	bs->leader = leader;
}

/*
 * The leader's second pass: its share, then every other process's sort
 * done.  Returns the heap tuples the scan saw, and bs->indtuples the rows
 * indexed; *sort is the sort over them all, to read the items from.
 */
static double
lionvec_parallel_heapscan(LionVecBuildState *bs, Relation heap,
						  bool *brokenhotchain, Tuplesortstate **sort)
{
	LionVecBuildLeader *leader = bs->leader;
	LionVecBuildShared *shared = leader->shared;
	SortCoordinate coordinate;
	double		reltuples;

	(void) lionvec_parallel_post(shared, LIONVEC_PHASE_SCAN);
	lionvec_parallel_rows(bs, shared, leader->sharedsort, heap,
						  maintenance_work_mem / leader->nparticipants, true);
	for (;;)
	{
		SpinLockAcquire(&shared->mutex);
		if (shared->nparticipantsdone == leader->nparticipants)
		{
			reltuples = shared->reltuples;
			bs->indtuples = shared->indtuples;
			bs->refine_tail = shared->refine_tail;
			if (shared->brokenhotchain)
				*brokenhotchain = true;
			SpinLockRelease(&shared->mutex);
			break;
		}
		SpinLockRelease(&shared->mutex);
		ConditionVariableSleep(&shared->donecv,
							   WAIT_EVENT_PARALLEL_CREATE_INDEX_SCAN);
	}
	ConditionVariableCancelSleep();

	coordinate = palloc0(sizeof(SortCoordinateData));
	coordinate->isWorker = false;
	coordinate->nParticipants = leader->nparticipants;
	coordinate->sharedsort = leader->sharedsort;
	*sort = lionvec_build_sort_begin(lionvec_build_sort_desc(),
									 maintenance_work_mem, coordinate);
	return reltuples;
}

/*
 * A parallel worker (PostgreSQL's ParallelWorkerMain() calls it by name):
 * the leader's state, rebuilt from shared memory, and then its posts until
 * the second pass, its last.
 */
void
lionvec_parallel_build_main(dsm_segment *seg, shm_toc *toc)
{
	LionVecBuildShared *shared;
	Sharedsort *sharedsort;
	char	   *arrays;
	char	   *sample;
	LionVecBuildState bs;
	LionVecSample s;
	LionVecKmeansWork w;
	Relation	heap;
	Relation	index;
	LOCKMODE	heaplock;
	LOCKMODE	indexlock;
	WalUsage   *walusage;
	BufferUsage *bufferusage;
	int			dimsp;
	uint32		seen = 0;

	debug_query_string = shm_toc_lookup(toc, PARALLEL_KEY_LIONVEC_QUERY, true);
	pgstat_report_activity(STATE_RUNNING, debug_query_string);

	shared = shm_toc_lookup(toc, PARALLEL_KEY_LIONVEC_SHARED, false);
	arrays = shm_toc_lookup(toc, PARALLEL_KEY_LIONVEC_ARRAYS, false);
	sample = shm_toc_lookup(toc, PARALLEL_KEY_LIONVEC_SAMPLE, false);
	sharedsort = shm_toc_lookup(toc, PARALLEL_KEY_LIONVEC_SORT, false);

	/* the lock modes index.c took for the leader */
	if (!shared->isconcurrent)
	{
		heaplock = ShareLock;
		indexlock = AccessExclusiveLock;
	}
	else
	{
		heaplock = ShareUpdateExclusiveLock;
		indexlock = RowExclusiveLock;
	}

	pgstat_report_query_id(shared->queryid, false);
	heap = table_open(shared->heaprelid, heaplock);
	index = index_open(shared->indexrelid, indexlock);
	tuplesort_attach_shared(sharedsort, seg);
	InstrStartParallelQuery();

	memset(&bs, 0, sizeof(bs));
	bs.index = index;
	bs.metric = shared->metric;
	bs.half = shared->half;
	bs.dims = shared->dims;
	bs.nparts = shared->nparts;
	bs.spill = shared->spill;
	bs.sn = shared->sn;
	bs.lookup = shared->lookup;
	bs.locmay = shared->locmay;
	bs.reflocruns = shared->reflocruns;
	bs.rbuf = InvalidBuffer;
	bs.refine_tail = InvalidBlockNumber;
	bs.buildcxt = CurrentMemoryContext;
	bs.tmpcxt = AllocSetContextCreate(CurrentMemoryContext,
									  "lionvec build row",
									  ALLOCSET_DEFAULT_SIZES);
	dimsp = LIONVEC_PADDED(bs.dims);
	if (bs.dims <= 0 || bs.nparts < 1 || bs.sn < bs.nparts)
		elog(ERROR, "lionvec: a parallel build of %d dimensions, %d partitions and %d sample rows",
			 bs.dims, bs.nparts, bs.sn);
	(void) lionvec_build_arrays(arrays, bs.nparts, bs.sn, dimsp, &bs.a);
	bs.info = lionvec_build_info(bs.metric, bs.half, bs.dims,
								 shared->hasmean ? bs.a.mean : NULL);
	if (bs.info->mean != NULL)
	{
		int			i;

		for (i = 0; i < dimsp; i++)
			bs.mean2 += (double) bs.info->mean[i] * bs.info->mean[i];
	}
	lionvec_build_use_arrays(&bs);

	s.n = bs.sn;
	s.dimsp = dimsp;
	s.codes = (const uint8 *) sample;
	s.scale = (const float *) (sample + MAXALIGN((Size) bs.sn * dimsp));
	lionvec_build_kmeans_work(&bs, &w);

	for (;;)
	{
		int			phase = lionvec_parallel_wait(shared, &seen);

		if (phase == LIONVEC_PHASE_KMEANS)
			lionvec_kmeans_share(shared, &s, bs.nparts, bs.info->mean, bs.a.cent,
								 bs.a.assign, &w,
								 bs.regions.n > 0 ? &bs.regions : NULL, seen);
		else if (phase == LIONVEC_PHASE_SCAN)
		{
			lionvec_parallel_rows(&bs, shared, sharedsort, heap,
								  maintenance_work_mem / shared->nrequested,
								  false);
			break;
		}
		else
			elog(ERROR, "lionvec: a parallel build's phase %d", phase);
	}

	bufferusage = shm_toc_lookup(toc, PARALLEL_KEY_LIONVEC_BUFFERS, false);
	walusage = shm_toc_lookup(toc, PARALLEL_KEY_LIONVEC_WAL, false);
	InstrEndParallelQuery(&bufferusage[ParallelWorkerNumber],
						  &walusage[ParallelWorkerNumber]);

	index_close(index, indexlock);
	table_close(heap, heaplock);
}

/*
 * lionvec refuses a table with row-level security enabled, or a partition of
 * one that has it (DESIGN.md §44.6, "Row-level security"): the policies are a
 * filter no lion index answers and LionOrdered does not apply, so under
 * pg_lion.vector_postfilter's default every query a policy applies to would
 * fail, with a WHERE or without.  lionvecbuild() builds no index on such a
 * table, and the planner fails a query whose order the index could give
 * (lion_ordered.c, lion_ordered_set_rel_pathlist()), the owner's too; any
 * other query of the table is planned as ever.
 */
void
lionvec_refuse_rls(Oid relid, const char *index)
{
	List	   *rels = list_make1_oid(relid);
	ListCell   *lc;

	if (get_rel_relispartition(relid))
		rels = list_concat(rels, get_partition_ancestors(relid));
	foreach(lc, rels)
	{
		Oid			rel = lfirst_oid(lc);
		HeapTuple	tup = SearchSysCache1(RELOID, ObjectIdGetDatum(rel));
		bool		rls;

		if (!HeapTupleIsValid(tup))
			elog(ERROR, "cache lookup failed for relation %u", rel);
		rls = ((Form_pg_class) GETSTRUCT(tup))->relrowsecurity;
		ReleaseSysCache(tup);
		if (!rls)
			continue;
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("lionvec does not support row-level security"),
				 rel == relid ?
				 errdetail("Index \"%s\" is on table \"%s\", which has row-level security enabled.",
						   index, get_rel_name(relid)) :
				 errdetail("Index \"%s\" is on table \"%s\", a partition of \"%s\", which has row-level security enabled.",
						   index, get_rel_name(relid), get_rel_name(rel)),
				 errhint("Disable row-level security on \"%s\", or index its vectors with another access method.",
						 get_rel_name(rel))));
	}
	list_free(rels);
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
	Tuplesortstate *sort;
	instr_time	start;
	instr_time	t;
	double		secs[4] = {0};
	int			nprocs = 1;
	int			kmrows;

	if (RelationGetNumberOfBlocks(index) != 0)
		elog(ERROR, "index \"%s\" already contains data",
			 RelationGetRelationName(index));
	lionvec_refuse_rls(RelationGetRelid(heap), RelationGetRelationName(index));

	memset(&bs, 0, sizeof(bs));
	bs.index = index;
	bs.metric = lionvec_index_metric(index);
	bs.half = lionvec_index_half(index);
	bs.dims = lionvec_typmod_dims(index, bs.half);
	if (bs.half && bs.dims > 0)
		bs.hbuf = palloc(sizeof(float) * bs.dims);
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
	 * pass 1: the mean and the sample.  It does not join a synchronized
	 * scan, which could start part-way through the heap, so that the sample
	 * is the same from build to build; nor does the second, whose items are
	 * sorted back into heap order (by TID) whatever order it reads them in.
	 */
	INSTR_TIME_SET_CURRENT(start);
	table_index_build_scan(heap, index, indexInfo, false, false,
						   lionvec_build_mean_callback, &bs, NULL);

	/* the rotation, and the mean rotated, in this backend's memory */
	if (bs.nsum > 0 && bs.dims > 0)
	{
		int			dimsp = LIONVEC_PADDED(bs.dims);
		float	   *mean = palloc0(sizeof(float) * dimsp);
		int			i;

		for (i = 0; i < bs.dims; i++)
			mean[i] = (float) (bs.sum[i] / bs.nsum);
		bs.info = lionvec_build_info(bs.metric, bs.half, bs.dims, mean);
		pfree(mean);
		lionvec_rotate(&bs.info->rot, bs.info->mean);
		for (i = 0; i < dimsp; i++)
			bs.mean2 += (double) bs.info->mean[i] * bs.info->mean[i];
	}
	else
		bs.info = lionvec_build_info(bs.metric, bs.half, bs.dims, NULL);

	/* whether it keeps a locator, before any REFINE item is written (§44.13) */
	lionvec_build_locator_plan(&bs, heap);
	bs.info->reflocruns = bs.reflocruns;

	memset(&meta, 0, sizeof(meta));
	meta.mean_head = InvalidBlockNumber;
	if (bs.info->mean != NULL)
		meta.mean_head = lionvec_build_write_mean(index, bs.info->mean,
												  bs.info->dimsp);

	/* the partitions, and whether rows spill to a second (two at least) */
	bs.nparts = bs.dims > 0 ? lionvec_partitions(index, bs.nsum, bs.sn) : 0;

	/*
	 * ... and k-means' sample: the rows drawn, cut to what k-means is given
	 * of them (lionvec_kmeans_rows(), DESIGN.md §44.3) before a parallel
	 * build copies it to shared memory
	 */
	bs.sdrawn = bs.sn;
	kmrows = lionvec_kmeans_rows(bs.sn, bs.nparts);
	if (kmrows < bs.sn)
		lionvec_build_subsample(&bs, kmrows);
	bs.spill = bs.nparts >= 2 &&
		(index->rd_options == NULL || ((LionVecOptions *) index->rd_options)->spill);
	INSTR_TIME_SET_CURRENT(t);
	INSTR_TIME_SUBTRACT(t, start);
	secs[0] = INSTR_TIME_GET_DOUBLE(t);
	INSTR_TIME_SET_CURRENT(start);
	if (bs.nparts > 0)
	{
		int			request = lionvec_build_workers(heap, index, bs.nsum);
		LionVecSample sample;

		if (request > 0)
			lionvec_begin_parallel(&bs, heap, indexInfo->ii_Concurrent, request);
		if (bs.leader == NULL && bs.a.cent == NULL)
		{
			/* serial: the same arrays, in this backend's memory */
			Size		size = lionvec_build_arrays(NULL, bs.nparts, bs.sn,
													bs.info->dimsp, NULL);
			char	   *arrays = MemoryContextAllocExtended(bs.buildcxt, size,
															MCXT_ALLOC_HUGE | MCXT_ALLOC_ZERO);

			(void) lionvec_build_arrays(arrays, bs.nparts, bs.sn, bs.info->dimsp,
										&bs.a);
			if (bs.info->mean != NULL)
				memcpy(bs.a.mean, bs.info->mean, sizeof(float) * bs.info->dimsp);
		}
		lionvec_build_use_arrays(&bs);
		sample.n = bs.sn;
		sample.dimsp = bs.info->dimsp;
		sample.codes = bs.scodes;
		sample.scale = bs.sscale;
		lionvec_build_centroids(&bs, &sample);
	}
	/* the sample is not needed now (a parallel build's is shared memory) */
	if (bs.leader == NULL && bs.scodes != NULL)
	{
		pfree(bs.scodes);
		pfree(bs.sscale);
	}
	bs.scodes = NULL;
	bs.sscale = NULL;
	INSTR_TIME_SET_CURRENT(t);
	INSTR_TIME_SUBTRACT(t, start);
	secs[1] = INSTR_TIME_GET_DOUBLE(t);

	/* pass 2: the REFINE items in place, the CODE items to the sort */
	INSTR_TIME_SET_CURRENT(start);
	if (bs.leader != NULL)
		reltuples = lionvec_parallel_heapscan(&bs, heap,
											  &indexInfo->ii_BrokenHotChain,
											  &sort);
	else
	{
		lionvec_build_rows_begin(&bs, heap, maintenance_work_mem, NULL);
		reltuples = table_index_build_scan(heap, index, indexInfo, false, true,
										   lionvec_build_callback, &bs, NULL);
		lionvec_build_end_refine(&bs);
		lionvec_build_rows_end(&bs);
		sort = bs.sort;
		ExecDropSingleTupleTableSlot(bs.inslot);
		bs.inslot = NULL;
	}
	INSTR_TIME_SET_CURRENT(t);
	INSTR_TIME_SUBTRACT(t, start);
	secs[2] = INSTR_TIME_GET_DOUBLE(t);

	INSTR_TIME_SET_CURRENT(start);
	heads = palloc(sizeof(BlockNumber) * (bs.nparts + 2));
	tails = palloc(sizeof(BlockNumber) * (bs.nparts + 2));
	tuplesort_performsort(sort);
	lionvec_build_write_codes(&bs, sort, heads, tails);
	tuplesort_end(sort);
	meta.part_head = InvalidBlockNumber;
	meta.region_head = InvalidBlockNumber;
	meta.nregions = (uint32) bs.regions.n;
	if (bs.nparts > 0)
	{
		if (bs.regions.n > 0)
		{
			bs.ppblk = palloc(sizeof(BlockNumber) * bs.nparts);
			bs.ppoff = palloc(sizeof(OffsetNumber) * bs.nparts);
		}
		meta.part_head = lionvec_build_write_parts(&bs, heads, tails);
		if (bs.regions.n > 0)
			meta.region_head = lionvec_build_write_regions(&bs);
	}
	/* the arrays a parallel build's centroids are in go with its memory */
	if (bs.leader != NULL)
	{
		nprocs = bs.leader->nparticipants;
		lionvec_end_parallel(bs.leader);
		bs.leader = NULL;
		memset(&bs.a, 0, sizeof(bs.a));
	}

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
		m->region_head = meta.region_head;
		m->nregions = meta.nregions;
		if (bs.keeploc)
		{
			m->flags |= LIONVEC_META_LOCATOR;
			m->loc_head = bs.locmeta.loc_head;
			m->loc_dtail = bs.locmeta.loc_dtail;
			m->loc_tail = bs.locmeta.loc_tail;
			m->toastrelid = heap->rd_rel->reltoastrelid;
			m->loc_built = bs.locmeta.loc_built;
			m->loc_bpages = bs.locmeta.loc_bpages;
			m->loc_root = InvalidBlockNumber;
			m->refloc_runs = (uint16) bs.reflocruns;
		}
		else if (bs.reflocruns > 0)
			elog(ERROR, "lionvec: a build that put places in its refine items keeps no locator");
	}
	MarkBufferDirty(metabuf);
	UnlockReleaseBuffer(metabuf);

	if (RelationNeedsWAL(index))
		log_newpage_range(index, MAIN_FORKNUM, 0,
						  RelationGetNumberOfBlocks(index), true);
	INSTR_TIME_SET_CURRENT(t);
	INSTR_TIME_SUBTRACT(t, start);
	secs[3] = INSTR_TIME_GET_DOUBLE(t);
	if (nprocs > 1)
		elog(DEBUG1, "lionvec index \"%s\": built in parallel",
			 RelationGetRelationName(index));
	else
		elog(DEBUG1, "lionvec index \"%s\": built serially",
			 RelationGetRelationName(index));
	if (bs.keeploc)
		elog(DEBUG1, "lionvec index \"%s\": a locator of %.0f rows in TOAST, its refine items %s",
			 RelationGetRelationName(index), bs.nlocs,
			 bs.reflocruns > 0 ? "with their places" : "without places");
	elog(DEBUG2, "lionvec index \"%s\": %.0f rows in %.1f s by %d process%s: the first pass %.1f s, k-means %.1f s, the second pass %.1f s, writing %.1f s",
		 RelationGetRelationName(index), bs.indtuples,
		 secs[0] + secs[1] + secs[2] + secs[3], nprocs, nprocs == 1 ? "" : "es",
		 secs[0], secs[1], secs[2], secs[3]);

	MemoryContextDelete(bs.tmpcxt);

	result = palloc(sizeof(IndexBuildResult));
	result->heap_tuples = reltuples;
	result->index_tuples = bs.indtuples;
	return result;
}

/*
 * An unlogged index's init fork: the meta page and the empty chains of rows
 * in no partition and of NULL rows; and, for an index whose vectors may lie
 * in TOAST (lionvec_build_locator_plan(), by the rule alone: the table is
 * empty when the init fork is used), the locator's first LOC page and its
 * LOCDIR page, whose one item names it, empty.
 */
static void
lionvecbuildempty(Relation index)
{
	int			metric = lionvec_index_metric(index);
	bool		half = lionvec_index_half(index);
	int			dims = lionvec_typmod_dims(index, half);
	LionVecBuildState plan;
	Relation	heap;
	Oid			toastrelid;
	Buffer		metabuf;
	Buffer		rest;
	Buffer		nulls;
	Buffer		loc = InvalidBuffer;
	Buffer		locdir = InvalidBuffer;
	LionVecMeta meta;

	memset(&plan, 0, sizeof(plan));
	plan.index = index;
	plan.dims = dims;
	plan.half = half;
	heap = table_open(index->rd_index->indrelid, AccessShareLock);
	lionvec_build_locator_plan(&plan, heap);
	toastrelid = heap->rd_rel->reltoastrelid;
	table_close(heap, AccessShareLock);

	metabuf = ExtendBufferedRel(BMR_REL(index), INIT_FORKNUM, NULL,
								EB_LOCK_FIRST | EB_SKIP_EXTENSION_LOCK);
	Assert(BufferGetBlockNumber(metabuf) == LIONVEC_METAPAGE);
	rest = ExtendBufferedRel(BMR_REL(index), INIT_FORKNUM, NULL,
							 EB_LOCK_FIRST | EB_SKIP_EXTENSION_LOCK);
	nulls = ExtendBufferedRel(BMR_REL(index), INIT_FORKNUM, NULL,
							  EB_LOCK_FIRST | EB_SKIP_EXTENSION_LOCK);
	if (plan.locmay)
	{
		LionVecLocDir d;

		loc = ExtendBufferedRel(BMR_REL(index), INIT_FORKNUM, NULL,
								EB_LOCK_FIRST | EB_SKIP_EXTENSION_LOCK);
		locdir = ExtendBufferedRel(BMR_REL(index), INIT_FORKNUM, NULL,
								   EB_LOCK_FIRST | EB_SKIP_EXTENSION_LOCK);
		lionvec_init_page(BufferGetPage(loc), LIONVEC_PAGE_LOC);
		lionvec_init_page(BufferGetPage(locdir), LIONVEC_PAGE_LOCDIR);
		d.blk = BufferGetBlockNumber(loc);
		d.lo = InvalidBlockNumber;
		d.hi = 0;
		if (PageAddItemExtended(BufferGetPage(locdir), &d, sizeof(d),
								InvalidOffsetNumber, 0) == InvalidOffsetNumber)
			elog(ERROR, "lionvec: could not add a locator directory item");
	}

	START_CRIT_SECTION();
	lionvec_init_page(BufferGetPage(rest), LIONVEC_PAGE_CODE);
	MarkBufferDirty(rest);
	log_newpage_buffer(rest, true);
	lionvec_init_page(BufferGetPage(nulls), LIONVEC_PAGE_CODE);
	MarkBufferDirty(nulls);
	log_newpage_buffer(nulls, true);
	if (plan.locmay)
	{
		MarkBufferDirty(loc);
		log_newpage_buffer(loc, true);
		MarkBufferDirty(locdir);
		log_newpage_buffer(locdir, true);
	}

	lionvec_init_meta(BufferGetPage(metabuf), metric, dims);
	meta = LionVecPageGetMeta(BufferGetPage(metabuf));
	meta->rest_head = meta->rest_tail = BufferGetBlockNumber(rest);
	meta->null_head = meta->null_tail = BufferGetBlockNumber(nulls);
	if (plan.locmay)
	{
		meta->flags |= LIONVEC_META_LOCATOR;
		meta->loc_head = meta->loc_dtail = BufferGetBlockNumber(locdir);
		meta->loc_tail = BufferGetBlockNumber(loc);
		meta->toastrelid = toastrelid;
		meta->refloc_runs = (uint16) plan.reflocruns;
	}
	MarkBufferDirty(metabuf);
	log_newpage_buffer(metabuf, true);
	END_CRIT_SECTION();

	if (plan.locmay)
	{
		UnlockReleaseBuffer(locdir);
		UnlockReleaseBuffer(loc);
	}
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
					int partno, int dimsp, int reflocruns, Buffer *buf)
{
	Page		page;
	ItemPointerData tid;
	LionVecRefine *ref;

	*buf = ReadBuffer(index, rblk);
	LockBuffer(*buf, BUFFER_LOCK_SHARE);
	page = BufferGetPage(*buf);
	lionvec_check_page(index, page, rblk, LIONVEC_PAGE_REFINE);
	LionVecCentTid(&tid, partno);
	ref = lionvec_refine_at(index, page, rblk, roff, dimsp, reflocruns, &tid);
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
 * A centroid an insert may choose (lionvec_insert_nearest()): what it needs
 * of the centroid's PART item once the PART chain has been read past it.
 */
typedef struct LionVecInsCand
{
	int			partno;
	double		lo;				/* its key's lower bound, for the pruning */
	float		norm;
	BlockNumber rblk;			/* its REFINE item */
	OffsetNumber roff;
	BlockNumber pblk;			/* its PART item */
	OffsetNumber poff;
	BlockNumber tail;			/* its chain's last page */
} LionVecInsCand;

/*
 * An insert's reading of the centroids (lionvec_insert_nearest()): the
 * streaming bounds over those read so far, and the centroids among them that
 * can still be candidates.
 */
typedef struct LionVecInsScan
{
	LionVecCandScan cs;
	LionVecQuery q;
	LionVecInsCand *cands;
	int			nc;
	int			maxc;
} LionVecInsScan;

/*
 * Partition partno's PART item, at (blk, off), to the bounds, and kept if it
 * can still be a candidate - those the threshold has passed since dropped
 * when the array is full, which grows only if that leaves it over half full.
 */
static void
lionvec_insert_consider(LionVecInsScan *is, const LionVecPart *part, int partno,
						BlockNumber blk, OffsetNumber off)
{
	LionVecInsCand *cd;
	double		lo;
	double		hi;

	if (!lionvec_cands_add(&is->cs, &is->q, part->cent.bits, part->cent.popcount,
						   part->cent.norm, part->cent.alpha1, &lo, &hi))
		return;
	if (is->nc == is->maxc)
	{
		double		thr = lionvec_cands_threshold(&is->cs);
		int			kept = 0;
		int			c;

		for (c = 0; c < is->nc; c++)
			if (is->cands[c].lo <= thr)
				is->cands[kept++] = is->cands[c];
		is->nc = kept;
		if (is->nc > is->maxc / 2)
		{
			is->maxc *= 2;
			is->cands = repalloc(is->cands, sizeof(LionVecInsCand) * is->maxc);
		}
	}
	cd = &is->cands[is->nc++];
	cd->partno = partno;
	cd->lo = lo;
	cd->norm = part->cent.norm;
	cd->rblk = part->cent.rblk;
	cd->roff = part->cent.roff;
	cd->pblk = blk;
	cd->poff = off;
	cd->tail = part->tail;
}

/* An index of no regions: every centroid, the whole PART chain in order. */
static void
lionvec_insert_read_parts(Relation index, const LionVecMetaData *meta,
						  LionVecInsScan *is)
{
	int			dimsp = meta->dimsp;
	int			k = (int) meta->nparts;
	BlockNumber blk = meta->part_head;
	BlockNumber nblocks = RelationGetNumberOfBlocks(index);
	uint64		visited = 0;
	int			n = 0;

	while (BlockNumberIsValid(blk))
	{
		Buffer		buf;
		Page		page;
		OffsetNumber off;
		OffsetNumber maxoff;

		CHECK_FOR_INTERRUPTS();
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
			lionvec_insert_consider(is, part, n, blk, off);
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
}

/*
 * The run of count partitions from first, whose first PART item region
 * regno names at (blk, off): read from there along the PART chain, each item
 * checked to be the next partition, which the build wrote in order.
 */
static void
lionvec_insert_read_run(Relation index, const LionVecMetaData *meta,
						LionVecInsScan *is, int regno, int first, int count,
						BlockNumber blk, OffsetNumber off, uint64 *visited,
						BlockNumber *nblocks)
{
	int			dimsp = meta->dimsp;
	int			got = 0;

	Assert(count > 0);
	if (!BlockNumberIsValid(blk) || blk == LIONVEC_METAPAGE || blk >= *nblocks)
	{
		*nblocks = RelationGetNumberOfBlocks(index);
		if (!BlockNumberIsValid(blk) || blk == LIONVEC_METAPAGE || blk >= *nblocks)
			ereport(ERROR,
					(errcode(ERRCODE_INDEX_CORRUPTED),
					 errmsg("lionvec index \"%s\" has region %d's partitions at block %u, past its end",
							RelationGetRelationName(index), regno, blk)));
	}
	while (got < count)
	{
		Buffer		buf;
		Page		page;
		OffsetNumber maxoff;

		CHECK_FOR_INTERRUPTS();
		if (!BlockNumberIsValid(blk))
			ereport(ERROR,
					(errcode(ERRCODE_INDEX_CORRUPTED),
					 errmsg("lionvec index \"%s\" has the PART chain end after %d of region %d's %d partitions",
							RelationGetRelationName(index), got, regno, count)));
		lionvec_chain_step(index, visited, nblocks);
		buf = ReadBuffer(index, blk);
		LockBuffer(buf, BUFFER_LOCK_SHARE);
		page = BufferGetPage(buf);
		lionvec_check_page(index, page, blk, LIONVEC_PAGE_PART);
		maxoff = PageGetMaxOffsetNumber(page);
		if (off < FirstOffsetNumber || off > maxoff)
			ereport(ERROR,
					(errcode(ERRCODE_INDEX_CORRUPTED),
					 errmsg("lionvec index \"%s\" has region %d's partitions at (%u,%u), past the page's items",
							RelationGetRelationName(index), regno, blk, off)));
		for (; off <= maxoff && got < count; off = OffsetNumberNext(off))
		{
			LionVecPart *part = lionvec_part_at(index, page, blk, off, dimsp);

			if (part->partno != (uint32) (first + got))
				ereport(ERROR,
						(errcode(ERRCODE_INDEX_CORRUPTED),
						 errmsg("lionvec index \"%s\" has partition %u where %d of region %d belongs, at (%u,%u)",
								RelationGetRelationName(index), part->partno,
								first + got, regno, blk, off)));
			lionvec_insert_consider(is, part, first + got, blk, off);
			got++;
		}
		blk = LionVecPageGetOpaque(page)->next;
		off = FirstOffsetNumber;
		UnlockReleaseBuffer(buf);
	}
}

/*
 * An index of regions (DESIGN.md §44.3): the REGION chain, every region's
 * centroid's plane 1 keyed against the row, then the partitions of the beam
 * of regions nearest it - and more, nearer first, while they hold fewer
 * than need - region by region in their order (lionvec_regions_pick()), as
 * the build's lionvec_route() takes them.  The regions are checked to number
 * 0 up, their runs to follow on from each other and to cover the
 * partitions, so that a picked run's partitions are its own.
 */
static void
lionvec_insert_read_regions(Relation index, const LionVecMetaData *meta,
							LionVecInsScan *is, int need)
{
	int			dimsp = meta->dimsp;
	int			k = (int) meta->nparts;
	int			n = (int) meta->nregions;
	double	   *key = palloc(sizeof(double) * n);
	int		   *first = palloc(sizeof(int) * (n + 1));
	int		   *order = palloc(sizeof(int) * n);
	int		   *pick = palloc(sizeof(int) * n);
	BlockNumber *pblk = palloc(sizeof(BlockNumber) * n);
	OffsetNumber *poff = palloc(sizeof(OffsetNumber) * n);
	BlockNumber blk = meta->region_head;
	BlockNumber nblocks = RelationGetNumberOfBlocks(index);
	uint64		visited = 0;
	int			got = 0;
	int			total = 0;
	int			np;
	int			t;

	Assert(n > 0 && n <= k);
	while (BlockNumberIsValid(blk))
	{
		Buffer		buf;
		Page		page;
		OffsetNumber off;
		OffsetNumber maxoff;

		CHECK_FOR_INTERRUPTS();
		lionvec_chain_step(index, &visited, &nblocks);
		buf = ReadBuffer(index, blk);
		LockBuffer(buf, BUFFER_LOCK_SHARE);
		page = BufferGetPage(buf);
		lionvec_check_page(index, page, blk, LIONVEC_PAGE_REGION);
		maxoff = PageGetMaxOffsetNumber(page);
		for (off = FirstOffsetNumber; off <= maxoff; off = OffsetNumberNext(off))
		{
			LionVecRegion *reg = lionvec_region_at(index, page, blk, off, dimsp);

			if (got >= n || reg->regno != (uint32) got ||
				reg->first != (uint32) total ||
				reg->nparts > (uint32) (k - total))
				ereport(ERROR,
						(errcode(ERRCODE_INDEX_CORRUPTED),
						 errmsg("lionvec index \"%s\" has region %u of partitions %u to %u where region %d of partitions from %d belongs, at (%u,%u)",
								RelationGetRelationName(index), reg->regno,
								reg->first, reg->first + reg->nparts, got,
								total, blk, off)));
			first[got] = total;
			pblk[got] = reg->pblk;
			poff[got] = reg->poff;
			key[got] = reg->nparts > 0 ?
				lionvec_region_key(&is->q, reg->cent.bits, reg->cent.popcount,
								   reg->cent.norm, reg->cent.alpha1) : INFINITY;
			total += (int) reg->nparts;
			got++;
		}
		blk = LionVecPageGetOpaque(page)->next;
		UnlockReleaseBuffer(buf);
	}
	if (got != n || total != k)
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg("lionvec index \"%s\" has %d regions of %d partitions, and its meta page says %d of %d",
						RelationGetRelationName(index), got, total, n, k)));
	first[n] = total;

	np = lionvec_regions_pick(key, first, n, lionvec_region_beam(n, need), need,
							  order, pick);
	for (t = 0; t < np; t++)
	{
		int			g = pick[t];

		/* a run reads no page twice, so no more pages than the index has */
		visited = 0;
		lionvec_insert_read_run(index, meta, is, g, first[g], first[g + 1] - first[g],
								pblk[g], poff[g], &visited, &nblocks);
	}
	pfree(key);
	pfree(first);
	pfree(order);
	pfree(pick);
	pfree(pblk);
	pfree(poff);
}

/*
 * The partition of a row with a vector (DESIGN.md §44.2): the centroid
 * nearest it, as the build finds it (lionvec_build_nearest()), from the PART
 * chain, or, in an index of regions, the runs of it the beam of regions
 * nearest the row holds, and the candidates' REFINE items.  xc[dimsp] is the
 * row centred.  In an index that spills, *dest2 gets the partition of the
 * row's copy: SOAR's choice, as the build makes it, but of centroids
 * decoded from their 8-bit codes (lionvec_decode8()), the build's floats
 * being gone - each REFINE item read once, its codes kept for that;
 * otherwise its partno is -1.
 *
 * The PART items are read a centroid at a time (lionvec_cands_add()), and
 * only the centroids that can still be candidates are kept, 40 bytes each,
 * pruned as the threshold falls - not every centroid's plane 1 and nine
 * arrays besides, which at 1536 dimensions were 250 bytes a partition, 64 MB
 * a row at LIONVEC_MAX_PARTS partitions.  The candidates and their order are
 * the build's (lion_vec_math.h): the same centroids in the same order.
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
	LionVecInsScan is;
	LionVecInsCand *cands;
	double	   *key;
	int			rank[LIONVEC_SOAR_CANDS + 1];
	uint8	   *ccodes = NULL;	/* spilling: [nc][dimsp], the candidates' codes */
	float	   *calpha8 = NULL;
	float	   *crnorm8 = NULL;
	double		thr;
	int			nc;
	int			nr;
	int			c;
	int			i;

	is.maxc = 64;
	is.nc = 0;
	is.cands = palloc(sizeof(LionVecInsCand) * is.maxc);
	is.q.dimsp = dimsp;
	is.q.qc = palloc(sizeof(float) * dimsp);
	is.q.planes = palloc(sizeof(uint64) * nw * LIONVEC_QBITS);
	lionvec_query_init(&is.q, LIONVEC_L2, NULL, xc, LIONVEC_ASSIGN_EPS);
	lionvec_cands_begin(&is.cs, m);

	/* the centroids' plane 1, keeping those that can still be candidates */
	if (meta->nregions > 0)
		lionvec_insert_read_regions(index, meta, &is, m);
	else
		lionvec_insert_read_parts(index, meta, &is);

	/* the candidates: those within the threshold at the end, in order */
	cands = is.cands;
	thr = lionvec_cands_threshold(&is.cs);
	i = 0;
	for (c = 0; c < is.nc; c++)
		if (cands[c].lo <= thr)
			cands[i++] = cands[c];
	nc = i;

	/* ... ranked by their 8-bit codes */
	key = palloc(sizeof(double) * Max(nc, 1));
	if (spill)
	{
		/* each candidate's codes, for SOAR's decoding of the ranked ones */
		ccodes = MemoryContextAllocHuge(CurrentMemoryContext,
										mul_size((Size) Max(nc, 1), (Size) dimsp));
		calpha8 = palloc(sizeof(float) * Max(nc, 1));
		crnorm8 = palloc(sizeof(float) * Max(nc, 1));
	}
	for (c = 0; c < nc; c++)
	{
		LionVecInsCand *cd = &cands[c];
		Buffer		buf;
		LionVecRefine *ref = lionvec_cent_refine(index, cd->rblk, cd->roff,
												 cd->partno, dimsp,
												 meta->refloc_runs, &buf);
		double		est;
		double		err;

		lionvec_estimate8(&is.q, ref->codes, cd->norm, ref->alpha8, ref->rnorm8,
						  &est, &err);
		if (spill)
		{
			memcpy(ccodes + (Size) c * dimsp, ref->codes, dimsp);
			calpha8[c] = ref->alpha8;
			crnorm8[c] = ref->rnorm8;
		}
		UnlockReleaseBuffer(buf);
		key[c] = (double) cd->norm * cd->norm - 2.0 * est;
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

	{
		LionVecInsCand *cd = &cands[rank[0]];

		dest->partno = cd->partno;
		dest->tail = cd->tail;
		dest->pblk = cd->pblk;
		dest->poff = cd->poff;
	}

	/* the copy's: SOAR's, of the nearest's next (lionvec_build_nearest()) */
	dest2->partno = -1;
	dest2->tail = InvalidBlockNumber;
	dest2->pblk = InvalidBlockNumber;
	dest2->poff = InvalidOffsetNumber;
	if (spill)
	{
		float	   *cf = palloc(sizeof(float) * (Size) nr * dimsp);
		const float *cfs[LIONVEC_SOAR_CANDS];
		LionVecInsCand *cd;
		int			s;

		for (i = 0; i < nr; i++)
		{
			int			c8 = rank[i];

			lionvec_decode8(ccodes + (Size) c8 * dimsp, dimsp, cands[c8].norm,
							calpha8[c8], crnorm8[c8], cf + (Size) i * dimsp);
			if (i > 0)
				cfs[i - 1] = cf + (Size) i * dimsp;
		}
		s = lionvec_soar_second(xc, cf, cfs, nr - 1, dimsp, LIONVEC_SOAR_LAMBDA);
		Assert(s >= 0 && s < nr - 1);
		cd = &cands[rank[s + 1]];
		dest2->partno = cd->partno;
		dest2->tail = cd->tail;
		dest2->pblk = cd->pblk;
		dest2->poff = cd->poff;
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
 * found it through the copy.  In an index that keeps a locator, a row whose
 * vector lies in TOAST then has its entry appended (lionvec_loc_append(),
 * §44.13), last, so that an entry never names a row whose items are not
 * in: a crash before it leaves a row without one, read as it always was.
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
	lionvec_meta_copy(index, BufferGetPage(metabuf), &meta);
	if (it->hasref && meta.dimsp != dimsp)
		elog(ERROR, "lionvec: index \"%s\" changed dimensions under an insert",
			 RelationGetRelationName(index));
	if (meta.refloc_runs != it->reflocruns)
		elog(ERROR, "lionvec: index \"%s\" changed its refine items under an insert",
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
		Size		rsize = it->refsize;
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
		/* the REFINE item is in, and no CODE item points at it yet */
		LION_INJECTION_POINT("lionvec-insert-refined");
	}

	/* the CODE item, and its copy: the same, pointing at the same REFINE item */
	lionvec_append_code(index, metabuf, dimsp, it->code, it->codesize, &dest,
						!it->hasref);
	if (dest2.partno >= 0)
	{
		Assert(it->hasref && dest2.partno != dest.partno);
		/* the row is in its nearest partition, and not yet in its second */
		LION_INJECTION_POINT("lionvec-insert-coded");
		it->code->flags |= LIONVEC_SPILL;
		lionvec_append_code(index, metabuf, dimsp, it->code, it->codesize,
							&dest2, false);
		it->code->flags &= ~LIONVEC_SPILL;
	}
	if (it->hasloc && (meta.flags & LIONVEC_META_LOCATOR) != 0)
	{
		/* the row's items are in, and its locator entry is not yet */
		LION_INJECTION_POINT("lionvec-insert-unlocated");
		lionvec_loc_append(index, metabuf, &meta, it->loc);
	}
	UnlockReleaseBuffer(metabuf);
}

/*
 * An inserted row's locator entry (§44.13), in an index that keeps one: the
 * TOAST pointer of the tuple the heap stored at tid (lionvec_stored_pointer()),
 * its chunks found through the TOAST index, and their bytes the very bytes
 * the index is given, value - which comes back detoasted, for the caller to
 * encode.  A row whose stored vector is not in TOAST as an entry's must be,
 * or whose chunks do not hold exactly value, has none.
 */
static void
lionvec_insert_locate(Relation index, Relation heap, const LionVecInfo *info,
					  ItemPointer tid, Datum *value, LionVecItems *it)
{
	AttrNumber	attno = index->rd_index->indkey.values[0];
	LionVecToast t;
	LionToastPointer ptr;
	struct varlena *v;
	char	   *data;

	it->hasloc = false;
	if (!info->locator || attno <= 0 || heap == NULL ||
		heap->rd_tableam != GetHeapamTableAmRoutine() ||
		heap->rd_rel->reltoastrelid != info->toastrelid ||
		!lionvec_toast_open(&t, heap, info->dims, info->half))
		return;
	if (lionvec_stored_pointer(heap, tid, attno, &ptr) &&
		lionvec_toast_ptr_ok(&t, &ptr))
	{
		v = pg_detoast_datum((struct varlena *) DatumGetPointer(*value));
		*value = PointerGetDatum(v);
		data = palloc(t.extsize);
		if ((int64) VARSIZE(v) == VARHDRSZ + t.extsize &&
			lionvec_toast_locate(&t, &ptr, data, it->loc) &&
			memcmp(data, VARDATA(v), t.extsize) == 0)
		{
			it->loc->tid = *tid;
			it->hasloc = true;
		}
		pfree(data);
	}
	lionvec_toast_close(&t);
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
	Datum		value;

	if (!isnull[0] && info->dims == 0)
	{
		Pointer		tofree;
		int			dim;

		(void) lionvec_datum_floats(values[0], info->half, NULL, 0, &dim,
									&tofree);
		lionvec_set_dims(index, dim, info->half);
		info = lionvec_get_info(index);
	}

	lionvec_items_alloc(&it, info);
	value = values[0];
	if (!isnull[0])
		lionvec_insert_locate(index, heapRel, info, ht_ctid, &value, &it);
	lionvec_items_fill(info, &it, ht_ctid, value, isnull[0]);
	lionvec_items_place(&it, info->dimsp);

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
					   int dimsp, int reflocruns, BufferAccessStrategy strategy)
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
								  reflocruns, &refs[i].tid) == NULL)
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
	int			reflocruns;		/* its REFINE items' places' runs (§44.13) */
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
								   vs->reflocruns, vs->info->strategy);
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

	/*
	 * Lazy VACUUM hands every pass the same stats when its dead TIDs fill
	 * memory more than once; this pass counts the rows left afresh, as
	 * btvacuumscan() does, or reltuples would sum every pass's count
	 */
	stats->num_index_tuples = 0;

	lionvec_read_meta(index, &meta);
	heads = lionvec_chain_heads(index, &meta, &nheads);

	memset(&vs, 0, sizeof(vs));
	vs.info = info;
	vs.stats = stats;
	vs.callback = callback;
	vs.callback_state = callback_state;
	vs.dimsp = meta.dimsp;
	vs.reflocruns = meta.refloc_runs;
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
	lionvec_vacuum_refines(index, vs.refs, vs.nrefs, vs.dimsp, vs.reflocruns,
						   info->strategy);
	pfree(vs.refs);
	pfree(heads);

	/*
	 * ... and the locator's entries of the same rows, before the heap gives
	 * their TIDs to others (§44.13).  Entries are made only once the
	 * dimensions are known, which then never change.
	 */
	if ((meta.flags & LIONVEC_META_LOCATOR) != 0 && meta.dims > 0)
	{
		bool		half = lionvec_index_half(index);
		int			nchunks = lionvec_toast_nchunks(lionvec_toast_extsize(meta.dims, half),
													LION_TOAST_MAX_CHUNK_SIZE);

		lionvec_vacuum_locs(index, &meta, nchunks, callback, callback_state,
							info->strategy);
	}

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
 * How many items of itemsz bytes a lionvec page holds, their line pointers
 * included, as PageAddItem() fills it: 35 CODE items at 1536 dimensions.
 * The planner's count, and a scan's, of the pages the build wrote a chain
 * in (lion_vec_scan.c, lv_probe(); DESIGN.md §44.4, "Prefetch").
 */
int
lionvec_page_items(Size itemsz)
{
	return lionvec_items_per_page(BLCKSZ - SizeOfPageHeaderData -
								  MAXALIGN(sizeof(LionVecPageOpaqueData)),
								  MAXALIGN(itemsz) + sizeof(ItemIdData));
}

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
	ref = MAXALIGN(LIONVEC_REFINE_ITEM_SIZE(dimsp, meta.refloc_runs)) +
		sizeof(ItemIdData);
	/* a locator's entry a row, of one run in the common case (§44.13) */
	if ((meta.flags & LIONVEC_META_LOCATOR) != 0)
		ref += MAXALIGN(LIONVEC_LOC_SIZE(1)) + sizeof(ItemIdData);
	*perpage = lionvec_page_items(LIONVEC_CODE_SIZE(dimsp));
	return Max(1.0, ceil(pages * *items * code / (*items * code + ref)));
}

/*
 * The cost of a scan's first row, which is most of its cost.  Restricted to
 * no more members than lionvec_exact_limit() allows - by the same partition
 * count the scan reads from the meta page, so that the two agree - the
 * members' distances from the heap, every one, a random page each, which
 * is what a cold scan pays (§44.6; priced so, at 1.3 members a partition
 * the plain scan with a Filter, priced low under a selective filter, can
 * come out cheaper than either exact plan, §44.7).  Otherwise every
 * centroid's estimate, then the partitions a scan probes, a random read
 * each and their CODE pages in order, plane 1 of their members, and a
 * batch's refines and exact distances, each a page read and a heap fetch.
 * `members` is how many rows a restriction admits, or the index's rows.
 * *ranking is the centroids' part, which every participant of a parallel
 * scan pays (lion_vec_parallel_cost()); none for the members alone.
 */
static Cost
lionvec_startup_cost(IndexOptInfo *index, double members, double *codepages,
					 Cost *ranking)
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

	if (frac < 1.0 && members <= lionvec_exact_limit(nparts))
	{
		*codepages = 0.0;
		*ranking = 0.0;
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
	*ranking = nparts * LIONVEC_ROW_OPS * cpu_operator_cost;
	return *ranking +
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
	Cost		ranking;

	return lionvec_startup_cost(index, members, &codepages, &ranking);
}

/*
 * The cost of a scan of path's order: its first row's (*startup, of which
 * *ranking is the centroids'), every row's (*total) and the CODE pages it
 * reads (*pages).
 *
 * Every further batch of pg_lion.vector_batch rows costs a share of the
 * first's start-up (LIONVEC_DEEPER), and each row at worst a refine and
 * an exact distance.  The planner prices a LIMIT under a Filter as LIMIT
 * / selectivity rows of this, so a plain scan that has to walk past the
 * rows a selective filter rejects costs more than LionOrdered, which
 * reads the members alone; charged a refine and a fetch alone, a scan
 * that did not pause tied LionOrdered at 1% and ran 7 times slower.
 */
static void
lionvec_order_cost(IndexPath *path, Cost *startup, Cost *total,
				   Cost *ranking, double *pages)
{
	IndexOptInfo *index = path->indexinfo;
	double		deeper;
	double		spc_random;
	double		spc_seq;

	get_tablespace_page_costs(index->reltablespace, &spc_random, &spc_seq);
	*startup = lionvec_startup_cost(index, index->tuples, pages, ranking);
	deeper = lionvec_patience > 0 ? LIONVEC_DEEPER_PAUSED : LIONVEC_DEEPER;
	*total = *startup +
		Max(index->tuples, 1.0) *
		(*startup * deeper / Max(lionvec_batch, 1) +
		 spc_random + cpu_index_tuple_cost);
}

/*
 * costsize.c's get_parallel_divisor(), which is static: the workers, and
 * the leader's share while it takes part - 1 less 0.3 a worker, none from
 * four workers on.
 */
static double
lionvec_parallel_divisor(Path *path)
{
	double		divisor = path->parallel_workers;

	if (parallel_leader_participation)
	{
		double		leader = 1.0 - 0.3 * path->parallel_workers;

		if (leader > 0)
			divisor += leader;
	}
	return Max(divisor, 1.0);
}

/*
 * A partial path's cost (DESIGN.md §44.7, "A parallel scan"): its
 * participants split the partitions (lion_vec_scan.c, lv_claim()), so the
 * probes' share of the start-up and the rows' part after it divide among
 * them; the centroids' ranking each pays in full.  cost_index() calls
 * lionveccostestimate() before it sets parallel_workers and divides only
 * its own CPU part, so lion_ordered_set_rel_pathlist() calls this on the
 * partial paths that come out of it.
 */
void
lion_vec_parallel_cost(IndexPath *path)
{
	Cost		startup;
	Cost		total;
	Cost		ranking;
	Cost		pstartup;
	Cost		ptotal;
	double		pages;
	double		divisor;

	if (path->path.parallel_workers <= 0 || path->indexorderbys == NIL ||
		!lion_vec_index(path->indexinfo))
		return;
	lionvec_order_cost(path, &startup, &total, &ranking, &pages);
	divisor = lionvec_parallel_divisor(&path->path);
	pstartup = ranking + (startup - ranking) / divisor;
	ptotal = pstartup + (total - startup) / divisor;
	path->path.startup_cost -= startup - pstartup;
	path->path.total_cost -= total - ptotal;
}

static void
lionveccostestimate(PlannerInfo *root, IndexPath *path, double loop_count,
					Cost *indexStartupCost, Cost *indexTotalCost,
					Selectivity *indexSelectivity, double *indexCorrelation,
					double *indexPages)
{
	IndexOptInfo *index = path->indexinfo;
	Cost		ranking;

	*indexSelectivity = 1.0;
	*indexCorrelation = 0.0;

	/* nothing to give but an order: never a scan without one */
	if (path->indexorderbys == NIL)
	{
		*indexStartupCost = get_float8_infinity();
		*indexTotalCost = get_float8_infinity();
		*indexPages = index->pages;
#if PG_VERSION_NUM >= 180000

		/*
		 * From 18 the planner weighs disabled nodes before costs, so with
		 * enable_seqscan off a disabled sequential scan lost to this path,
		 * infinite or not, and a count(*) became an index-only scan that
		 * failed ("no data returned for index-only scan").  More disabled
		 * nodes than a disabled scan has keep it out, as pgvector's do.
		 */
		path->path.disabled_nodes += 2;
#endif
		return;
	}

	/*
	 * *indexPages, the CODE pages a scan reads, is what a partial path's
	 * workers are counted from (compute_parallel_worker(), against
	 * min_parallel_index_scan_size), unless the table's parallel_workers
	 * says how many; its cost is lion_vec_parallel_cost()'s.
	 */
	lionvec_order_cost(path, indexStartupCost, indexTotalCost, &ranking,
					   indexPages);
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

	{
		bool		half;

		if (!lionvec_type_ok(opcintype, &half))
		{
			ereport(INFO,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 errmsg("lionvec operator class \"%s\" is over type %s, and lionvec indexes pgvector's vector and halfvec",
							opclassname, format_type_be(opcintype))));
			result = false;
		}
	}

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
			oprform->amopsortfamily == InvalidOid ||
			!lionvec_distance_type_ok(get_op_rettype(oprform->amopopr)))
		{
			ereport(INFO,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 errmsg("lionvec operator class \"%s\" has operator %s of strategy %d, and lionvec takes one ORDER BY operator of strategy 1, 2 or 3 over its input type, returning float8 or float4",
							opclassname, format_operator(oprform->amopopr),
							oprform->amopstrategy)));
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
		/*
		 * each participant runs the whole scan over the partitions it claims
		 * first (lion_vec_scan.c, lv_claim(); DESIGN.md §44.4, "A parallel
		 * scan"), and Gather Merge merges their orders
		 */
		.amcanparallel = true,
#if PG_VERSION_NUM >= 170000
		/*
		 * by lionvec's own count of workers, not the one core then makes
		 * (lionvec_build_workers(), DESIGN.md §44.12), and on 16 as well
		 */
		.amcanbuildparallel = true,
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
		.amestimateparallelscan = lionvecestimateparallelscan,
		.aminitparallelscan = lionvecinitparallelscan,
		.amparallelrescan = lionvecparallelrescan,
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
