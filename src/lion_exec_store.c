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
 * Part of the LionCount custom scan: lion_customscan.h describes the node
 * and declares what its files share.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "lion_customscan.h"
#include "lion_store.h"

#include "common/hashfn.h"

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
	Oid			indexoid;
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

	/* a run's */
	MemoryContext cxt;			/* the groups, their keys and values */
	MemoryContext outcxt;		/* the row being emitted */
	lion_sgroup_hash *groups;
	lion_sdist_hash *dist;
	LionSGroup *single;			/* without a GROUP BY: the one group */
	LionSGroup *probe;			/* a row's keys, to look its group up */
	int64		ngroups;
	int64		rows;			/* the rows handed over */
	bool		counted;
	bool		iterating;
	lion_sgroup_iterator iter;
	bool		singledone;

	/* the row being emitted */
	LionSGroup *cur;
	Datum	   *aggval;
	bool	   *aggnull;

	/* what EXPLAIN ANALYZE reports, over every run */
	LionGatherStats gstats;
	int64		totalgroups;
} LionStoreRun;

static Size
lion_sgroup_size(LionStoreRun *run)
{
	return offsetof(LionSGroup, agg) + sizeof(LionSAggState) * Max(run->nagg, 1);
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
 * The plan's LION_PRIV_STORE member, read into st->store: the index, the
 * columns, the GROUP BY's equalities and hash functions, the aggregates.
 * Nothing is opened: an EXPLAIN without ANALYZE reads only this.
 */
void
lion_store_begin(LionCountScanState *st, CustomScan *cscan, EState *estate)
{
	List	   *m = (List *) list_nth(cscan->custom_private, LION_PRIV_STORE);
	LionStoreRun *run;
	List	   *attnos;
	List	   *groups;
	List	   *specs;
	int			i;

	st->store = NULL;
	if (m == NIL)
		return;
	if (list_length(m) != 5 || list_length((List *) linitial(m)) != 1)
		elog(ERROR, "LionCount: malformed store gather");

	/*
	 * The gather is fed by the count of the WHERE, or by the sum over every
	 * row: nothing of the node's own groups, walks or joins is beside it.
	 */
	if (st->groupattno != 0 || st->groupattno2 != 0 || st->distattno != 0 ||
		st->nwagg != 0 || st->joinclause >= 0 || st->npart > 0 ||
		st->topkn != 0 || st->decode != NULL ||
		(st->hasgroupidx && !st->sumall))
		elog(ERROR, "LionCount: a store gather beside another walk");

	run = (LionStoreRun *) palloc0(sizeof(LionStoreRun));
	run->indexoid = linitial_oid((List *) linitial(m));
	attnos = (List *) lsecond(m);
	groups = (List *) lfourth(m);
	specs = (List *) list_nth(m, 4);

	run->ncols = list_length(attnos);
	if (run->ncols <= 0 || list_length((List *) lthird(m)) != run->ncols)
		elog(ERROR, "LionCount: malformed store gather");
	run->attnos = (AttrNumber *) palloc(sizeof(AttrNumber) * run->ncols);
	for (i = 0; i < run->ncols; i++)
		run->attnos[i] = (AttrNumber) list_nth_int(attnos, i);

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

	run->cxt = AllocSetContextCreate(estate->es_query_cxt,
									 "LionCount store groups",
									 ALLOCSET_DEFAULT_SIZES);
	run->outcxt = AllocSetContextCreate(estate->es_query_cxt,
										"LionCount store row",
										ALLOCSET_SMALL_SIZES);
	run->aggval = (Datum *) palloc0(sizeof(Datum) * Max(run->nagg, 1));
	run->aggnull = (bool *) palloc0(sizeof(bool) * Max(run->nagg, 1));
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
	MemoryContextSwitchTo(oldcxt);
	return g;
}

/*
 * One row the count counts, with the values of the gathered columns
 * (LionGatherRowFn): its group's rows and aggregates.  The values are the
 * gather's until this returns; what a group keeps is copied.
 */
static void
lion_store_row(void *arg, const Datum *values, const bool *isnull)
{
	LionStoreRun *run = (LionStoreRun *) arg;
	LionSGroup *g;
	int			i;

	run->rows++;
	if (run->ngroup == 0)
		g = run->single;
	else
	{
		LionSGroup *probe = run->probe;
		LionSGroupEnt *ent;
		uint32		hash = 0;
		bool		found;
		int			k;

		/*
		 * The hash of the keys, combined as execGrouping.c combines a
		 * grouping's columns: rotate, then xor in the next column's - a NULL
		 * adding nothing - with the column's own hash function under its
		 * collation, so that the keys its equality calls equal fall together.
		 */
		for (k = 0; k < run->ngroup; k++)
		{
			int			c = run->groupcol[k];

			hash = pg_rotate_left32(hash, 1);
			probe->nulls[k] = isnull[c];
			probe->keys[k] = values[c];
			if (!isnull[c])
				hash ^= DatumGetUInt32(FunctionCall1Coll(&run->grouphash[k],
														 run->groupcoll[k],
														 values[c]));
		}
		probe->hash = murmurhash32(hash);
		ent = lion_sgroup_insert_hash(run->groups, probe, probe->hash, &found);
		if (!found)
			ent->key = lion_sgroup_new(run, probe);
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
				{
					LionSDistKey key;
					bool		found;
					uint32		h;
					LionSDistEnt *ent;

					key.gid = g->id;
					key.agg = i;
					key.value = v;
					h = DatumGetUInt32(FunctionCall1Coll(&a->hash, a->distcoll,
														 v));
					h = hash_combine(murmurhash32((uint32) g->id ^
												  (uint32) (g->id >> 32)),
									 hash_combine((uint32) i, h));
					ent = lion_sdist_insert_hash(run->dist, key, h, &found);
					if (!found)
					{
						MemoryContext oldcxt = MemoryContextSwitchTo(run->cxt);

						ent->key.value = datumCopy(v, a->typbyval, a->typlen);
						MemoryContextSwitchTo(oldcxt);
						s->n++;
					}
				}
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
 * The count, with the gather attached to every count of it, and the groups
 * it fills.  The count is what `count(*)` under the same WHERE counts - the
 * WHERE's intersection, or the sum over every row - and it is checked
 * against the rows handed over: a path of the count engine that counted rows
 * without handing them over would be an answer missing rows, not a slower
 * one.
 */
static void
lion_store_count(LionCountScanState *st)
{
	LionStoreRun *run = st->store;
	Relation	index;
	LionIndexState *ix;
	LionGather *gather;
	LionGatherStats gs;
	int		   *ords;
	volatile int64 count = 0;
	int			i;

	MemoryContextReset(run->cxt);
	run->groups = NULL;
	run->dist = NULL;
	run->ngroups = 0;
	run->rows = 0;
	if (run->ngroup > 0)
	{
		run->groups = lion_sgroup_create(run->cxt, 256, run);
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
		run->dist = lion_sdist_create(run->cxt, 256, run);

	/*
	 * The index whose store the values are read from, opened for the count:
	 * each column by its place in the store, found again from the heap
	 * column, as every other index of the node is (lion_index_col_for()).
	 */
	index = index_open(run->indexoid, AccessShareLock);
	ix = lion_get_index_state(index);
	ords = (int *) palloc(sizeof(int) * run->ncols);
	for (i = 0; i < run->ncols; i++)
	{
		int			ord;

		for (ord = 0; ord < ix->nstored; ord++)
		{
			AttrNumber	col = ix->stored[ord].attno;

			if (index->rd_index->indkey.values[col - 1] == run->attnos[i])
				break;
		}
		if (ord >= ix->nstored)
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("lion index \"%s\" does not store column %d of \"%s\"",
							RelationGetRelationName(index), run->attnos[i],
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
	run->totalgroups += run->ngroups;
	lion_gather_destroy(gather);
	pfree(ords);
	index_close(index, AccessShareLock);

	if (count != run->rows)
		elog(ERROR, "LionCount: counted " INT64_FORMAT " rows but gathered " INT64_FORMAT,
			 count, run->rows);
	if (run->single != NULL)
		run->single->count = count;
	run->counted = true;
	run->iterating = false;
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
 * group has rows.  NULL when there is no more, or when the HAVING turned the
 * row away (st->filtered, lion_emit_keys()).
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

		if (!run->iterating)
		{
			lion_sgroup_start_iterate(run->groups, &run->iter);
			run->iterating = true;
		}
		ent = lion_sgroup_iterate(run->groups, &run->iter);
		if (ent == NULL)
		{
			st->done = true;
			return NULL;
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

/* A rescan, or the end: the groups go, and the next row counts again. */
void
lion_store_reset(LionCountScanState *st)
{
	LionStoreRun *run = st->store;

	if (run == NULL)
		return;
	run->counted = false;
	run->iterating = false;
	run->singledone = false;
	run->cur = NULL;
	run->groups = NULL;
	run->dist = NULL;
	run->single = NULL;
	run->probe = NULL;
	MemoryContextReset(run->cxt);
	MemoryContextReset(run->outcxt);
}

/*
 * EXPLAIN: the GROUP BY the node forms of the gathered values, and the
 * columns it gathers (`Store: col1, col2`) - and with ANALYZE where their
 * values came from: the rows of all-visible pages the store supplied, the
 * rows read from the heap (a page that is not all-visible, or one the store
 * left to it), and the all-visible pages it left to it.
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
	}
}
