/*-------------------------------------------------------------------------
 *
 * lion_exec_store.c
 *		The LionCount node over the window store (DESIGN.md §40, "The custom
 *		shapes"): GROUP BY, count(DISTINCT) and aggregates over stored
 *		columns, from the values the count gathers for the rows it counts.
 *
 * The node counts exactly what it would count for `count(*)` under the same
 * WHERE - the intersection of the WHERE's posting sets, or the sum over every
 * entry of a column when nothing in the WHERE selects rows - and the count
 * engine hands it every row it counts with the values of the gathered
 * columns (LionGather, lion_count.h): from the store for the rows of the
 * pages the visibility map calls all-visible, under the container pins of
 * §9, and from the heap tuple for every other row.  The node hashes them by
 * their GROUP BY columns, under the grouping equality and collation the
 * query's own Agg would have used, and keeps per group its rows and the
 * state of each aggregate; when the count is over it emits one row per group,
 * through the same target-list kinds, HAVING and projection as every other
 * row of the node (lion_emit_keys()).
 *
 * The hash table is held to hash_mem as a HashAggregate's is (nodeAgg.c;
 * DESIGN.md §40, "As built: spilling").  Once its groups and pairs take more,
 * the pass goes on in spill mode, in which no new group and no new
 * (group, value) pair of a count(DISTINCT) enters it: the row of a group it
 * does not hold is written to a batch file chosen by the next bits of the
 * group's hash, for a pass of its own once this pass's groups have gone out,
 * and a value a group it holds has not had before is written to a batch of
 * pairs, which are counted into their groups before those go out.  A batch
 * that does not fit either spills again, a level deeper.
 *
 * Part of the LionCount custom scan: lion_customscan.h describes the node
 * and declares what its files share.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "lion_customscan.h"
#include "lion_store.h"

#include "common/hashfn.h"
#include "lib/hyperloglog.h"
#include "storage/buffile.h"

/*
 * The spill's partitions, chosen as nodeAgg.c's HashAggregate chooses its own
 * (hash_choose_num_partitions()): enough that each batch is likely to fit in
 * hash_mem, half as many again, between 4 and 1024, and never so many that
 * the buffers of the node's open files - a BufFile holds one block - take
 * more than a quarter of hash_mem, the files of the batches still waiting
 * counted in.  The HyperLogLog of each partition's hashes estimates what its
 * batch holds, from which a spill out of that batch takes its partitions.
 */
#define LION_SPILL_PARTITION_FACTOR	1.50
#define LION_SPILL_MIN_PARTITIONS	4
#define LION_SPILL_MAX_PARTITIONS	1024
#define LION_SPILL_BUFFER_SIZE		BLCKSZ
#define LION_SPILL_HLL_BIT_WIDTH	5

/* One aggregate over a gathered column, as the plan describes it. */
typedef struct LionSAggSpec
{
	int			kind;			/* LION_SAGG_* */
	int			col;			/* its column: a position in the gathered ones */
	int			width;			/* a sum's or average's integer argument */
	FmgrInfo	cmp;			/* EXTREME: the aggregate's sort operator */
	Oid			inputcollid;
	int16		typlen;			/* the column's type */
	bool		typbyval;
	FmgrInfo	hash;			/* DISTINCT: the equality's hash function */
	FmgrInfo	eq;				/* ... and the equality */
	Oid			distcoll;
} LionSAggSpec;

/* ... and its state in one group. */
typedef struct LionSAggState
{
#ifdef HAVE_INT128
	int128		sum;
#endif
	int64		n;				/* the rows with a value; DISTINCT: values */
	Datum		ext;
	bool		hasext;
} LionSAggState;

/* One group: its keys, its rows, and every aggregate's state. */
typedef struct LionSGroup
{
	uint32		hash;
	int64		id;				/* its number, which the DISTINCT pairs name */
	int64		count;
	Datum	   *keys;
	bool	   *nulls;
	LionSAggState agg[FLEXIBLE_ARRAY_MEMBER];
} LionSGroup;

typedef struct LionSGroupEnt
{
	LionSGroup *key;
	uint32		hash;
	char		status;
} LionSGroupEnt;

/* A (group, aggregate, value) a count(DISTINCT) has seen. */
typedef struct LionSDistKey
{
	int64		gid;
	int			agg;
	Datum		value;
} LionSDistKey;

typedef struct LionSDistEnt
{
	LionSDistKey key;
	uint32		hash;
	char		status;
} LionSDistEnt;

/*
 * One spill of a pass: the rows of the groups it has no room for, or the
 * pairs it has no room for, each written with its hash to the temporary file
 * of the partition the next bits of the hash choose (hashagg_spill_init()).
 * A partition's file is made at its first record.
 */
typedef struct LionSSpill
{
	bool		pairs;			/* pairs, else rows */
	int			nparts;
	int			shift;			/* a partition is (hash & mask) >> shift */
	uint32		mask;
	BufFile   **files;
	int64	   *ntuples;
	int64	   *nbytes;
	hyperLogLogState *hll;		/* each partition's hashes */
} LionSSpill;

/* A batch: one partition of a spill, rewound, for a pass of its own. */
typedef struct LionSBatch
{
	BufFile    *file;
	int64		ntuples;
	int64		nbytes;
	double		card;			/* its groups or pairs, by its HyperLogLog */
	int			used_bits;		/* the hash bits the partitions above took */
} LionSBatch;

struct LionStoreRun;

static bool lion_sgroup_equal(struct LionStoreRun *run, const LionSGroup *a,
							  const LionSGroup *b);
static bool lion_sdist_equal(struct LionStoreRun *run, LionSDistKey a,
							 LionSDistKey b);

#define SH_PREFIX		lion_sgroup
#define SH_ELEMENT_TYPE	LionSGroupEnt
#define SH_KEY_TYPE		LionSGroup *
#define SH_KEY			key
#define SH_HASH_KEY(tb, key)	((key)->hash)
#define SH_EQUAL(tb, a, b) \
	lion_sgroup_equal((struct LionStoreRun *) (tb)->private_data, a, b)
#define SH_STORE_HASH
#define SH_GET_HASH(tb, a)		((a)->hash)
#define SH_SCOPE		static inline
#define SH_DECLARE
#define SH_DEFINE
#include "lib/simplehash.h"

/*
 * A pair's hash is computed by its inserter (lion_sdist_insert_hash()); the
 * table only ever asks for it again when it grows, from the entry.
 */
#define SH_PREFIX		lion_sdist
#define SH_ELEMENT_TYPE	LionSDistEnt
#define SH_KEY_TYPE		LionSDistKey
#define SH_KEY			key
#define SH_HASH_KEY(tb, key)	(murmurhash64((uint64) (key).gid) ^ (uint32) (key).agg)
#define SH_EQUAL(tb, a, b) \
	lion_sdist_equal((struct LionStoreRun *) (tb)->private_data, a, b)
#define SH_STORE_HASH
#define SH_GET_HASH(tb, a)		((a)->hash)
#define SH_SCOPE		static inline
#define SH_DECLARE
#define SH_DEFINE
#include "lib/simplehash.h"

/*
 * The node's gather and what it has made of it (LionCountScanState.store).
 * Built from the plan when the node begins; the groups are made by a run
 * and emptied by a rescan.
 */
typedef struct LionStoreRun
{
	/* the plan's (LION_PRIV_STORE) */
	Oid			indexoid;		/* a plain table's; a partition's is in its
								 * LionPartState */
	int			ncols;
	AttrNumber *attnos;			/* the gathered columns, in the heap */
	int			ngroup;
	int		   *groupcol;		/* each GROUP BY column's position */
	FmgrInfo   *grouphash;
	FmgrInfo   *groupeq;
	Oid		   *groupcoll;
	int16	   *grouptyplen;
	bool	   *grouptypbyval;
	int			nagg;
	LionSAggSpec *aggs;
	bool		hasdistinct;
	double		estgroups;		/* the planner's estimate of the groups */
	double		estpairs;		/* ... and of the count(DISTINCT) pairs */
	TupleDesc	spilldesc;		/* a spilled row: the gathered columns, as
								 * the parent's types */

	/*
	 * A pass's: the count's, a batch of groups', and the batches of pairs
	 * each of them leaves.  Every group, key, extreme, distinct value and
	 * both hash tables are in cxt, the pairs in its child distcxt, and what
	 * cxt holds is what hash_mem bounds.
	 */
	MemoryContext cxt;			/* the groups, their keys and values */
	MemoryContext distcxt;		/* the pairs and their table */
	Size		maxblock;		/* both contexts' largest block */
	MemoryContext outcxt;		/* the row being emitted */
	lion_sgroup_hash *groups;
	lion_sdist_hash *dist;
	LionSGroup *single;			/* without a GROUP BY: the one group */
	LionSGroup *probe;			/* a row's keys, to look its group up */
	LionSGroup **byid;			/* with a count(DISTINCT): the groups by id */
	int64		byidcap;
	int64		ngroups;
	int64		rows;			/* the rows handed over */
	bool		counted;
	bool		iterating;
	lion_sgroup_iterator iter;
	bool		singledone;

	/* the spill (DESIGN.md §40, "As built: spilling") */
	MemoryContext spillcxt;		/* the files, the batches, the read buffer */
	Size		limit;			/* hash_mem */
	bool		spilling;		/* nothing new enters the tables */
	double		passgroups;		/* the groups the pass is estimated to have */
	double		passpairs;		/* ... and the pairs */
	int			groupbits;		/* the group hash bits its batch took */
	int			pairbits;		/* ... and the pair hash bits */
	LionSSpill *groupspill;
	LionSSpill *pairspill;
	List	   *groupbatches;	/* LionSBatch, a stack */
	List	   *pairbatches;	/* ... the pass's, to count before its groups
								 * go out */
	int			nfiles;			/* files open or reserved by a spill */
	int64		diskbytes;		/* what the open files hold */
	Datum	   *rvalues;		/* a record read back, deformed */
	bool	   *risnull;
	Datum	   *pvalues;		/* a pair to write: NULL but its value */
	bool	   *pisnull;
	char	   *rbuf;			/* ... and the record's tuple */
	Size		rbufsize;

	/* the row being emitted */
	LionSGroup *cur;
	Datum	   *aggval;
	bool	   *aggnull;

	/* what EXPLAIN ANALYZE reports, over every run */
	LionGatherStats gstats;
	int64		totalgroups;
	int64		nbatches;		/* 1, and every batch spilled */
	int64		diskpeak;		/* the most the open files held, bytes */
} LionStoreRun;

static Size
lion_sgroup_size(LionStoreRun *run)
{
	return offsetof(LionSGroup, agg) + sizeof(LionSAggState) * Max(run->nagg, 1);
}

/*
 * What the node's tables take, for the planner's estimate of them
 * (lion_store_hash_bytes()), which says when the node will spill and the
 * path is priced for it: the memory one group, one pair, and one copy of a
 * by-reference value of width bytes take as the node allocates them.
 *
 * A chunk of the node's contexts is an AllocSet's: rounded up to a power of
 * two up to the 8kB past which a chunk has a block of its own, behind a
 * header of 8 bytes.  A hash table's entry is at most 0.9 full and the table doubles,
 * so an entry takes half as much again as itself on average, and so does a
 * group's place in the array by id.
 */
#define LION_STORE_CHUNK_HEADER	8

double
lion_store_chunk_bytes(double size)
{
	if (size <= 0)
		return 0;
	if (size <= ALLOCSET_SEPARATE_THRESHOLD)
		size = (double) pg_nextpower2_32((uint32) Max(ceil(size), 8.0));
	return size + LION_STORE_CHUNK_HEADER;
}

/* ... a group with ngroup GROUP BY columns and naggs aggregates ... */
double
lion_store_group_bytes(int ngroup, int naggs, bool hasdistinct)
{
	double		bytes;

	bytes = lion_store_chunk_bytes(offsetof(LionSGroup, agg) +
								   sizeof(LionSAggState) * Max(naggs, 1));
	if (ngroup > 0)
		bytes += lion_store_chunk_bytes(sizeof(Datum) * ngroup) +
			lion_store_chunk_bytes(sizeof(bool) * ngroup) +
			1.5 * sizeof(LionSGroupEnt);
	if (hasdistinct)
		bytes += 1.5 * sizeof(LionSGroup *);
	return bytes;
}

/* ... and a count(DISTINCT)'s pair, its value's copy copybytes. */
double
lion_store_pair_bytes(double copybytes)
{
	return 1.5 * sizeof(LionSDistEnt) + copybytes;
}

static bool
lion_sgroup_equal(LionStoreRun *run, const LionSGroup *a, const LionSGroup *b)
{
	int			k;

	for (k = 0; k < run->ngroup; k++)
	{
		if (a->nulls[k] != b->nulls[k])
			return false;
		if (a->nulls[k])
			continue;
		if (!DatumGetBool(FunctionCall2Coll(&run->groupeq[k], run->groupcoll[k],
											a->keys[k], b->keys[k])))
			return false;
	}
	return true;
}

static bool
lion_sdist_equal(LionStoreRun *run, LionSDistKey a, LionSDistKey b)
{
	LionSAggSpec *s;

	if (a.gid != b.gid || a.agg != b.agg)
		return false;
	s = &run->aggs[a.agg];
	return DatumGetBool(FunctionCall2Coll(&s->eq, s->distcoll, a.value,
										  b.value));
}

/*
 * The plan's LION_PRIV_STORE member, read into st->store: the index - or,
 * over a partitioned table, each partition's, into its LionPartState, for
 * lion_open_parts() to open with the partition's other relations - the
 * columns, the GROUP BY's equalities and hash functions, the planner's
 * estimates of the groups and pairs, the aggregates.  Nothing is opened: an
 * EXPLAIN without ANALYZE reads only this.
 */
void
lion_store_begin(LionCountScanState *st, CustomScan *cscan, EState *estate)
{
	List	   *m = (List *) list_nth(cscan->custom_private, LION_PRIV_STORE);
	LionStoreRun *run;
	List	   *oids;
	List	   *attnos;
	List	   *groups;
	List	   *est;
	List	   *specs;
	Size		hash_mem;
	int			i;

	st->store = NULL;
	if (m == NIL)
		return;
	if (list_length(m) != 6 || !IsA(linitial(m), OidList))
		elog(ERROR, "LionCount: malformed store gather");
	oids = (List *) linitial(m);
	if (list_length(oids) != Max(st->npart, 1))
		elog(ERROR, "LionCount: malformed store gather");

	/*
	 * The gather is fed by the count of the WHERE, or by the sum over every
	 * row, of the table or of each partition in turn: nothing of the node's
	 * own groups, walks or joins is beside it.
	 */
	if (st->groupattno != 0 || st->groupattno2 != 0 || st->distattno != 0 ||
		st->nwagg != 0 || st->joinclause >= 0 ||
		st->topkn != 0 || st->decode != NULL ||
		(st->hasgroupidx && !st->sumall))
		elog(ERROR, "LionCount: a store gather beside another walk");

	run = (LionStoreRun *) palloc0(sizeof(LionStoreRun));
	run->indexoid = InvalidOid;
	if (st->npart == 0)
		run->indexoid = linitial_oid(oids);
	for (i = 0; i < st->npart; i++)
		st->part[i].storeidxoid = list_nth_oid(oids, i);
	for (i = 0; i < Max(st->npart, 1); i++)
	{
		if (!OidIsValid(list_nth_oid(oids, i)))
			elog(ERROR, "LionCount: malformed store gather");
	}
	attnos = (List *) lsecond(m);
	groups = (List *) lfourth(m);
	est = (List *) list_nth(m, 4);
	specs = (List *) list_nth(m, 5);
	if (list_length(est) != 2)
		elog(ERROR, "LionCount: malformed store gather");
	run->estgroups = Max((double) linitial_int(est), 1.0);
	run->estpairs = Max((double) lsecond_int(est), 0.0);

	run->ncols = list_length(attnos);
	if (run->ncols <= 0 || list_length((List *) lthird(m)) != run->ncols)
		elog(ERROR, "LionCount: malformed store gather");
	run->attnos = (AttrNumber *) palloc(sizeof(AttrNumber) * run->ncols);
	for (i = 0; i < run->ncols; i++)
		run->attnos[i] = (AttrNumber) list_nth_int(attnos, i);

	/*
	 * A row is spilled as a MinimalTuple of the gathered columns, under the
	 * parent's types, which are every partition's (lion_store_count_relation()).
	 */
	run->spilldesc = CreateTemplateTupleDesc(run->ncols);
	for (i = 0; i < run->ncols; i++)
	{
		Oid			typid;
		int32		typmod;
		Oid			collid;

		get_atttypetypmodcoll(st->heapoid, run->attnos[i], &typid, &typmod,
							  &collid);
		TupleDescInitEntry(run->spilldesc, (AttrNumber) (i + 1), NULL, typid,
						   typmod, 0);
	}
	TupleDescFinalize(run->spilldesc);
	run->rvalues = (Datum *) palloc0(sizeof(Datum) * run->ncols);
	run->risnull = (bool *) palloc0(sizeof(bool) * run->ncols);
	run->pvalues = (Datum *) palloc0(sizeof(Datum) * run->ncols);
	run->pisnull = (bool *) palloc(sizeof(bool) * run->ncols);
	memset(run->pisnull, true, sizeof(bool) * run->ncols);

	/* the GROUP BY columns: {position, equality, collation} */
	run->ngroup = list_length(groups);
	run->groupcol = (int *) palloc0(sizeof(int) * Max(run->ngroup, 1));
	run->grouphash = (FmgrInfo *) palloc0(sizeof(FmgrInfo) * Max(run->ngroup, 1));
	run->groupeq = (FmgrInfo *) palloc0(sizeof(FmgrInfo) * Max(run->ngroup, 1));
	run->groupcoll = (Oid *) palloc0(sizeof(Oid) * Max(run->ngroup, 1));
	run->grouptyplen = (int16 *) palloc0(sizeof(int16) * Max(run->ngroup, 1));
	run->grouptypbyval = (bool *) palloc0(sizeof(bool) * Max(run->ngroup, 1));
	for (i = 0; i < run->ngroup; i++)
	{
		List	   *g = (List *) list_nth(groups, i);
		Oid			eqop;
		RegProcedure hashfn;

		if (list_length(g) != 3)
			elog(ERROR, "LionCount: malformed store gather");
		run->groupcol[i] = linitial_int(g);
		eqop = (Oid) lsecond_int(g);
		run->groupcoll[i] = (Oid) lthird_int(g);
		if (run->groupcol[i] < 0 || run->groupcol[i] >= run->ncols)
			elog(ERROR, "LionCount: malformed store gather");
		if (!get_op_hash_functions(eqop, &hashfn, NULL))
			elog(ERROR, "LionCount: no hash function for operator %u", eqop);
		fmgr_info(hashfn, &run->grouphash[i]);
		fmgr_info(get_opcode(eqop), &run->groupeq[i]);
		get_typlenbyval(get_atttype(st->heapoid, run->attnos[run->groupcol[i]]),
						&run->grouptyplen[i], &run->grouptypbyval[i]);
	}

	/*
	 * The aggregates: {kind, position, width, aggregate, input collation,
	 * DISTINCT's equality, its collation}.
	 */
	run->nagg = list_length(specs);
	run->aggs = (LionSAggSpec *) palloc0(sizeof(LionSAggSpec) * Max(run->nagg, 1));
	for (i = 0; i < run->nagg; i++)
	{
		List	   *spec = (List *) list_nth(specs, i);
		LionSAggSpec *a = &run->aggs[i];

		if (list_length(spec) != 7)
			elog(ERROR, "LionCount: malformed store gather");
		a->kind = list_nth_int(spec, 0);
		a->col = list_nth_int(spec, 1);
		a->width = list_nth_int(spec, 2);
		a->inputcollid = (Oid) list_nth_int(spec, 4);
		if (a->col < 0 || a->col >= run->ncols ||
			!(a->kind == LION_SAGG_COUNTCOL || a->kind == LION_SAGG_DISTINCT ||
			  LION_SAGG_IS_WAGG(a->kind)))
			elog(ERROR, "LionCount: malformed store gather");
		get_typlenbyval(get_atttype(st->heapoid, run->attnos[a->col]),
						&a->typlen, &a->typbyval);
#ifndef HAVE_INT128
		if (LION_SAGG_IS_WAGG(a->kind) &&
			LION_SAGG_WAGG_KIND(a->kind) != LION_WAGG_EXTREME)
			elog(ERROR, "LionCount: a sum over stored values without 128-bit integers");
#endif
		if (a->kind == LION_SAGG_WAGG(LION_WAGG_EXTREME))
		{
			Oid			aggfnoid = (Oid) list_nth_int(spec, 3);
			HeapTuple	tup;
			Oid			sortop;

			tup = SearchSysCache1(AGGFNOID, ObjectIdGetDatum(aggfnoid));
			if (!HeapTupleIsValid(tup))
				elog(ERROR, "cache lookup failed for aggregate %u", aggfnoid);
			sortop = ((Form_pg_aggregate) GETSTRUCT(tup))->aggsortop;
			ReleaseSysCache(tup);
			if (!OidIsValid(sortop))
				elog(ERROR, "LionCount: aggregate %u has no sort operator",
					 aggfnoid);
			fmgr_info(get_opcode(sortop), &a->cmp);
		}
		else if (a->kind == LION_SAGG_DISTINCT)
		{
			Oid			eqop = (Oid) list_nth_int(spec, 5);
			RegProcedure hashfn;

			if (!get_op_hash_functions(eqop, &hashfn, NULL))
				elog(ERROR, "LionCount: no hash function for operator %u", eqop);
			fmgr_info(hashfn, &a->hash);
			fmgr_info(get_opcode(eqop), &a->eq);
			a->distcoll = (Oid) list_nth_int(spec, 6);
			run->hasdistinct = true;
		}
	}

	/* the target list's kinds name groups and aggregates that are there */
	for (i = 0; i < st->ntlist; i++)
	{
		int			kind = st->tlkind[i];

		if ((LION_TL_IS_SKEY(kind) && LION_TL_SKEY_NO(kind) >= run->ngroup) ||
			(LION_TL_IS_SAGG(kind) && LION_TL_SAGG_NO(kind) >= run->nagg))
			elog(ERROR, "LionCount: malformed store gather");
	}

	/*
	 * The tables' memory grows from small blocks to blocks of at most a
	 * sixteenth of hash_mem, as a HashAggregate's does under a small work_mem
	 * (hash_create_memory()), so that the check against hash_mem is not made
	 * a block of several times the limit too late, and an empty pass does
	 * not take much of a small one.
	 */
	hash_mem = get_hash_memory_limit();
	run->maxblock = pg_prevpower2_size_t(Max(hash_mem / 16, (Size) 1));
	run->maxblock = Min(run->maxblock, (Size) ALLOCSET_DEFAULT_MAXSIZE);
	run->maxblock = Max(run->maxblock, (Size) ALLOCSET_DEFAULT_INITSIZE);
	run->cxt = AllocSetContextCreate(estate->es_query_cxt,
									 "LionCount store groups",
									 ALLOCSET_SMALL_MINSIZE,
									 ALLOCSET_SMALL_INITSIZE,
									 run->maxblock);
	run->outcxt = AllocSetContextCreate(estate->es_query_cxt,
										"LionCount store row",
										ALLOCSET_SMALL_SIZES);
	run->spillcxt = AllocSetContextCreate(estate->es_query_cxt,
										  "LionCount store spill",
										  ALLOCSET_DEFAULT_SIZES);
	run->aggval = (Datum *) palloc0(sizeof(Datum) * Max(run->nagg, 1));
	run->aggnull = (bool *) palloc0(sizeof(bool) * Max(run->nagg, 1));
	run->nbatches = 1;
	st->store = run;
}

/* A new group with the keys of probe, copied into the run's memory. */
static LionSGroup *
lion_sgroup_new(LionStoreRun *run, const LionSGroup *probe)
{
	MemoryContext oldcxt = MemoryContextSwitchTo(run->cxt);
	LionSGroup *g = (LionSGroup *) palloc0(lion_sgroup_size(run));
	int			k;

	g->hash = probe != NULL ? probe->hash : 0;
	g->id = run->ngroups++;
	if (run->ngroup > 0)
	{
		g->keys = (Datum *) palloc(sizeof(Datum) * run->ngroup);
		g->nulls = (bool *) palloc(sizeof(bool) * run->ngroup);
		for (k = 0; k < run->ngroup; k++)
		{
			g->nulls[k] = probe->nulls[k];
			g->keys[k] = probe->nulls[k] ? (Datum) 0 :
				datumCopy(probe->keys[k], run->grouptypbyval[k],
						  run->grouptyplen[k]);
		}
	}

	/*
	 * A batch of pairs names its group by id (lion_store_pair_batch()), so
	 * with a count(DISTINCT) the pass keeps its groups by id as well.
	 */
	if (run->hasdistinct)
	{
		if (g->id >= run->byidcap)
		{
			int64		cap = Max(run->byidcap * 2, 256);

			if (run->byid == NULL)
				run->byid = (LionSGroup **)
					MemoryContextAllocHuge(run->cxt, sizeof(LionSGroup *) * cap);
			else
				run->byid = (LionSGroup **)
					repalloc_huge(run->byid, sizeof(LionSGroup *) * cap);
			run->byidcap = cap;
		}
		run->byid[g->id] = g;
	}
	MemoryContextSwitchTo(oldcxt);
	return g;
}

/*
 * The hash of a row's GROUP BY keys, combined as execGrouping.c combines a
 * grouping's columns: rotate, then xor in the next column's - a NULL adding
 * nothing - with the column's own hash function under its collation, so that
 * the keys its equality calls equal fall together.
 */
static uint32
lion_store_group_hash(LionStoreRun *run, const Datum *values,
					  const bool *isnull)
{
	uint32		hash = 0;
	int			k;

	for (k = 0; k < run->ngroup; k++)
	{
		int			c = run->groupcol[k];

		hash = pg_rotate_left32(hash, 1);
		if (!isnull[c])
			hash ^= DatumGetUInt32(FunctionCall1Coll(&run->grouphash[k],
													 run->groupcoll[k],
													 values[c]));
	}
	return murmurhash32(hash);
}

/* ... and of the pair of value v of aggregate agg in group gid. */
static uint32
lion_store_pair_hash(LionStoreRun *run, int64 gid, int agg, Datum v)
{
	LionSAggSpec *a = &run->aggs[agg];
	uint32		h;

	h = DatumGetUInt32(FunctionCall1Coll(&a->hash, a->distcoll, v));
	return hash_combine(murmurhash32((uint32) gid ^ (uint32) (gid >> 32)),
						hash_combine((uint32) agg, h));
}

/*
 * After a new pair has gone in: past hash_mem, the rest of the pass spills
 * (hash_agg_check_limits()).  The memory is what the tables' contexts have
 * allocated - the groups, their keys and extremes, the distinct values and
 * both hash tables - and the check comes after the entry is in, so that a
 * pass always keeps one, and gets somewhere, however little room it has.
 */
static inline void
lion_store_check_pair(LionStoreRun *run)
{
	if (MemoryContextMemAllocated(run->cxt, true) > run->limit)
		run->spilling = true;
}

/*
 * ... and after a new group, the same; and with a count(DISTINCT), once the
 * groups alone - cxt without its child, the pairs' - take half of hash_mem.
 * The pairs that come for the groups a pass holds after it has begun to
 * spill are counted from their batches beside those groups, in what the
 * groups leave of hash_mem (lion_store_pair_batch()): groups that had filled
 * it would leave a batch of pairs no room, and it would split again and
 * again, a level deeper each time, to count a pair or two.
 */
static inline void
lion_store_check_group(LionStoreRun *run)
{
	if (MemoryContextMemAllocated(run->cxt, true) > run->limit ||
		(run->hasdistinct &&
		 MemoryContextMemAllocated(run->cxt, false) > run->limit / 2))
		run->spilling = true;
}

/*
 * How many partitions a spill takes of input entries of entrysize bytes, the
 * first used_bits of whose hashes the spills above it have taken, as
 * hash_choose_num_partitions() decides it: enough that each batch is likely
 * to fit in hash_mem, LION_SPILL_PARTITION_FACTOR times over, between the
 * minimum and the maximum, and no more than keep the buffers of every file
 * the node may have open - the batches still waiting, and the one being read,
 * included - within a quarter of hash_mem; a power of two, of the bits the
 * hash has left.  *bits is its log2.
 */
static int
lion_spill_num_partitions(LionStoreRun *run, double input, double entrysize,
						  int used_bits, int *bits)
{
	double		hash_mem = (double) get_hash_memory_limit();
	double		limit;
	double		dpartitions;
	int			npartitions;
	int			partition_bits;

	limit = (hash_mem * 0.25 -
			 (double) (run->nfiles + 1) * LION_SPILL_BUFFER_SIZE) /
		LION_SPILL_BUFFER_SIZE;
	dpartitions = 1 + LION_SPILL_PARTITION_FACTOR * input * entrysize / hash_mem;
	if (dpartitions > limit)
		dpartitions = limit;
	if (dpartitions < LION_SPILL_MIN_PARTITIONS)
		dpartitions = LION_SPILL_MIN_PARTITIONS;
	if (dpartitions > LION_SPILL_MAX_PARTITIONS)
		dpartitions = LION_SPILL_MAX_PARTITIONS;
	npartitions = (int) dpartitions;

	partition_bits = pg_ceil_log2_32((uint32) npartitions);
	if (partition_bits + used_bits >= 32)
		partition_bits = 32 - used_bits;
	*bits = partition_bits;
	return 1 << partition_bits;
}

/*
 * A spill of the pass - of rows, or of pairs when `pairs` - into the
 * partitions the next bits of their hashes choose, input entries estimated
 * at entrysize bytes each (hashagg_spill_init()).  Every partition counts
 * as an open file from here on, though its file is made at its first record.
 */
static LionSSpill *
lion_spill_init(LionStoreRun *run, bool pairs, int used_bits, double input,
				double entrysize)
{
	MemoryContext oldcxt = MemoryContextSwitchTo(run->spillcxt);
	LionSSpill *sp = (LionSSpill *) palloc0(sizeof(LionSSpill));
	int			bits;
	int			p;

	sp->pairs = pairs;
	sp->nparts = lion_spill_num_partitions(run, input, entrysize, used_bits,
										   &bits);
	sp->files = (BufFile **) palloc0(sizeof(BufFile *) * sp->nparts);
	sp->ntuples = (int64 *) palloc0(sizeof(int64) * sp->nparts);
	sp->nbytes = (int64 *) palloc0(sizeof(int64) * sp->nparts);
	sp->hll = (hyperLogLogState *) palloc0(sizeof(hyperLogLogState) *
										   sp->nparts);
	for (p = 0; p < sp->nparts; p++)
		initHyperLogLog(&sp->hll[p], LION_SPILL_HLL_BIT_WIDTH);
	sp->shift = 32 - used_bits - bits;
	sp->mask = (sp->shift < 32) ? (uint32) (sp->nparts - 1) << sp->shift : 0;
	run->nfiles += sp->nparts;
	MemoryContextSwitchTo(oldcxt);
	return sp;
}

/*
 * One record onto the partition of sp its hash chooses: the hash, a pair's
 * group id and aggregate, and the tuple (hashagg_spill_tuple()).
 */
static void
lion_spill_write(LionStoreRun *run, LionSSpill *sp, uint32 hash, int64 gid,
				 int32 agg, MinimalTuple tup)
{
	int			p = (sp->shift < 32) ? (int) ((hash & sp->mask) >> sp->shift) : 0;
	BufFile    *file = sp->files[p];
	int64		n = sizeof(uint32) + tup->t_len;

	if (file == NULL)
	{
		MemoryContext oldcxt = MemoryContextSwitchTo(run->spillcxt);

		file = sp->files[p] = BufFileCreateTemp(false);
		MemoryContextSwitchTo(oldcxt);
	}
	BufFileWrite(file, &hash, sizeof(uint32));
	if (sp->pairs)
	{
		BufFileWrite(file, &gid, sizeof(int64));
		BufFileWrite(file, &agg, sizeof(int32));
		n += sizeof(int64) + sizeof(int32);
	}
	BufFileWrite(file, tup, tup->t_len);
	sp->ntuples[p]++;
	sp->nbytes[p] += n;

	/*
	 * The disk the spill takes, at its peak, as nodeAgg.c's hash_disk_used.
	 * The files are only ever appended to, so what was written to them is
	 * their size; BufFileSize() would say the same once the last buffer is
	 * out, but asserts, before 18, that its file is a FileSet's.
	 */
	run->diskbytes += n;
	run->diskpeak = Max(run->diskpeak, run->diskbytes);

	/* hashed again, as nodeAgg.c does: a partition's hashes share bits */
	addHyperLogLog(&sp->hll[p], hash_bytes_uint32(hash));
}

/*
 * The input of a spill is over: each partition that holds anything, rewound -
 * which writes out its buffer - becomes a batch on *batches, its hashes
 * having used the bits this spill took besides those above it
 * (hashagg_spill_finish()).  An empty one's file was never made.
 */
static void
lion_spill_finish(LionStoreRun *run, LionSSpill *sp, List **batches)
{
	MemoryContext oldcxt = MemoryContextSwitchTo(run->spillcxt);
	int			p;

	for (p = 0; p < sp->nparts; p++)
	{
		LionSBatch *b;

		if (sp->files[p] == NULL)
		{
			run->nfiles--;
			freeHyperLogLog(&sp->hll[p]);
			continue;
		}
		if (BufFileSeek(sp->files[p], 0, 0, SEEK_SET) != 0)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not rewind the temporary file of a lion index count")));
		b = (LionSBatch *) palloc(sizeof(LionSBatch));
		b->file = sp->files[p];
		b->ntuples = sp->ntuples[p];
		b->nbytes = sp->nbytes[p];
		b->card = estimateHyperLogLog(&sp->hll[p]);
		b->used_bits = 32 - sp->shift;
		freeHyperLogLog(&sp->hll[p]);
		sp->files[p] = NULL;
		*batches = lappend(*batches, b);
		run->nbatches++;
	}
	pfree(sp->files);
	pfree(sp->ntuples);
	pfree(sp->nbytes);
	pfree(sp->hll);
	pfree(sp);
	MemoryContextSwitchTo(oldcxt);
}

/*
 * The next record of batch b: its hash, a pair's group id and aggregate, and
 * its tuple deformed into run->rvalues and run->risnull, whose by-reference
 * values point into the read buffer until the next (hashagg_batch_read()).
 */
static void
lion_batch_read(LionStoreRun *run, LionSBatch *b, bool pairs, uint32 *hash,
				int64 *gid, int32 *agg)
{
	uint32		t_len;
	MinimalTuple tup;
	HeapTupleData htup;

	BufFileReadExact(b->file, hash, sizeof(uint32));
	if (pairs)
	{
		BufFileReadExact(b->file, gid, sizeof(int64));
		BufFileReadExact(b->file, agg, sizeof(int32));
	}
	BufFileReadExact(b->file, &t_len, sizeof(uint32));
	if (t_len < SizeofMinimalTupleHeader || t_len > MaxAllocSize)
		elog(ERROR, "LionCount: a damaged batch of the store's spill");
	if (t_len > run->rbufsize)
	{
		if (run->rbuf != NULL)
			pfree(run->rbuf);
		run->rbufsize = Max((Size) t_len, (Size) 1024);
		run->rbuf = MemoryContextAlloc(run->spillcxt, run->rbufsize);
	}
	tup = (MinimalTuple) run->rbuf;
	tup->t_len = t_len;
	BufFileReadExact(b->file, (char *) tup + sizeof(uint32),
					 t_len - sizeof(uint32));

	/* deformed as a slot deforms a minimal tuple (tts_minimal_store_tuple()) */
	htup.t_len = t_len + MINIMAL_TUPLE_OFFSET;
	htup.t_data = (HeapTupleHeader) ((char *) tup - MINIMAL_TUPLE_OFFSET);
	ItemPointerSetInvalid(&htup.t_self);
	htup.t_tableOid = InvalidOid;
	heap_deform_tuple(&htup, run->spilldesc, run->rvalues, run->risnull);
}

/* A batch read to its end: its file goes, and the disk it took. */
static void
lion_batch_close(LionStoreRun *run, LionSBatch *b)
{
	BufFileClose(b->file);
	run->nfiles--;
	run->diskbytes -= b->nbytes;
	pfree(b);
}

/*
 * A row of a group the pass does not hold, in spill mode: onto the groups'
 * spill, as the tuple of its gathered values, for a later pass.  The
 * partitions are estimated from the pass's groups and the memory a group
 * has taken so far, its pairs included.
 */
static void
lion_store_spill_row(LionStoreRun *run, uint32 hash, const Datum *values,
					 const bool *isnull)
{
	MinimalTuple tup;

	if (run->groupspill == NULL)
		run->groupspill =
			lion_spill_init(run, false, run->groupbits, run->passgroups,
							(double) MemoryContextMemAllocated(run->cxt, true) /
							(double) Max(run->ngroups, 1));
	tup = lion_form_minimal_tuple(run->spilldesc, values, isnull);
	lion_spill_write(run, run->groupspill, hash, 0, 0, tup);
	pfree(tup);
}

/*
 * A pair whose group the pass holds but whose value the table has not had,
 * in spill mode: onto the pairs' spill, its value alone in a tuple of the
 * gathered columns, the others NULL.
 */
static void
lion_store_spill_pair(LionStoreRun *run, uint32 hash, int64 gid, int agg,
					  Datum v)
{
	int			col = run->aggs[agg].col;
	MinimalTuple tup;

	if (run->pairspill == NULL)
	{
		double		npairs = (double) run->dist->members;

		run->pairspill =
			lion_spill_init(run, true, run->pairbits, run->passpairs,
							(npairs > 0) ?
							(double) MemoryContextMemAllocated(run->distcxt, true) /
							npairs : 2.0 * sizeof(LionSDistEnt));
	}
	run->pvalues[col] = v;
	run->pisnull[col] = false;
	tup = lion_form_minimal_tuple(run->spilldesc, run->pvalues, run->pisnull);
	run->pisnull[col] = true;
	run->pvalues[col] = (Datum) 0;
	lion_spill_write(run, run->pairspill, hash, gid, (int32) agg, tup);
	pfree(tup);
}

/*
 * Value v of count(DISTINCT) agg in a row of group g, hash the pair's: one
 * more distinct value for the group when its table has not had it.  In
 * spill mode nothing new goes in, and a value the table has not had is
 * spilled for a batch of pairs (lion_store_pass_end()).
 */
static void
lion_store_add_pair(LionStoreRun *run, LionSGroup *g, int agg, Datum v,
					uint32 hash)
{
	LionSAggSpec *a = &run->aggs[agg];
	LionSDistKey key;

	key.gid = g->id;
	key.agg = agg;
	key.value = v;
	if (!run->spilling)
	{
		bool		found;
		LionSDistEnt *ent = lion_sdist_insert_hash(run->dist, key, hash,
												   &found);

		if (!found)
		{
			MemoryContext oldcxt = MemoryContextSwitchTo(run->distcxt);

			ent->key.value = datumCopy(v, a->typbyval, a->typlen);
			MemoryContextSwitchTo(oldcxt);
			g->agg[agg].n++;
			lion_store_check_pair(run);
		}
	}
	else if (lion_sdist_lookup_hash(run->dist, key, hash) == NULL)
		lion_store_spill_pair(run, hash, g->id, agg, v);
}

/*
 * A row of the pass, hash its GROUP BY keys': the count's (lion_store_row()),
 * or one a batch of groups gives back under the hash it was spilled with
 * (lion_store_group_batch()).  Its group's rows and aggregates - or, in spill
 * mode, when the table does not hold its group, the row onto the groups'
 * spill.  The values are the caller's until this returns; what a group keeps
 * is copied.
 */
static void
lion_store_add(LionStoreRun *run, const Datum *values, const bool *isnull,
			   uint32 hash)
{
	LionSGroup *g;
	int			i;

	if (run->ngroup == 0)
		g = run->single;
	else
	{
		LionSGroup *probe = run->probe;
		LionSGroupEnt *ent;
		int			k;

		for (k = 0; k < run->ngroup; k++)
		{
			probe->nulls[k] = isnull[run->groupcol[k]];
			probe->keys[k] = values[run->groupcol[k]];
		}
		probe->hash = hash;
		if (!run->spilling)
		{
			bool		found;

			ent = lion_sgroup_insert_hash(run->groups, probe, hash, &found);
			if (!found)
			{
				ent->key = lion_sgroup_new(run, probe);
				lion_store_check_group(run);
			}
		}
		else
		{
			ent = lion_sgroup_lookup_hash(run->groups, probe, hash);
			if (ent == NULL)
			{
				lion_store_spill_row(run, hash, values, isnull);
				return;
			}
		}
		g = ent->key;
	}

	g->count++;
	for (i = 0; i < run->nagg; i++)
	{
		LionSAggSpec *a = &run->aggs[i];
		LionSAggState *s = &g->agg[i];
		Datum		v = values[a->col];

		if (isnull[a->col])
			continue;			/* every one of them skips a NULL */
		switch (a->kind)
		{
			case LION_SAGG_COUNTCOL:
				s->n++;
				break;
			case LION_SAGG_DISTINCT:
				lion_store_add_pair(run, g, i, v,
									lion_store_pair_hash(run, g->id, i, v));
				break;
#ifdef HAVE_INT128
			case LION_SAGG_WAGG(LION_WAGG_SUM):
			case LION_SAGG_WAGG(LION_WAGG_SUM8):
			case LION_SAGG_WAGG(LION_WAGG_AVG):
			case LION_SAGG_WAGG(LION_WAGG_AVG8):
				s->sum += (a->width == 2) ? (int128) DatumGetInt16(v) :
					(a->width == 4) ? (int128) DatumGetInt32(v) :
					(int128) DatumGetInt64(v);
				s->n++;
				break;
#endif
			case LION_SAGG_WAGG(LION_WAGG_EXTREME):
				if (!s->hasext ||
					DatumGetBool(FunctionCall2Coll(&a->cmp, a->inputcollid, v,
												   s->ext)))
				{
					MemoryContext oldcxt = MemoryContextSwitchTo(run->cxt);

					if (s->hasext && !a->typbyval)
						pfree(DatumGetPointer(s->ext));
					s->ext = datumCopy(v, a->typbyval, a->typlen);
					s->hasext = true;
					MemoryContextSwitchTo(oldcxt);
				}
				break;
			default:
				elog(ERROR, "LionCount: stored aggregate kind %d", a->kind);
		}
	}
}

/*
 * One row the count counts, with the values of the gathered columns
 * (LionGatherRowFn).  It is counted here, once, whichever pass forms its
 * group: lion_store_count_relation() checks the rows handed over against
 * the rows counted, and a batch's rows are not handed over again.
 */
static void
lion_store_row(void *arg, const Datum *values, const bool *isnull)
{
	LionStoreRun *run = (LionStoreRun *) arg;

	run->rows++;
	lion_store_add(run, values, isnull,
				   (run->ngroup > 0) ?
				   lion_store_group_hash(run, values, isnull) : 0);
}

/*
 * The entries a pass's hash table is made for: 256, or fewer under a small
 * hash_mem, of which an empty table of 256 pairs would take a quarter.  They
 * grow as they fill.
 */
static uint32
lion_store_table_size(LionStoreRun *run)
{
	return (uint32) Max(Min(run->limit / 1024, (Size) 256), (Size) 16);
}

/*
 * A pass begins - the count's, or a batch of groups' - with its tables made
 * afresh and nothing spilled: its groups estimated at ngroups and its pairs
 * at npairs, for the partitions of a spill out of it, the first used_bits of
 * whose group hashes the batch it reads has used.
 */
static void
lion_store_pass_begin(LionStoreRun *run, double ngroups, double npairs,
					  int used_bits)
{
	Assert(run->groupspill == NULL && run->pairspill == NULL &&
		   run->pairbatches == NIL);
	MemoryContextReset(run->cxt);
	run->distcxt = AllocSetContextCreate(run->cxt, "LionCount store pairs",
										 ALLOCSET_SMALL_MINSIZE,
										 ALLOCSET_SMALL_INITSIZE,
										 run->maxblock);
	run->groups = NULL;
	run->dist = NULL;
	run->byid = NULL;
	run->byidcap = 0;
	run->ngroups = 0;
	run->limit = get_hash_memory_limit();
	run->spilling = false;
	run->passgroups = ngroups;
	run->passpairs = npairs;
	run->groupbits = used_bits;
	run->pairbits = 0;
	if (run->ngroup > 0)
	{
		run->groups = lion_sgroup_create(run->cxt, lion_store_table_size(run),
										 run);
		run->probe = (LionSGroup *) MemoryContextAllocZero(run->cxt,
														   lion_sgroup_size(run));
		run->probe->keys = (Datum *) MemoryContextAllocZero(run->cxt,
															sizeof(Datum) * run->ngroup);
		run->probe->nulls = (bool *) MemoryContextAllocZero(run->cxt,
															sizeof(bool) * run->ngroup);
		run->single = NULL;
	}
	else
		run->single = lion_sgroup_new(run, NULL);
	if (run->hasdistinct)
		run->dist = lion_sdist_create(run->distcxt,
									  lion_store_table_size(run), run);
	run->iterating = false;
}

/*
 * The next batch of the pass's pairs, the deepest first: each pair counted
 * into its group - which the pass still holds, found by its id - when this
 * batch's table has not had it, and, should the table pass hash_mem beside
 * the groups, every pair it has not had spilled a level deeper, by the next
 * bits of the pair's hash.
 */
static void
lion_store_pair_batch(LionStoreRun *run)
{
	LionSBatch *b = (LionSBatch *) llast(run->pairbatches);
	int64		i;

	run->pairbatches = list_delete_last(run->pairbatches);
	run->dist = lion_sdist_create(run->distcxt, lion_store_table_size(run),
								  run);
	run->spilling = false;
	run->pairbits = b->used_bits;
	run->passpairs = b->card;
	for (i = 0; i < b->ntuples; i++)
	{
		uint32		hash;
		int64		gid;
		int32		agg;

		CHECK_FOR_INTERRUPTS();
		lion_batch_read(run, b, true, &hash, &gid, &agg);
		if (gid < 0 || gid >= run->ngroups || agg < 0 || agg >= run->nagg ||
			run->aggs[agg].kind != LION_SAGG_DISTINCT ||
			run->risnull[run->aggs[agg].col])
			elog(ERROR, "LionCount: a damaged batch of the store's spill");
		lion_store_add_pair(run, run->byid[gid], agg,
							run->rvalues[run->aggs[agg].col], hash);
	}
	lion_batch_close(run, b);
	if (run->pairspill != NULL)
	{
		lion_spill_finish(run, run->pairspill, &run->pairbatches);
		run->pairspill = NULL;
	}
	run->spilling = false;
	MemoryContextReset(run->distcxt);
	run->dist = NULL;
}

/*
 * The pass's input is over (lion_store_count(), lion_store_group_batch()):
 * its spills become batches - of groups, onto the stack the node reads once
 * this pass's groups have gone out, and of pairs, counted into this pass's
 * groups now - and its groups are complete.
 *
 * No pair is counted twice.  In spill mode no new pair enters the table, so a
 * pair is written to a batch only when the table does not have it - and
 * then never had it, and never will: it was never counted in memory.  Every
 * later row of the pair goes to the same partition, under the same hash,
 * so a batch holds all of a pair that memory did not, and its own table
 * counts each once; a batch that spills again keeps the same rule a level
 * deeper.  The pass's table may therefore go before its batches are read.
 * Nor is a group split: once spilling, no group enters the table, and a
 * group in it keeps its rows to the end of the pass, so a group's rows are
 * all in memory or all in one batch, as a HashAggregate's are.
 */
static void
lion_store_pass_end(LionStoreRun *run)
{
	if (run->groupspill != NULL)
	{
		lion_spill_finish(run, run->groupspill, &run->groupbatches);
		run->groupspill = NULL;
	}
	if (run->pairspill != NULL)
	{
		lion_spill_finish(run, run->pairspill, &run->pairbatches);
		run->pairspill = NULL;
	}
	if (run->pairbatches != NIL)
	{
		MemoryContextReset(run->distcxt);
		run->dist = NULL;
		while (run->pairbatches != NIL)
			lion_store_pair_batch(run);
	}
	run->spilling = false;
	run->totalgroups += run->ngroups;
	run->iterating = false;
}

/*
 * The next batch of groups, the deepest first (nodeAgg.c's hash_batches, a
 * stack): a pass of its own over the batch's rows, under the hashes they were
 * spilled with, which may spill again a level deeper, and the pairs it
 * leaves.  The rows were handed over, and counted, once already: run->rows
 * does not count them again.
 */
static void
lion_store_group_batch(LionStoreRun *run)
{
	LionSBatch *b = (LionSBatch *) llast(run->groupbatches);
	int64		i;

	run->groupbatches = list_delete_last(run->groupbatches);
	lion_store_pass_begin(run, b->card,
						  run->estpairs * b->card / run->estgroups,
						  b->used_bits);
	for (i = 0; i < b->ntuples; i++)
	{
		uint32		hash;

		CHECK_FOR_INTERRUPTS();
		lion_batch_read(run, b, false, &hash, NULL, NULL);
		lion_store_add(run, run->rvalues, run->risnull, hash);
	}
	lion_batch_close(run, b);
	lion_store_pass_end(run);
}

/* Every file the spill has open goes: at a rescan, or at the node's end. */
static void
lion_store_close_files(LionStoreRun *run)
{
	LionSSpill *spills[2];
	List	   *batches[2];
	ListCell   *lc;
	int			i;
	int			p;

	spills[0] = run->groupspill;
	spills[1] = run->pairspill;
	batches[0] = run->groupbatches;
	batches[1] = run->pairbatches;
	for (i = 0; i < 2; i++)
	{
		foreach(lc, batches[i])
			BufFileClose(((LionSBatch *) lfirst(lc))->file);
		if (spills[i] == NULL)
			continue;
		for (p = 0; p < spills[i]->nparts; p++)
		{
			if (spills[i]->files[p] != NULL)
				BufFileClose(spills[i]->files[p]);
		}
	}
	run->groupspill = NULL;
	run->pairspill = NULL;
	run->groupbatches = NIL;
	run->pairbatches = NIL;
	run->nfiles = 0;
	run->diskbytes = 0;
	run->rbuf = NULL;
	run->rbufsize = 0;
	MemoryContextReset(run->spillcxt);
}

/*
 * The count of the relation being counted - the table, or a partition in its
 * turn - with a gather over index's store attached to every count of it,
 * each row handed to lion_store_row().  Each gathered column is found by its
 * place in the store, from the heap column it is in THIS relation: the plan's
 * attnos are the parent's (DESIGN.md §16), and a partition may number its
 * columns differently (lion_heap_attno_in()); its type is the parent's in
 * every partition, so what the groups hash and compare is the same.  The
 * count is checked against the rows handed over: a path of the count engine
 * that counted rows without handing them over would be an answer missing
 * rows, not a slower one.
 */
static int64
lion_store_count_relation(LionCountScanState *st, Relation index)
{
	LionStoreRun *run = st->store;
	LionIndexState *ix;
	LionGather *gather;
	LionGatherStats gs;
	int		   *ords;
	int64		before = run->rows;
	volatile int64 count = 0;
	int			i;

	ix = lion_get_index_state(index);
	ords = (int *) palloc(sizeof(int) * run->ncols);
	for (i = 0; i < run->ncols; i++)
	{
		AttrNumber	attno = lion_heap_attno_in(st->heap, st->heapoid,
											   run->attnos[i]);
		int			ord;

		for (ord = 0; ord < ix->nstored; ord++)
		{
			AttrNumber	col = ix->stored[ord].attno;

			if (index->rd_index->indkey.values[col - 1] == attno)
				break;
		}
		if (ord >= ix->nstored)
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("lion index \"%s\" does not store column %d of \"%s\"",
							RelationGetRelationName(index), attno,
							RelationGetRelationName(st->heap))));
		ords[i] = ord;
	}

	gather = lion_gather_create(st->heap, index, run->ncols, ords,
								lion_store_row, run, CurrentMemoryContext);
	lion_vis_cache_set_gather(st->viscache, gather);
	PG_TRY();
	{
		if (st->sumall)
			count = lion_sumall_relation(st);
		else
			count = lion_count_relation(st);
	}
	PG_FINALLY();
	{
		lion_vis_cache_set_gather(st->viscache, NULL);
	}
	PG_END_TRY();

	lion_gather_get_stats(gather, &gs);
	run->gstats.store_rows += gs.store_rows;
	run->gstats.heap_rows += gs.heap_rows;
	run->gstats.store_pages += gs.store_pages;
	run->gstats.absent_pages += gs.absent_pages;
	lion_gather_destroy(gather);
	pfree(ords);

	if (count != run->rows - before)
		elog(ERROR, "LionCount: counted " INT64_FORMAT " rows but gathered " INT64_FORMAT,
			 count, run->rows - before);
	return count;
}

/*
 * The count, with the gather attached to every count of it, and the groups
 * it fills.  The count is what `count(*)` under the same WHERE counts - the
 * WHERE's intersection, or the sum over every row - of the table, or of
 * every live leaf partition in turn (DESIGN.md §40, "As built: partitioned
 * tables"): each partition's turn begun as lion_run_partition() begins it,
 * its WHERE located, its rows counted and gathered from its own index's store
 * into the one hash table, everything it located let go of and its turn
 * ended, so that no pin outlives the turn (§16).  The groups are complete
 * only when the last partition has been counted; nothing comes out before.
 * That is the first pass: what it spills, the node reads after its groups
 * (lion_store_pass_end()).
 */
static void
lion_store_count(LionCountScanState *st)
{
	LionStoreRun *run = st->store;
	int64		count = 0;
	int			p;

	lion_store_pass_begin(run, run->estgroups, run->estpairs, 0);
	run->rows = 0;

	if (st->npart == 0)
	{
		/*
		 * The index whose store the values are read from, opened for the
		 * count; the table's own relations are open, and its WHERE located,
		 * already.
		 */
		Relation	index = index_open(run->indexoid, AccessShareLock);

		count = lion_store_count_relation(st, index);
		index_close(index, AccessShareLock);
	}
	else
	{
		for (p = 0; p < st->npart; p++)
		{
			Assert(st->part[p].storeidx != NULL);
			lion_open_relation(st, p);
			lion_locate_where(st);
			count += lion_store_count_relation(st, st->part[p].storeidx);
			lion_release_where(st);
			lion_close_relation(st);
			CHECK_FOR_INTERRUPTS();
		}
	}

	if (run->single != NULL)
		run->single->count = count;
	lion_store_pass_end(run);
	run->counted = true;
	run->singledone = false;
}

/* The results of group g's aggregates, for lion_store_emit_value(). */
static void
lion_store_finish_group(LionStoreRun *run, LionSGroup *g)
{
	MemoryContext oldcxt;
	int			i;

	MemoryContextReset(run->outcxt);
	oldcxt = MemoryContextSwitchTo(run->outcxt);
	for (i = 0; i < run->nagg; i++)
	{
		LionSAggSpec *a = &run->aggs[i];
		LionSAggState *s = &g->agg[i];

		if (a->kind == LION_SAGG_COUNTCOL || a->kind == LION_SAGG_DISTINCT)
		{
			run->aggval[i] = Int64GetDatum(s->n);
			run->aggnull[i] = false;
		}
		else
		{
			LionWAgg	w;

			memset(&w, 0, sizeof(w));
			w.kind = LION_SAGG_WAGG_KIND(a->kind);
#ifdef HAVE_INT128
			w.sum = s->sum;
#endif
			w.n = s->n;
			w.ext = s->ext;
			w.hasext = s->hasext;
			lion_wagg_finish(&w);
			run->aggval[i] = w.result;
			run->aggnull[i] = w.resnull;
		}
	}
	MemoryContextSwitchTo(oldcxt);
	run->cur = g;
}

/*
 * The node's next row: the count, the first time, then one row per group -
 * or, with no GROUP BY, the one row, which a plain aggregate has even over
 * no rows and a GROUP BY the planner folded to one group has only when the
 * group has rows.  The groups of the first pass come out first, then each
 * batch of groups it spilled is read in a pass of its own and its groups
 * come out, the deepest batch first, until none is left; without a GROUP BY
 * only pairs spill, and the one row waits for them.  NULL when there is no
 * more, or when the HAVING turned the row away (st->filtered,
 * lion_emit_keys()).
 */
TupleTableSlot *
lion_store_next(LionCountScanState *st)
{
	LionStoreRun *run = st->store;
	LionSGroup *g;

	Assert(run != NULL);
	if (!run->counted)
		lion_store_count(st);

	if (run->ngroup == 0)
	{
		st->done = true;
		g = run->single;
		if (run->singledone || (st->singlegroup && g->count == 0))
			return NULL;
		run->singledone = true;
	}
	else
	{
		LionSGroupEnt *ent;

		for (;;)
		{
			if (!run->iterating)
			{
				lion_sgroup_start_iterate(run->groups, &run->iter);
				run->iterating = true;
			}
			ent = lion_sgroup_iterate(run->groups, &run->iter);
			if (ent != NULL)
				break;
			if (run->groupbatches == NIL)
			{
				st->done = true;
				return NULL;
			}
			lion_store_group_batch(run);
		}
		g = ent->key;
	}

	lion_store_finish_group(run, g);
	return lion_emit_keys(st, 0, NULL, NULL, g->count);
}

/* A gathered GROUP BY column's value, or an aggregate's, of the row emitted. */
Datum
lion_store_emit_value(LionCountScanState *st, int kind, bool *isnull)
{
	LionStoreRun *run = st->store;

	Assert(run != NULL && run->cur != NULL);
	if (LION_TL_IS_SKEY(kind))
	{
		int			k = LION_TL_SKEY_NO(kind);

		Assert(k < run->ngroup);
		*isnull = run->cur->nulls[k];
		return run->cur->keys[k];
	}
	Assert(LION_TL_IS_SAGG(kind) && LION_TL_SAGG_NO(kind) < run->nagg);
	*isnull = run->aggnull[LION_TL_SAGG_NO(kind)];
	return run->aggval[LION_TL_SAGG_NO(kind)];
}

/*
 * A rescan, or the end: the groups go, and every batch file not yet read,
 * and the next row counts again.
 */
void
lion_store_reset(LionCountScanState *st)
{
	LionStoreRun *run = st->store;

	if (run == NULL)
		return;
	lion_store_close_files(run);
	run->counted = false;
	run->iterating = false;
	run->singledone = false;
	run->spilling = false;
	run->cur = NULL;
	run->groups = NULL;
	run->dist = NULL;
	run->single = NULL;
	run->probe = NULL;
	run->byid = NULL;
	run->byidcap = 0;
	MemoryContextReset(run->cxt);
	run->distcxt = NULL;
	MemoryContextReset(run->outcxt);
}

/*
 * EXPLAIN: the GROUP BY the node forms of the gathered values, and the
 * columns it gathers (`Store: col1, col2`) - and with ANALYZE where their
 * values came from: the rows of all-visible pages the store supplied, the
 * rows read from the heap (a page that is not all-visible, or one the store
 * left to it), and the all-visible pages it left to it; the groups formed;
 * and the spill, as a HashAggregate reports its own: the batches - 1 when
 * nothing spilled, and one more for every batch of groups or of pairs - and
 * the most disk the batch files took at once.
 */
void
lion_store_explain(LionCountScanState *st, ExplainState *es)
{
	LionStoreRun *run = st->store;
	StringInfoData buf;
	int			i;

	if (run == NULL)
		return;
	if (run->ngroup > 0)
	{
		initStringInfo(&buf);
		for (i = 0; i < run->ngroup; i++)
			appendStringInfo(&buf, "%s%s", (i > 0) ? ", " : "",
							 get_attname(st->heapoid,
										 run->attnos[run->groupcol[i]],
										 false));
		ExplainPropertyText("Group Key", buf.data, es);
		pfree(buf.data);
	}
	initStringInfo(&buf);
	for (i = 0; i < run->ncols; i++)
		appendStringInfo(&buf, "%s%s", (i > 0) ? ", " : "",
						 get_attname(st->heapoid, run->attnos[i], false));
	ExplainPropertyText("Store", buf.data, es);
	pfree(buf.data);

	if (es->analyze)
	{
		ExplainPropertyInteger("Store Rows", NULL, run->gstats.store_rows, es);
		ExplainPropertyInteger("Store Rows From Heap", NULL,
							   run->gstats.heap_rows, es);
		ExplainPropertyInteger("Store Pages Absent", NULL,
							   run->gstats.absent_pages, es);
		ExplainPropertyInteger("Store Groups", NULL, run->totalgroups, es);
		ExplainPropertyInteger("Store Batches", NULL, run->nbatches, es);
		ExplainPropertyInteger("Store Disk Usage", "kB",
							   (run->diskpeak + 1023) / 1024, es);
	}
}
