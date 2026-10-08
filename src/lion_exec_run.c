/*-------------------------------------------------------------------------
 *
 * lion_exec_run.c
 *		The run of a LionCount scan: partitions, pausing, rescans, parallel
 *		DSM, shutdown and end.
 *
 * Part of the LionCount custom scan: lion_customscan.h describes the node
 * and declares what its files share.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "lion_customscan.h"

static TupleTableSlot *lion_exec_custom_scan_internal(CustomScanState *node);

/*
 * The group of c of GROUP BY coalesce(g, c) (DESIGN.md §10), not yet counted
 * or emitted: before each entry walk that may meet it.
 */
static void
lion_coal_reset(LionCountScanState *st)
{
	if (st->coal != NULL)
	{
		st->coal->count = 0;
		st->coal->walked = false;
	}
}

/*
 * The nested loop of DESIGN.md §20 and §26 between outer groups: the inner
 * index's walk, if one is begun, ended, and no outer group open - the caller
 * releases groupset, which held it.
 */
static void
lion_inner_reset(LionCountScanState *st)
{
	if (st->inner == NULL)
		return;
	if (st->inner->scanning)
	{
		lion_entry_scan_end(&st->inner->escan);
		st->inner->scanning = false;
	}
	st->inner->outeropen = false;
}

/*
 * Walk one partition without a group key: its turn, in which it is the
 * relation counted - its clauses located, it counted, and everything it
 * located let go of again (DESIGN.md §16).
 */
static int64
lion_run_partition(LionCountScanState *st, int p)
{
	int64		count;

	lion_open_relation(st, p);
	lion_locate_where(st);

	if (st->hasgroupidx)
	{
		/* No group key of its own: the only driver left is a sum-over-all. */
		Assert(st->sumall);
		count = lion_sumall_relation(st);
	}
	else
		count = lion_count_relation(st);

	lion_release_where(st);
	lion_close_relation(st);

	return count;
}

/*
 * GROUP BY over a partitioned table: one PARTIAL aggregate per group per
 * partition (DESIGN.md §16).
 *
 * The partitions are walked in the planner's order and each one has its
 * turn - made the relation counted, iterated, and let go of - its groups
 * emitted as they are counted.  The node therefore holds no cross-partition
 * state beyond the open relations - no hash table, no per-node group memory
 * beyond one partition's iteration state - and the Finalize HashAggregate
 * core puts above it combines the partial counts, spilling to disk under
 * hash_mem like any HashAggregate.  A group with rows in several partitions
 * is emitted once per partition, and the §9 pin discipline is unchanged: a
 * partition's posting sets are all released before its turn ends.
 */
static TupleTableSlot *
lion_next_partial_group(LionCountScanState *st)
{
	for (;;)
	{
		if (!st->partopen)
		{
			if (st->curpart >= st->npart)
			{
				st->done = true;
				return NULL;
			}

			lion_open_relation(st, st->curpart);
			lion_locate_where(st);
			st->partopen = true;

			/*
			 * A positive clause with no entry in THIS partition selects
			 * nothing here, whatever the others hold, so its entry scan is
			 * skipped and no group of it is emitted.
			 */
			if (!st->wheremissing)
			{
				lion_entry_scan_begin_range(&st->escan, st->groupidx,
											st->groupidxcol,
											st->hasrange ? &st->range : NULL);
				st->scanning = true;
				/* each partition's group of c is its own partial row */
				lion_coal_reset(st);
			}
		}

		if (st->scanning)
		{
			bool		exhausted;
			TupleTableSlot *slot = lion_next_group_any(st, &exhausted);

			if (!exhausted)
				return slot;

			lion_entry_scan_end(&st->escan);
			st->scanning = false;
		}

		/* This partition is done: release its sets, then end its turn. */
		lion_release_where(st);
		lion_close_relation(st);
		st->partopen = false;
		st->curpart++;
	}
}

/*
 * The partitioned form of the node without a group key (DESIGN.md §16): the
 * one row is the sum over every partition, so nothing comes out until the
 * last of them has been counted.
 */
static TupleTableSlot *
lion_exec_partitioned(LionCountScanState *st)
{
	int64		total = 0;
	int			p;

	if (st->hasgroupidx && st->groupattno != 0)
		return lion_next_partial_group(st);

	for (p = 0; p < st->npart; p++)
		total += lion_run_partition(st, p);

	st->done = true;

	/*
	 * A plain aggregate always produces its one row; a GROUP BY whose columns
	 * the planner folded to constants produces one only if the group exists.
	 */
	if (total == 0 && st->singlegroup)
		return NULL;

	return lion_emit_tuple(st, (Datum) 0, true, (Datum) 0, true, total);
}

/*
 * A row is about to go up to the executor, which may not ask for the next one
 * for as long as a cursor stays open - so nothing the node keeps across the
 * row may hold a buffer pin, or an idle cursor would hold VACUUM up on that
 * page (§11 takes a cleanup lock on every page it rewrites).  DESIGN.md §9's
 * pin is the interlock for a count's visibility-map questions and nothing
 * else, and between two rows no count is running:
 *
 *	- the walks let go of the directory leaf they stand on
 *	  (lion_entry_scan_pause());
 *	- every located WHERE set that holds a pin - an INLINE one - gives it up
 *	  and becomes NOPIN (lion_posting_set_unpin()): a private copy, which is
 *	  what a set located past the §15 pin budget has always been, and which
 *	  every count already knows how to take.  Each count after this row gets
 *	  the interlock from the set that drives it, which every shape that
 *	  returns more than one row locates afresh under a pin of its own - a
 *	  group's entry, the inner set of a (g, k) pair (§20, §26), the fk set of
 *	  a dimension row (§27), and the listed value an IN list drives the
 *	  groups by, which lion_next_group_inlist() locates again for that
 *	  reason.  A count that is left with no source that carries it trusts no
 *	  map and rechecks, and a lone NOPIN set is located again before it is
 *	  counted, as always (lion_count_sources_cached());
 *	- and the outer group's set of a two-column GROUP BY (§20) does the same:
 *	  its remaining pairs are carried by their inner sets.
 */
void
lion_pause_run(LionCountScanState *st)
{
	if (st->scanning)
		lion_entry_scan_pause(&st->escan);
	if (st->inner != NULL && st->inner->scanning)
		lion_entry_scan_pause(&st->inner->escan);

	lion_unpin_where(st);
	if (st->inner != NULL && st->inner->outeropen)
		lion_posting_set_unpin(&st->groupset);

	/*
	 * ... and a walk of the fk index in key order (§27) lets go of the leaf it
	 * stands on; the next key reads it again by its block number.
	 */
	if (st->joinwalkbegun)
		lion_lookup_walk_pause(&st->joinwalker);
}

/*
 * The WHERE sets let go of the pins they were located with: NOPIN copies from
 * here on, every count of them carried by another source's pin (DESIGN.md
 * §15, "Paused and finished counts").  They gain pins only where they are
 * located (lion_locate_where()), so after the first time there is nothing
 * left to unpin in them - and walking them all again after every row made a
 * list-driven GROUP BY of N values N-squared.
 */
void
lion_unpin_where(LionCountScanState *st)
{
	int			i;
	int			j;

	if (st->sources == NULL || !st->wherepinned)
		return;
	for (i = 1; i <= st->nitem; i++)
	{
		LionCountSource *src = &st->sources[i];

		for (j = 0; j < src->nsets; j++)
			lion_posting_set_unpin(&src->sets[j]);
	}
	st->wherepinned = false;
}

/*
 * The node has produced its last row.  A plain table stays open until the
 * node ends, and everything located in it used to stay located with it - the
 * WHERE sets and their pins, which a cursor left open after its last FETCH, or
 * after the one row of a plain count, held until the transaction ended.
 * Nothing is counted again before a rescan, which locates everything afresh,
 * so the walks, the group sets and the WHERE sets all go now.  (A partitioned
 * scan has let go of each partition's at the end of its turn already.)
 */
static void
lion_finish_run(LionCountScanState *st)
{
	if (st->scanning)
	{
		lion_entry_scan_end(&st->escan);
		st->scanning = false;
	}
	lion_inner_reset(st);
	lion_posting_set_release(&st->groupset);
	lion_posting_set_release(&st->groupset2);
	lion_decode_reset(st);
	lion_join_batch_reset(st);
	lion_release_where(st);

	/*
	 * ... and the visibility-map page the counts kept pinned from one to the
	 * next, which a paused node keeps as an index-only scan keeps its own:
	 * no VACUUM waits for a map page's pin (lion_vis_cache_release_vm()).
	 */
	lion_vis_cache_release_vm(st->viscache);
}

TupleTableSlot *
lion_exec_custom_scan(CustomScanState *node)
{
	LionCountScanState *st = (LionCountScanState *) node;
	int64		dirbefore = lion_dir_pages_read;
	TupleTableSlot *slot;

	/*
	 * One group per call, except that a group the HAVING rejects is not a
	 * result: keep going until one passes or the groups run out.
	 */
	for (;;)
	{
		if (st->done)
		{
			slot = NULL;
			break;
		}
		st->filtered = false;
		slot = lion_exec_custom_scan_internal(node);
		if (slot != NULL || !st->filtered)
			break;
		CHECK_FOR_INTERRUPTS();
	}

	/*
	 * No index pin outlives the call (DESIGN.md §15, "Paused and finished
	 * counts"): a node that is done lets go of everything, and one that
	 * returns a row with more to come keeps its sets but none of their pins -
	 * only the visibility-map page its counts share, as an index-only scan
	 * keeps its own.  The row itself points at nothing either releases: a key
	 * the WHERE pinned is in keycxt, a group's key in pergroup or outercxt,
	 * and a listed value's in the set, which a pause keeps.
	 */
	if (st->done)
		lion_finish_run(st);
	else if (slot != NULL)
		lion_pause_run(st);
	st->dirpages += lion_dir_pages_read - dirbefore;

	return slot;
}

static TupleTableSlot *
lion_exec_custom_scan_internal(CustomScanState *node)
{
	LionCountScanState *st = (LionCountScanState *) node;
	int64		count;

	/*
	 * The parameters first: a generic prepared plan's `k = $1` and a nested
	 * loop's exec Param are only values here, and ReScan has thrown the
	 * previous ones away (DESIGN.md §10).
	 */
	if (!st->valsdone)
		lion_eval_clause_values(st);

	/*
	 * ---- the FK-side join: one partial row per dimension row ----
	 *
	 * The join over a partitioned fact table takes each batch of keys to
	 * every partition in turn, and locates nothing of its own here.
	 */
	switch (st->mode)
	{
		case LION_MODE_JOIN:
		case LION_MODE_JOIN_FACTGROUP:
			Assert(st->joinclause >= 0);
			if (st->npart == 0 && !st->located)
				lion_join_locate_where(st);
			return lion_next_join_row(st);
		default:
			Assert(st->joinclause < 0);
			break;
	}

	/* A partitioned table counts one partition at a time. */
	if (st->npart > 0)
		return lion_exec_partitioned(st);

	if (!st->located)
		lion_locate_where(st);

	/* ---- no index to iterate: exactly one row ---- */
	Assert((st->mode == LION_MODE_COUNT) == !st->hasgroupidx);
	if (st->mode == LION_MODE_COUNT)
	{
		/* Without a group index a clause has to drive the count. */
		st->done = true;
		count = lion_count_relation(st);

		/*
		 * A plain aggregate always produces its one row; a GROUP BY whose
		 * columns the planner folded to constants produces one only if the
		 * group exists.
		 */
		if (count == 0 && st->singlegroup)
			return NULL;

		return lion_emit_tuple(st, (Datum) 0, true, (Datum) 0, true, count);
	}

	/* ---- the group index drives the count ---- */
	if (st->wheremissing)
	{
		st->done = true;
		/*
		 * A sum over all entries still has to report its one row, and so does
		 * a count(DISTINCT k) without a GROUP BY (DESIGN.md §26) - unless the
		 * planner folded a GROUP BY to one group, which does not exist then.
		 */
		if (st->dist != NULL)
		{
			st->dist->count = 0;
			st->dist->colcount = 0;
		}
		Assert((st->mode == LION_MODE_SUM || st->mode == LION_MODE_DISTINCT) ==
			   (st->sumall || (st->distattno != 0 && st->groupattno == 0)));
		if ((st->mode == LION_MODE_SUM || st->mode == LION_MODE_DISTINCT) &&
			!st->singlegroup)
			return lion_emit_tuple(st, (Datum) 0, true, (Datum) 0, true, 0);
		return NULL;
	}

	switch (st->mode)
	{
		case LION_MODE_DISTINCT:
			/* ---- count(DISTINCT k) over the WHERE: one row (§26) ---- */
			Assert(st->distattno != 0 && st->groupattno == 0);
			return lion_distinct_relation(st);

		case LION_MODE_SUM:
			/* ---- every entry of the index, summed into one row ---- */
			Assert(st->sumall &&
				   !(st->distattno != 0 && st->groupattno == 0));
			{
				/*
				 * ... and the aggregates over lion columns' entries
				 * (DESIGN.md §37), beside the counts or without any.
				 */
				bool		hasagg = (st->wagg != NULL && st->wagg->nagg > 0);
				int64		total = (!hasagg || st->wagg->needcount) ?
					lion_sumall_relation(st) : 0;

				if (hasagg)
					lion_wagg_run(st);
				st->done = true;

				/*
				 * A GROUP BY the planner folded to one group has no row when
				 * the group is empty - which a range-bounded sum (DESIGN.md
				 * §28) can be: `... WHERE g = 3 AND k < 20 GROUP BY g`.
				 */
				if (total == 0 && st->singlegroup)
					return NULL;
				return lion_emit_tuple(st, (Datum) 0, true, (Datum) 0, true,
									   total);
			}

		case LION_MODE_GROUP_RANGED:

			/*
			 * A parallel GROUP BY (DESIGN.md §10, "A GROUP BY in parallel")
			 * walks the entries once per range of container keys it counts,
			 * and begins each walk itself.  Run without the Gather's shared
			 * memory - which a Gather that cannot run in parallel mode does
			 * not set up - the node is the only participant, and counts every
			 * group whole as the serial node does: one partial row a group,
			 * which the Finalize Agg takes as it takes any.
			 */
			Assert(st->ranged != NULL && !st->sumall && st->distattno == 0);
			if (st->shared != NULL)
			{
				bool		exhausted;
				TupleTableSlot *slot = lion_next_group_ranged(st, &exhausted);

				if (exhausted)
					st->done = true;
				return slot;
			}
			break;

		case LION_MODE_GROUP:
		case LION_MODE_GROUP2:
		case LION_MODE_GROUP_DISTINCT:
		case LION_MODE_DECODE:
			Assert(st->ranged == NULL && !st->sumall &&
				   !(st->distattno != 0 && st->groupattno == 0));
			break;

		case LION_MODE_COUNT:
		case LION_MODE_JOIN:
		case LION_MODE_JOIN_FACTGROUP:
			elog(ERROR, "LionCount: mode %d past its dispatch", (int) st->mode);
	}

	/*
	 * An IN list on the grouping column drives the groups itself (DESIGN.md
	 * §15) and never looks at the index's entries, so there is no entry scan to
	 * begin.  (The partitioned path opens one per partition either way; a scan
	 * that is only begun holds nothing, so it costs the flag it sets.)
	 */
	if (!st->scanning && st->ingroupitem < 0 && st->decode == NULL)
	{
		lion_entry_scan_begin_range(&st->escan, st->groupidx, st->groupidxcol,
									st->hasrange ? &st->range : NULL);
		st->scanning = true;
		lion_coal_reset(st);
	}

	/* ---- GROUP BY: one row per non-empty group ---- */
	{
		bool		exhausted;
		TupleTableSlot *slot = lion_next_group_any(st, &exhausted);

		if (exhausted)
			st->done = true;
		return slot;
	}
}

/*
 * Everything the node built while running, undone.  A partitioned scan may be
 * standing in the middle of a partition (a LIMIT above it, an error being
 * unwound), so the relation it has open is closed here too; every posting set
 * has been released before any of them, which is what DESIGN.md §9 requires.
 */
static void
lion_reset_run(LionCountScanState *st)
{
	int			i;

	if (st->scanning)
	{
		lion_entry_scan_end(&st->escan);
		st->scanning = false;
	}
	lion_inner_reset(st);
	if (st->inner != NULL)
		st->inner->next = 0;
	lion_posting_set_release(&st->groupset);
	lion_posting_set_release(&st->groupset2);
	lion_coal_reset(st);
	lion_decode_reset(st);
	lion_release_where(st);

	/* The FK-side join's collected filters (in outercxt, reset below). */
	lion_posting_set_release(&st->joinfilter);
	st->joincollected = false;
	st->joinfiltered = false;
	st->joinviewshared = false;

	/* ... or a partitioned fact table's, one copy per partition */
	if (st->joinpart != NULL)
	{
		for (i = 0; i < st->npart; i++)
		{
			LionJoinPart *jp = &st->joinpart[i];

			lion_posting_set_release(&jp->filter);
			memset(&jp->filter, 0, sizeof(jp->filter));
			jp->filter.pinbuf = InvalidBuffer;
			jp->collected = false;
			jp->filtered = false;
			jp->missing = false;
			jp->viewshared = false;
			MemoryContextReset(jp->cxt);
		}
		st->joinrunfilterrows = 0;
	}

	/* ... and the account of a plan that probes them, begun again */
	lion_join_switch_reset(st);

	/* ... and a forward semi join's sorted keys, sorted again next run. */
	if (st->joinsort != NULL)
	{
		tuplesort_end(st->joinsort);
		st->joinsort = NULL;
	}
	if (st->joinsortslot != NULL)
		ExecClearTuple(st->joinsortslot);
	if (st->joinkeycxt != NULL)
		MemoryContextReset(st->joinkeycxt);
	st->joinsortdone = false;
	st->joinhaveprev = false;
	st->joinkeypos = 0;
	st->joinchunk = -1;

	/* ... and the batch of keys being looked up in key order, and the walk */
	lion_join_batch_reset(st);
	st->joinchilddone = false;

	/* ... and the way the child is read, decided again at the next row */
	st->joinbegun = false;
	st->joinwalked = false;

	/* ... and a fact column's groups: the rows put by, and the next turn */
	if (st->fg != NULL)
	{
		LionFactGroupState *fg = st->fg;

		fg->turn = 0;
		fg->nrows = 0;
		fg->rowpos = 0;
		fg->rowcap = 0;
		fg->rows = NULL;
		fg->n = 0;
		fg->sets = NULL;
		fg->key = NULL;
		fg->isnull = NULL;
		if (fg->cxt != NULL)
			MemoryContextReset(fg->cxt);
		if (fg->rowcxt != NULL)
			MemoryContextReset(fg->rowcxt);
	}

	/* ... and the top k's groups, found again (DESIGN.md §36) */
	if (st->topk != NULL)
	{
		LionTopkState *tk = st->topk;

		tk->ran = false;
		tk->whole = false;
		tk->key = NULL;
		tk->isnull = NULL;
		tk->count = NULL;
		tk->out = 0;
		tk->pos = 0;
		if (tk->cxt != NULL)
			MemoryContextReset(tk->cxt);
	}

	if (st->npart > 0)
		lion_close_relation(st);
	if (st->joinvisitcxt != NULL)
		MemoryContextReset(st->joinvisitcxt);

	if (st->pergroup != NULL)
		MemoryContextReset(st->pergroup);
	if (st->outercxt != NULL)
		MemoryContextReset(st->outercxt);

	for (i = 0; i < st->nclause; i++)
	{
		st->clause[i].hasstoredkey = false;
		st->clause[i].keyisnull = false;
		st->clause[i].storedkey = (Datum) 0;
	}
	if (st->keycxt != NULL)
		MemoryContextReset(st->keycxt);

	if (st->viscache != NULL)
		lion_vis_cache_reset(st->viscache);

	/*
	 * The clause values go too: a rescan of a parameterised inner side has to
	 * read the new exec Param rather than the value the last scan copied
	 * (DESIGN.md §10).  A literal's value lives in the Const and stays.
	 */
	st->valsdone = false;
	for (i = 0; i < st->nclause; i++)
	{
		LionClauseState *cl = &st->clause[i];

		if (cl->valstate != NULL)
		{
			cl->val = (Datum) 0;
			cl->valisnull = false;
		}
	}
	if (st->valcxt != NULL)
		MemoryContextReset(st->valcxt);

	st->curpart = 0;
	st->partopen = false;
}

void
lion_rescan_custom_scan(CustomScanState *node)
{
	LionCountScanState *st = (LionCountScanState *) node;

	lion_reset_run(st);
	st->done = false;

	/*
	 * The join's child (DESIGN.md §27) starts over too.  Core propagates a
	 * changed parameter to outer and inner plans but not to custom_ps, so it
	 * is handed on here; a child that has one rescans itself on its next
	 * ExecProcNode().
	 */
	if (st->child != NULL)
	{
		if (node->ss.ps.chgParam != NULL)
			UpdateChangedParamSet(st->child, node->ss.ps.chgParam);
		if (st->child->chgParam == NULL)
			ExecReScan(st->child);
	}
	st->childslot = NULL;
}

/*
 * A parallel FK-side join (DESIGN.md §27, "Parallel") keeps a small struct in
 * the Gather's dynamic shared memory: what the participants add up for EXPLAIN
 * ANALYZE, and the fact filters' copy, which they collect once for all of them
 * when the plan collects the filters (DESIGN.md §27, "One copy per query"):
 * the copy's own state follows the struct, and its containers are in the
 * query's DSA or in files of its file set.  Each participant reads its share
 * of the dimension rows from the parallel-aware child and locates the fact
 * filters for itself.
 *
 * A parallel GROUP BY (DESIGN.md §10, "A GROUP BY in parallel") keeps the same
 * struct, with no copy after it: the ranges of container keys its participants
 * claim one at a time, and the sums of their counters.  A node that is
 * neither, or one run without a Gather, never gets here.
 */
Size
lion_estimate_dsm(CustomScanState *node, ParallelContext *pcxt)
{
	LionCountScanState *st = (LionCountScanState *) node;

	if (st->ranged != NULL)
		return MAXALIGN(sizeof(LionJoinShared));
	return MAXALIGN(sizeof(LionJoinShared)) +
		(Size) ((st->joincollect && st->npart > 0) ? st->npart : 1) *
		lion_shared_copy_size();
}

/*
 * The heap blocks of each leaf partition of a partitioned fact table, which
 * its shared copy's chunks are cut from - opened for the moment it takes to
 * ask, in the leader, which holds the lock the executor took on each of them.
 */
static BlockNumber *
lion_join_part_blocks(LionCountScanState *st, double *total)
{
	BlockNumber *blocks = (BlockNumber *) palloc(sizeof(BlockNumber) *
												 Max(st->npart, 1));
	int			p;

	*total = 0;
	for (p = 0; p < st->npart; p++)
	{
		Relation	heap = table_open(st->part[p].heapoid, NoLock);

		blocks[p] = RelationGetNumberOfBlocks(heap);
		table_close(heap, NoLock);
		*total += (double) blocks[p];
	}
	return blocks;
}

void
lion_initialize_dsm(CustomScanState *node, ParallelContext *pcxt,
					void *coordinate)
{
	LionCountScanState *st = (LionCountScanState *) node;
	LionJoinShared *shared = (LionJoinShared *) coordinate;

	memset(shared, 0, sizeof(LionJoinShared));
	SpinLockInit(&shared->mutex);
	shared->filterrows = -1;
	pg_atomic_init_u32(&shared->nextchunk, 0);
	st->shared = shared;

	/*
	 * Every participant locates the fact filters for itself, so each takes
	 * its share of the list pin budget (DESIGN.md §15, "The pin budget"):
	 * the workers planned and the leader, whether it takes part or not.
	 */
	shared->participants = pcxt->nworkers + 1;
	lion_list_pin_participants(shared->participants);

	/*
	 * A parallel GROUP BY's ranges: a few for each participant the plan was
	 * made for, whether they all start or not - the ones that do claim the
	 * ranges of the ones that do not.
	 */
	if (st->ranged != NULL)
		shared->nranges = lion_key_ranges(RelationGetNumberOfBlocks(st->heap),
										  shared->participants,
										  lion_parallel_range_keys,
										  &shared->ckeys);

	/*
	 * The copy of the fact filters, when the plan collects them: its memory
	 * is a Parallel Hash's, a hash table's for each participant the plan was
	 * made for.  Not where the DSM could not be made (pcxt->seg is NULL, and
	 * the plan runs in the leader alone, which then makes a copy of its own).
	 * The leader keeps its parallel context, which says once the workers are
	 * launched whether any of them started.
	 */
	st->joinpcxt = pcxt;
	if (st->joincollect && pcxt->seg != NULL && st->joinpart == NULL)
	{
		lion_shared_copy_init(LION_JOIN_SHARED_COPY(shared), pcxt->seg,
							  shared->participants,
							  get_hash_memory_limit() * (Size) shared->participants,
							  RelationGetNumberOfBlocks(st->heap));
		shared->copyready = true;
		st->joinsharedcopy = LION_JOIN_SHARED_COPY(shared);
	}

	/*
	 * A partitioned fact table has a copy per leaf partition, each collected
	 * by the participants together when they first take a batch to it; the
	 * memory a Parallel Hash would have is divided among them by their heaps'
	 * sizes, and past its share each copy's chunks go to its own files.
	 */
	if (st->joincollect && pcxt->seg != NULL && st->joinpart != NULL)
	{
		Size		memory = get_hash_memory_limit() * (Size) shared->participants;
		double		total;
		BlockNumber *blocks = lion_join_part_blocks(st, &total);
		int			p;

		for (p = 0; p < st->npart; p++)
		{
			double		frac = (total > 0) ? (double) blocks[p] / total :
				1.0 / st->npart;

			lion_shared_copy_init(LION_JOIN_SHARED_PART_COPY(shared, p),
								  pcxt->seg, shared->participants,
								  (Size) ((double) memory * frac), blocks[p]);
			st->joinpart[p].shared = LION_JOIN_SHARED_PART_COPY(shared, p);
		}
		pfree(blocks);
		shared->copyready = true;
	}
}

/*
 * Before the workers are launched again for a rescan.  The sums are kept:
 * they are the whole execution's, as the leader's own counters are.  The runs
 * of distinct keys a forward semi join's participants claim start again from
 * the first, since the rescan's keys are sorted again (and may be others), and
 * so do a parallel GROUP BY's ranges of keys.
 * No participant is running: the Gather has shut the workers down, and the
 * leader's own node claims nothing before the Gather launches them again.
 *
 * The fact filters' shared copy is emptied - its memory freed, its files
 * deleted - and collected again by the next run, whose parameters may be
 * others.  The leader's view of it goes first: the rescan of its own node,
 * which would let it go too, may come after this (ExecReScanGather()).
 */
void
lion_reinitialize_dsm(CustomScanState *node, ParallelContext *pcxt,
					  void *coordinate)
{
	LionCountScanState *st = (LionCountScanState *) node;
	LionJoinShared *shared = (LionJoinShared *) coordinate;

	pg_atomic_write_u32(&shared->nextchunk, 0);
	SpinLockAcquire(&shared->mutex);
	shared->sortedruns += shared->sorted;
	shared->sorted = 0;
	SpinLockRelease(&shared->mutex);

	/* a parallel GROUP BY's ranges, cut again over the heap as it is now */
	if (st->ranged != NULL)
		shared->nranges = lion_key_ranges(RelationGetNumberOfBlocks(st->heap),
										  shared->participants,
										  lion_parallel_range_keys,
										  &shared->ckeys);

	if (st->joinsharedcopy != NULL)
	{
		if (st->joinviewshared)
		{
			lion_posting_set_release(&st->joinfilter);
			st->joinviewshared = false;
			st->joinfiltered = false;
			st->joincollected = false;
		}
		lion_shared_copy_reinit(st->joinsharedcopy,
								node->ss.ps.state->es_query_dsa,
								RelationGetNumberOfBlocks(st->heap));
	}

	/* ... one per leaf partition of a partitioned fact table */
	if (st->joinpart != NULL && shared->copyready)
	{
		double		total;
		BlockNumber *blocks = lion_join_part_blocks(st, &total);
		int			p;

		for (p = 0; p < st->npart; p++)
		{
			LionJoinPart *jp = &st->joinpart[p];

			if (jp->viewshared)
			{
				lion_posting_set_release(&jp->filter);
				jp->viewshared = false;
				jp->filtered = false;
				jp->collected = false;
			}
			lion_shared_copy_reinit(jp->shared,
									node->ss.ps.state->es_query_dsa,
									blocks[p]);
		}
		pfree(blocks);
	}
}

void
lion_initialize_worker(CustomScanState *node, shm_toc *toc, void *coordinate)
{
	LionCountScanState *st = (LionCountScanState *) node;

	st->shared = (LionJoinShared *) coordinate;
	lion_list_pin_participants(st->shared->participants);

	/* the fact filters' copy, and the file set its chunks may spill to */
	if (st->shared->copyready && st->joinpart == NULL)
	{
		st->joinsharedcopy = LION_JOIN_SHARED_COPY(st->shared);
		lion_shared_copy_attach(st->joinsharedcopy);
	}

	/* ... or each leaf partition's */
	if (st->shared->copyready && st->joinpart != NULL)
	{
		int			p;

		for (p = 0; p < st->npart; p++)
		{
			st->joinpart[p].shared =
				LION_JOIN_SHARED_PART_COPY(st->shared, p);
			lion_shared_copy_attach(st->joinpart[p].shared);
		}
	}
}

/*
 * The end of a participant's execution.  A worker adds its counters to the
 * shared sums, once.  The leader takes a copy of them for EXPLAIN: its
 * ExecShutdownNode() reaches this node before the Gather above it, so before
 * the dynamic shared memory is detached, and after the Gather has read every
 * worker's last row - which it does only once they have all finished
 * (gather_readnext() shuts them down), and every shape above this node reads
 * all of it.  A Gather stopped early would leave some worker's counters out
 * of EXPLAIN, and nothing else.
 *
 * A participant's view of the shared copy of the fact filters goes here as
 * well: the Gather's shutdown, which comes next, detaches the memory it
 * points into and deletes the files it has open.
 */
void
lion_shutdown_custom_scan(CustomScanState *node)
{
	LionCountScanState *st = (LionCountScanState *) node;
	LionJoinShared *shared = st->shared;
	LionRangedState *rs = st->ranged;
	int			i;

	if (shared == NULL)
		return;

	if (st->joinviewshared)
	{
		lion_posting_set_release(&st->joinfilter);
		st->joinviewshared = false;
	}
	if (st->joinpart != NULL)
	{
		int			p;

		for (p = 0; p < st->npart; p++)
		{
			LionJoinPart *jp = &st->joinpart[p];

			if (jp->viewshared)
			{
				lion_posting_set_release(&jp->filter);
				jp->viewshared = false;
				jp->filtered = false;
				jp->collected = false;
			}
		}
	}

	if (IsParallelWorker())
	{
		if (st->joinreported)
			return;
		SpinLockAcquire(&shared->mutex);
		lion_count_stats_add(&shared->stats, &st->stats);
		shared->lookups += st->joinlookups;
		shared->missing += st->joinmissing;
		shared->dirpages += st->dirpages;
		shared->filterrows = Max(shared->filterrows, st->joinfilterrows);
		shared->spilled += st->joinspilled;
		shared->sorted = Max(shared->sorted, st->joinsorted);
		shared->batches += st->joinbatches;
		shared->childrows += st->joinchildrows;
		shared->posting += st->joinposting;
		shared->copies += st->joincopies;
		shared->copychunks += st->joincopychunks;
		shared->switches += st->joinswitches;
		shared->switchkeys += st->joinswitchkeys;
		for (i = 0; i < LION_JT_N; i++)
			INSTR_TIME_ADD(shared->time[i], st->jointime[i]);
		if (rs != NULL)
			shared->ranges += rs->ranges;
		shared->wherecollected += st->wherecollected;
		shared->wherespilled += st->wherespilled;
		shared->groupbatches += st->groupbatches;
		shared->groupsbatched += st->groupsbatched;
		if (st->fg != NULL)
			shared->factgroupcounts += st->fg->groupcounts;
		SpinLockRelease(&shared->mutex);
		st->joinreported = true;
		return;
	}

	SpinLockAcquire(&shared->mutex);
	st->joinworkerstats = shared->stats;
	st->joinworkerlookups = shared->lookups;
	st->joinworkermissing = shared->missing;
	st->joinworkerdirpages = shared->dirpages;
	st->joinworkerfilterrows = shared->filterrows;
	st->joinworkerspilled = shared->spilled;
	st->joinworkersorted = shared->sortedruns + shared->sorted;
	st->joinworkerbatches = shared->batches;
	st->joinworkerchildrows = shared->childrows;
	st->joinworkerposting = shared->posting;
	st->joinworkercopies = shared->copies;
	st->joinworkercopychunks = shared->copychunks;
	st->joinworkerswitches = shared->switches;
	st->joinworkerswitchkeys = shared->switchkeys;
	for (i = 0; i < LION_JT_N; i++)
		st->joinworkertime[i] = shared->time[i];
	if (rs != NULL)
	{
		rs->workerranges = shared->ranges;
		rs->workerwherecollected = shared->wherecollected;
		rs->workerwherespilled = shared->wherespilled;
		rs->workergroupbatches = shared->groupbatches;
		rs->workergroupsbatched = shared->groupsbatched;
	}
	if (st->fg != NULL)
		st->fg->workergroupcounts = shared->factgroupcounts;
	SpinLockRelease(&shared->mutex);
}

void
lion_end_custom_scan(CustomScanState *node)
{
	LionCountScanState *st = (LionCountScanState *) node;

	lion_reset_run(st);

	/* the leader's share of the list pin budget is its whole again */
	if (st->shared != NULL && !IsParallelWorker())
		lion_list_pin_participants(1);

	if (st->child != NULL)
	{
		ExecEndNode(st->child);
		st->child = NULL;
	}
	st->childslot = NULL;

	/*
	 * A plain table's relations were opened once and are closed once, and so
	 * were every partition's (lion_reset_run() has ended the current turn).
	 */
	if (st->npart == 0)
		lion_close_relation(st);
	else
		lion_close_parts(st);

	if (st->pergroup != NULL)
	{
		MemoryContextDelete(st->pergroup);
		st->pergroup = NULL;
	}
	if (st->outercxt != NULL)
	{
		MemoryContextDelete(st->outercxt);
		st->outercxt = NULL;
	}
	if (st->innercxt != NULL)
	{
		MemoryContextDelete(st->innercxt);
		st->innercxt = NULL;
	}
	if (st->inner != NULL)
	{
		st->inner->key = NULL;
		st->inner->isnull = NULL;
		st->inner->nkey = 0;
	}
	if (st->wherecxt != NULL)
	{
		MemoryContextDelete(st->wherecxt);
		st->wherecxt = NULL;
	}
	if (st->keycxt != NULL)
	{
		MemoryContextDelete(st->keycxt);
		st->keycxt = NULL;
	}
	if (st->valcxt != NULL)
	{
		MemoryContextDelete(st->valcxt);
		st->valcxt = NULL;
	}
	if (st->joinkeycxt != NULL)
	{
		MemoryContextDelete(st->joinkeycxt);
		st->joinkeycxt = NULL;
	}
	if (st->joinbatchcxt != NULL)
	{
		MemoryContextDelete(st->joinbatchcxt);
		st->joinbatchcxt = NULL;
	}
	if (st->joinvisitcxt != NULL)
	{
		MemoryContextDelete(st->joinvisitcxt);
		st->joinvisitcxt = NULL;
	}
	if (st->joinpartcxt != NULL)
	{
		MemoryContextDelete(st->joinpartcxt);
		st->joinpartcxt = NULL;
	}
	st->joinpart = NULL;
	if (st->fg != NULL)
	{
		LionFactGroupState *fg = st->fg;

		if (fg->cxt != NULL)
		{
			MemoryContextDelete(fg->cxt);
			fg->cxt = NULL;
		}
		if (fg->rowcxt != NULL)
		{
			MemoryContextDelete(fg->rowcxt);
			fg->rowcxt = NULL;
		}
	}
	if (st->viscache != NULL)
	{
		lion_vis_cache_destroy(st->viscache);
		st->viscache = NULL;
	}
}
