/*-------------------------------------------------------------------------
 *
 * lion_exec_begin.c
 *		Starting a LionCount scan: the scan state, the relations it reads, and
 *		the plan's private data decoded and checked.
 *
 * Part of the LionCount custom scan: lion_customscan.h describes the node
 * and declares what its files share.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "lion_customscan.h"
#include "lion_plan_private.h"

static const CustomExecMethods lion_count_exec_methods = {
	.CustomName = "LionCount",
	.BeginCustomScan = lion_begin_custom_scan,
	.ExecCustomScan = lion_exec_custom_scan,
	.EndCustomScan = lion_end_custom_scan,
	.ReScanCustomScan = lion_rescan_custom_scan,
	.EstimateDSMCustomScan = lion_estimate_dsm,
	.InitializeDSMCustomScan = lion_initialize_dsm,
	.ReInitializeDSMCustomScan = lion_reinitialize_dsm,
	.InitializeWorkerCustomScan = lion_initialize_worker,
	.ShutdownCustomScan = lion_shutdown_custom_scan,
	.ExplainCustomScan = lion_explain_custom_scan,
};

/* =====================================================================
 * Executor
 * ===================================================================== */

Node *
lion_create_custom_scan_state(CustomScan *cscan)
{
	LionCountScanState *st = (LionCountScanState *)
		newNode(sizeof(LionCountScanState), T_CustomScanState);

	st->css.methods = &lion_count_exec_methods;
	return (Node *) st;
}

/*
 * Open one relation's heap and indexes: a plain table once, or one partition
 * for the length of its own processing (DESIGN.md §16).
 *
 * The executor holds a lock on every range table entry of the plan, and the
 * partitions are range table entries too (the planner locked them when it
 * expanded the parent, and AcquireExecutorLocks() relocks the whole flat
 * range table for a cached plan), so nothing here takes a relation lock.  The
 * indexes are not range table entries and get their own AccessShareLock.
 */
/*
 * Read the KEYS of the inner index of a two-column GROUP BY, once for this
 * relation (DESIGN.md §20).
 *
 * Only the keys: an entry's head block or INLINE payload would be worthless
 * without the buffer pin that goes with it (DESIGN.md §9), and holding one pin
 * per distinct inner value for the whole scan is exactly what the pin budget
 * forbids.  So each pair re-locates its inner posting set with
 * lion_posting_set_lookup(), which is one bucket page - and the bucket count
 * is sized to the index's entry count, so that lookup is a hit in shared
 * buffers for every index small enough for the cost model to have chosen this
 * plan at all.
 *
 * The alternative, walking the inner index's entry scan once per OUTER group,
 * needs no memory but re-reads every bucket page of the inner index
 * outer_entries times.  It is kept as the fallback for the case the keys do
 * not fit the work_mem budget - a grouping the cost model did not expect, the
 * §16 lesson that a plan-time bound is only as good as estimate_num_groups -
 * and then innerkey is left NULL.
 */
static void
lion_load_inner_keys(LionCountScanState *st)
{
	LionEntryScan es;
	LionState  *istate = lion_index_column_state(st->groupidx2,
												st->groupidxcol2);
	MemoryContext oldcxt;
	MemoryContext tmpcxt;
	Size		budget = (Size) work_mem * INT64CONST(1024);
	int			cap = 64;
	int			n = 0;
	Datum	   *keys;
	bool	   *isnull;
	bool		full = false;

	Assert(st->innerkey == NULL);

	MemoryContextReset(st->innercxt);
	oldcxt = MemoryContextSwitchTo(st->innercxt);
	keys = (Datum *) palloc(sizeof(Datum) * cap);
	isnull = (bool *) palloc(sizeof(bool) * cap);
	MemoryContextSwitchTo(oldcxt);

	tmpcxt = AllocSetContextCreate(CurrentMemoryContext,
								   "LionCount inner entry scan",
								   ALLOCSET_SMALL_SIZES);

	lion_entry_scan_begin_col(&es, st->groupidx2, st->groupidxcol2);
	for (;;)
	{
		LionPostingSet ps;
		Datum		key;

		CHECK_FOR_INTERRUPTS();

		MemoryContextReset(tmpcxt);
		oldcxt = MemoryContextSwitchTo(tmpcxt);
		if (!lion_entry_scan_next(&es, &key, &ps))
		{
			MemoryContextSwitchTo(oldcxt);
			break;
		}
		MemoryContextSwitchTo(st->innercxt);
		if (n >= cap)
		{
			cap *= 2;
			keys = (Datum *) repalloc(keys, sizeof(Datum) * cap);
			isnull = (bool *) repalloc(isnull, sizeof(bool) * cap);
		}
		isnull[n] = ps.keyisnull;
		keys[n] = ps.keyisnull ? (Datum) 0 :
			datumCopy(key, istate->typbyval, istate->typlen);
		n++;
		MemoryContextSwitchTo(oldcxt);

		/* The set's pin goes now: only the key is kept. */
		lion_posting_set_release(&ps);

		if ((n & 0xff) == 0 &&
			MemoryContextMemAllocated(st->innercxt, true) > budget)
		{
			full = true;
			break;
		}
	}
	lion_entry_scan_end(&es);
	MemoryContextDelete(tmpcxt);

	if (full)
	{
		/* Too many to keep: walk the inner index per outer group instead. */
		MemoryContextReset(st->innercxt);
		st->innerkey = NULL;
		st->innerisnull = NULL;
		st->ninnerkey = 0;
		return;
	}

	st->innerkey = keys;
	st->innerisnull = isnull;
	st->ninnerkey = n;
}

/*
 * The KEY COLUMN (1-based) of `index` that holds heap column `heapattno`
 * (DESIGN.md §24).  A single-column index answers 1 for its own column and
 * nothing else; a multicolumn one is searched, because the columns may be in
 * any order - and, with partitions, in a DIFFERENT order in each of them.
 *
 * The search is the planner's own (lion_find_roaring_index()): the first key
 * column on that heap column whose opclass is multi-key exactly when
 * `multikey` says so, which is how the executor arrives at the very column
 * the plan was made for without the plan carrying it.  An index may list one
 * heap column twice, under a multi-key and a scalar opclass - `(tags
 * array_ops, tags <a whole-array class>)` - and the two hold different
 * entries, elements under one and whole arrays under the other.  The first
 * column on the heap column used to be taken whatever its opclass, so
 * `count(DISTINCT tags)` counted elements, and a multi-key query was handed
 * to a scalar column that has no extraction function at all (the 2026-09-27
 * review).  The kind a caller needs is always known: multi-key for a
 * multi-key clause (LION_CLAUSE_MULTI), scalar for every other clause and for
 * every index whose entries drive the scan.  Two columns of the SAME kind on
 * one heap column are told apart the way the planner tells them apart, by
 * position: it matches the first and never tries the second
 * (lion_match_index()).  An index with no such column is not the one the
 * plan was made for, and that is said rather than read.
 */
AttrNumber
lion_index_col_for(Relation index, AttrNumber heapattno, bool multikey)
{
	int			c;

	for (c = 0; c < IndexRelationGetNumberOfKeyAttributes(index); c++)
	{
		if (index->rd_index->indkey.values[c] != heapattno)
			continue;
		if (lion_opfamily_is_multikey(index->rd_opfamily[c],
									 index->rd_opcintype[c]) != multikey)
			continue;
		return (AttrNumber) (c + 1);
	}

	elog(ERROR, "lion index \"%s\" has no %s key column on column %d of \"%s\"",
		 RelationGetRelationName(index), multikey ? "multi-key" : "scalar",
		 (int) heapattno, get_rel_name(index->rd_index->indrelid));
	return 0;					/* keep the compiler quiet */
}

/*
 * The attribute number heap column `parentattno` of `parentoid` has in `heap`.
 *
 * Everything the plan carries is in the PARENT's numbering (DESIGN.md §16),
 * and a partition may number its columns differently - so the column an
 * index's indkey names has to be translated before it can be looked for
 * there.  Partitions match their parent's columns BY NAME, which is the same
 * mapping the executor's own tuple conversion uses.
 */
AttrNumber
lion_heap_attno_in(Relation heap, Oid parentoid, AttrNumber parentattno)
{
	char	   *name;
	AttrNumber	attno;

	if (RelationGetRelid(heap) == parentoid || parentattno <= 0)
		return parentattno;

	name = get_attname(parentoid, parentattno, false);
	attno = get_attnum(RelationGetRelid(heap), name);
	if (attno == InvalidAttrNumber)
		elog(ERROR, "relation \"%s\" has no column \"%s\"",
			 RelationGetRelationName(heap), name);
	pfree(name);

	return attno;
}

/*
 * The relations the statement this node runs in modifies or row-locks, by
 * Oid: its result relations and its row marks, which are exactly the range
 * table entries ScanRelIsReadOnly() tests a core scan's relation against.
 */
static List *
lion_statement_written_rels(EState *estate)
{
	PlannedStmt *pstmt = estate->es_plannedstmt;
	Bitmapset  *rtis;
	List	   *oids = NIL;
	int			rti = -1;

	if (pstmt == NULL)
		return NIL;

	rtis = lion_pstmt_written_rtis(pstmt);
	while ((rti = bms_next_member(rtis, rti)) >= 0)
	{
		RangeTblEntry *rte = exec_rt_fetch((Index) rti, estate);

		if (rte->rtekind == RTE_RELATION && OidIsValid(rte->relid))
			oids = list_append_unique_oid(oids, rte->relid);
	}
	bms_free(rtis);
	return oids;
}

/*
 * Is heap read-only for this statement, in the sense of ScanRelIsReadOnly()?
 * (DESIGN.md §11, "On-access pruning sets the visibility map too.")
 *
 * Core asks whether the SCAN's range table entry is a result relation or has
 * a row mark.  Ours never is: the node is only planted in a SELECT with no
 * row marks, and a relation the statement also modifies - `UPDATE t SET x =
 * (SELECT count(*) FROM t WHERE ...)`, a data-modifying CTE, INSERT ...
 * SELECT - is a different entry of the same table.  So the question is asked
 * by relation instead: the counted table, or for a partition that partition
 * or any of its ancestors (an INSERT routed through the parent names only the
 * parent), must not be among the relations the statement writes or locks.
 * Setting all-visible bits the same statement is about to clear would be
 * wasted work, and that is all this decides: the correctness argument does
 * not depend on it.
 */
static bool
lion_rel_read_only(LionCountScanState *st, Relation heap)
{
	Oid			relid = RelationGetRelid(heap);
	bool		result = true;

	if (st->writtenrels == NIL)
		return true;
	if (list_member_oid(st->writtenrels, relid) ||
		list_member_oid(st->writtenrels, st->heapoid))
		return false;
	if (heap->rd_rel->relispartition)
	{
		List	   *ancestors = get_partition_ancestors(relid);
		ListCell   *lc;

		foreach(lc, ancestors)
		{
			if (list_member_oid(st->writtenrels, lfirst_oid(lc)))
			{
				result = false;
				break;
			}
		}
		list_free(ancestors);
	}
	return result;
}

/*
 * The items - the sources after slot 0 - of the partition whose turn begins:
 * the plan's, less the clauses its bounds imply (DESIGN.md §16, "Clauses the
 * partition bounds imply"), which have no index open (lion_open_parts()).
 * An OR is left out when every leaf of it is, and otherwise located without
 * the arms and leaves the partition leaves out (lion_locate_or()); a range
 * taken as a source stands for all of its bounds, and is left out when every
 * one of them is, and otherwise opened by the first one the partition keeps.
 * The inner group of a two-column GROUP BY follows the items, so its slot
 * moves with their number.
 */
static void
lion_relation_items(LionCountScanState *st)
{
	int			n = 0;
	int			k;

	for (k = 0; k < st->nplanitem; k++)
	{
		LionSourceItem it = st->planitem[k];

		if (it.orno >= 0)
		{
			LionOrState *o = &st->ors[it.orno];
			int			j;

			for (j = 0; j < o->nleaves; j++)
			{
				if (st->clause[o->first + j].idx != NULL)
					break;
			}
			if (j == o->nleaves)
				continue;		/* left out whole */
		}
		else if (it.rangesrc)
		{
			AttrNumber	attno = st->clause[it.clauseno].attno;
			int			j;

			for (j = 0; j < st->nclause; j++)
			{
				if (st->clause[j].kind == LION_CLAUSE_RANGESRC &&
					!st->inor[j] && st->clause[j].attno == attno &&
					st->clause[j].idx != NULL)
					break;
			}
			if (j == st->nclause)
				continue;
			it.clauseno = j;
		}
		else if (st->clause[it.clauseno].idx == NULL)
			continue;
		st->item[n++] = it;
	}
	st->nitem = n;

	st->nsource = n + 1 + (st->innerattno != 0 ? 1 : 0);
	memset(st->sources, 0, sizeof(LionCountSource) * (st->nplanitem + 1 +
													  (st->innerattno != 0 ? 1 : 0)));
	st->sources[0].nsets = 1;
	st->sources[0].sets = &st->groupset;
	if (st->innerattno != 0)
	{
		st->sources[n + 1].nsets = 1;
		st->sources[n + 1].sets = &st->groupset2;
	}
}

/*
 * A heap the node counts, opened without a lock of its own: the executor
 * holds one on every range table entry of the plan, a partition's included
 * (the planner locked it when it expanded the parent, and
 * AcquireExecutorLocks() relocks the whole flat range table for a cached
 * plan).  A parallel worker (DESIGN.md §27, "Parallel") holds no lock of the
 * leader's and takes its own, as ExecGetRangeTableRelation() does for the
 * scans of a worker.
 */
static Relation
lion_open_heap(Oid heapoid)
{
	Relation	heap;

	heap = table_open(heapoid, IsParallelWorker() ? AccessShareLock : NoLock);
	Assert(CheckRelationLockedByMe(heap, AccessShareLock, true));
	return heap;
}

/*
 * An index the node reads.  Indexes are not range table entries, and nothing
 * relocks them for a cached plan, so each gets an AccessShareLock of its own,
 * as ExecInitIndexScan() takes one.
 */
static Relation
lion_open_index(Oid indexoid)
{
	return OidIsValid(indexoid) ? index_open(indexoid, AccessShareLock) : NULL;
}

/*
 * Every relation of every leaf partition (DESIGN.md §16), opened when the
 * node is initialised and kept open until it ends (lion_close_parts()), as
 * the scans under core's Append keep theirs from ExecInitNode() to
 * ExecEndNode().  The relcache references are the point: CheckTableNotInUse()
 * reads them to refuse a TRUNCATE, DROP, REINDEX or ALTER - of the heap or
 * of an index - that this session issues while a query still uses the
 * relation, for instance between two FETCHes of a cursor.  Opened only for
 * its turn, a partition could be emptied or dropped under the cursor between
 * two of them.
 *
 * What each costs is a reference count and a resource owner entry: the
 * relcache entries are the ones the planner built, and nothing is read from
 * storage, since the files are opened when a turn reads a page, and through
 * the virtual file descriptors, which fd.c pools.  The index locks are the
 * ones the planner took for each leaf (get_relation_info()), and the ones an
 * Append of index scans would take for a cached plan.
 */
static void
lion_open_parts(LionCountScanState *st)
{
	int			p;
	int			i;

	for (p = 0; p < st->npart; p++)
	{
		LionPartState *part = &st->part[p];

		part->heap = lion_open_heap(part->heapoid);
		part->groupidx = lion_open_index(part->groupidxoid);
		part->groupidx2 = lion_open_index(part->groupidxoid2);
		part->clauseidx = (Relation *)
			palloc0(sizeof(Relation) * Max(st->nclause, 1));
		for (i = 0; i < st->nclause; i++)
			part->clauseidx[i] = lion_open_index(part->clauseidxoid[i]);
		part->fgidx = lion_open_index(part->fgidxoid);
	}
}

/* ... and closed when the node ends. */
void
lion_close_parts(LionCountScanState *st)
{
	int			p;
	int			i;

	for (p = 0; p < st->npart; p++)
	{
		LionPartState *part = &st->part[p];

		if (part->fgidx != NULL)
			index_close(part->fgidx, AccessShareLock);
		part->fgidx = NULL;
		if (part->clauseidx != NULL)
		{
			for (i = 0; i < st->nclause; i++)
			{
				if (part->clauseidx[i] != NULL)
					index_close(part->clauseidx[i], AccessShareLock);
				part->clauseidx[i] = NULL;
			}
		}
		if (part->groupidx2 != NULL)
			index_close(part->groupidx2, AccessShareLock);
		part->groupidx2 = NULL;
		if (part->groupidx != NULL)
			index_close(part->groupidx, AccessShareLock);
		part->groupidx = NULL;
		if (part->heap != NULL)
			table_close(part->heap, NoLock);
		part->heap = NULL;
	}
}

/*
 * Make a relation the one counted: the plain table (p < 0), whose relations
 * are opened here, once for the life of the node, or leaf partition p for its
 * turn, whose relations have been open since the node was (lion_open_parts()).
 */
void
lion_open_relation(LionCountScanState *st, int p)
{
	LionPartState *part = (p >= 0) ? &st->part[p] : NULL;
	int			i;

	Assert(st->heap == NULL);

	if (part == NULL)
	{
		st->heap = lion_open_heap(st->heapoid);
		for (i = 0; i < st->nclause; i++)
			st->clause[i].idx = index_open(st->clause[i].idxoid,
										   AccessShareLock);
		st->groupidx = lion_open_index(st->groupidxoid);
		st->groupidx2 = lion_open_index(st->groupidxoid2);
		st->fgidx = lion_open_index(st->fgidxoid);
		if (st->decode != NULL)
		{
			LionDecodeRun *dr = st->decode;
			int			c;

			for (c = 0; c < dr->ncol; c++)
				dr->idx[c] = lion_open_index(dr->idxoid[c]);
		}
	}
	else
	{
		Assert(part->heap != NULL);
		st->heap = part->heap;

		/* NULL for a clause the partition's bounds imply (DESIGN.md §16) */
		for (i = 0; i < st->nclause; i++)
			st->clause[i].idx = part->clauseidx[i];
		st->groupidx = part->groupidx;
		st->groupidx2 = part->groupidx2;
	}
	lion_check_table_am(st->heap);	/* the planner declined it; see there */
	st->rel_read_only = lion_rel_read_only(st, st->heap);

	for (i = 0; i < st->nclause; i++)
	{
		if (st->clause[i].idx == NULL)
			continue;
		st->clause[i].idxcol =
			lion_index_col_for(st->clause[i].idx,
							   lion_heap_attno_in(st->heap, st->heapoid,
												  st->clause[i].attno),
							   st->clause[i].kind == LION_CLAUSE_MULTI);
	}
	if (part != NULL)
		lion_relation_items(st);

	/*
	 * The driving index's key column comes from the index that was really
	 * opened rather than from the plan (DESIGN.md §24): the planner only has
	 * to be right about WHICH index, and a partition's own index may put the
	 * same heap column at a different position from the parent's.  A driving
	 * column is always a scalar one: its entries have to be the column's
	 * values, one per row.
	 */
	if (st->groupidx != NULL)
		st->groupidxcol =
			lion_index_col_for(st->groupidx,
							   lion_heap_attno_in(st->heap, st->heapoid,
												  st->driveattno),
							   false);
	if (st->groupidx2 != NULL)
		st->groupidxcol2 =
			lion_index_col_for(st->groupidx2,
							   lion_heap_attno_in(st->heap, st->heapoid,
												  st->innerattno),
							   false);
	if (st->decode != NULL)
	{
		LionDecodeRun *dr = st->decode;
		int			c;

		for (c = 0; c < dr->ncol; c++)
		{
			if (dr->idx[c] != NULL)
				dr->idxcol[c] =
					lion_index_col_for(dr->idx[c],
									   lion_heap_attno_in(st->heap,
														  st->heapoid,
														  dr->attno[c]),
									   false);
		}
	}

	/*
	 * index_beginscan() would take a relation-level predicate lock on each of
	 * these (the AM has no ampredlocks); we read them without a scan, so take
	 * it here, before any lookup, so that absent keys are covered as well.
	 * Without it two SERIALIZABLE transactions could each count an absent
	 * key, insert it, and both commit.
	 */
	{
		Snapshot	snapshot = st->css.ss.ps.state->es_snapshot;

		for (i = 0; i < st->nclause; i++)
		{
			if (st->clause[i].idx != NULL)
				lion_reader_lock(st->clause[i].idx, snapshot);
		}
		if (st->groupidx != NULL)
			lion_reader_lock(st->groupidx, snapshot);
		if (st->groupidx2 != NULL)
			lion_reader_lock(st->groupidx2, snapshot);
		if (st->decode != NULL)
		{
			int			c;

			for (c = 0; c < st->decode->ncol; c++)
			{
				if (st->decode->idx[c] != NULL)
					lion_reader_lock(st->decode->idx[c], snapshot);
			}
		}
	}

	/*
	 * A two-column GROUP BY reads the inner index's keys once for this
	 * relation (DESIGN.md §20); the sets themselves are located per pair,
	 * because each one holds a buffer pin.
	 */
	if (st->groupidx2 != NULL)
		lion_load_inner_keys(st);
}

/*
 * The reverse: the plain table's relations closed, at the end of the node, or
 * a partition's turn over, its relations left open for lion_close_parts().
 * Every posting set of this relation must already have been released:
 * DESIGN.md §9 wants no pin to outlive the relation it belongs to, and a
 * partition's pins must not outlive that partition's turn.
 */
void
lion_close_relation(LionCountScanState *st)
{
	bool		own = (st->npart == 0); /* the relations are closed here */
	int			i;

	/* the counts' map page is this relation's */
	lion_vis_cache_release_vm(st->viscache);

	if (st->groupidx != NULL && own)
		index_close(st->groupidx, AccessShareLock);
	st->groupidx = NULL;
	st->groupidxcol = 0;
	if (st->groupidx2 != NULL && own)
		index_close(st->groupidx2, AccessShareLock);
	st->groupidx2 = NULL;
	st->groupidxcol2 = 0;
	if (st->fgidx != NULL)		/* a plain table's only */
		index_close(st->fgidx, AccessShareLock);
	st->fgidx = NULL;
	if (st->decode != NULL)		/* ... and so is the decoded walk */
	{
		LionDecodeRun *dr = st->decode;
		int			c;

		lion_decode_reset(st);
		for (c = 0; c < dr->ncol; c++)
		{
			if (dr->idx[c] != NULL)
				index_close(dr->idx[c], AccessShareLock);
			dr->idx[c] = NULL;
			dr->idxcol[c] = 0;
		}
	}
	st->innerkey = NULL;
	st->innerisnull = NULL;
	st->ninnerkey = 0;
	if (st->innercxt != NULL)
		MemoryContextReset(st->innercxt);
	for (i = 0; i < st->nclause; i++)
	{
		if (st->clause[i].idx != NULL && own)
			index_close(st->clause[i].idx, AccessShareLock);
		st->clause[i].idx = NULL;
		st->clause[i].idxcol = 0;
	}
	if (st->heap != NULL && own)
		table_close(st->heap, NoLock);
	st->heap = NULL;
}

/*
 * The EXECUTE checks of the plan the node replaces (LION_PRIV_EXECUTE,
 * lion_replaced_functions()), made for the current user every time the node
 * is initialised, as ExecInitNode() makes them for that plan - so a cached
 * plan answers a REVOKE and a SET ROLE the way the ordinary one does.  In
 * core's order: the scan's quals, then the Agg's grouping equality, then its
 * aggregates.
 *
 * Plain EXPLAIN initialises the plan too, and core checks the quals and the
 * aggregates then as well; the grouping equality it checks only when a
 * HashAggregate - the plan a grouped count competes with - builds its hash
 * table, which EXPLAIN does not, so neither does this.
 */
static void
lion_check_replaced_execute(const LionCountPriv *priv, int eflags)
{
	ListCell   *lc;

	foreach(lc, priv->exec_funcs)
		lion_check_execute(lfirst_oid(lc));
	if ((eflags & EXEC_FLAG_EXPLAIN_ONLY) == 0)
	{
		foreach(lc, priv->exec_groupfuncs)
			lion_check_execute(lfirst_oid(lc));
	}
	foreach(lc, priv->exec_aggs)
		lion_check_aggregate_execute(lfirst_oid(lc));
}

/*
 * The plan's Oids, attribute numbers and flags, from custom_private as
 * lion_count_priv_decode() took it apart (lion_plan_private.h).
 */
static void
lion_begin_plan(LionCountScanState *st, const LionCountPriv *priv)
{
	st->implied = priv->implied;
	st->heapoid = priv->heapoid;
	st->groupidxoid = priv->groupidxoid;
	st->groupidxoid2 = priv->groupidxoid2;
	st->scanrelid = priv->scanrelid;
	st->groupattno = priv->groupattno;
	st->groupattno2 = priv->groupattno2;
	st->singlegroup = (priv->flags & LION_FLAG_SINGLEGROUP) != 0;
	st->sumall = (priv->flags & LION_FLAG_SUMALL) != 0;
	st->hasgroupidx = (priv->flags & LION_FLAG_GROUPIDX) != 0;
	st->hasrange = (priv->flags & LION_FLAG_RANGE) != 0;
	st->nclause = priv->nclause;
	st->distattno = priv->distattno;
	st->allattno = priv->allattno;
}

/*
 * The top k by count (DESIGN.md §36) is made of one grouping column walked
 * in one table, counted a group at a time.  Its state is there only when the
 * plan has a k.
 */
static void
lion_begin_topk(LionCountScanState *st, const LionCountPriv *priv,
				EState *estate)
{
	LionTopkState *tk;

	st->topk = NULL;
	if (priv->topkn <= 0)
		return;

	tk = (LionTopkState *) MemoryContextAllocZero(estate->es_query_cxt,
												  sizeof(LionTopkState));
	tk->n = priv->topkn;
	tk->cand = priv->topkcand;
	tk->strict = priv->topkstrict;
	st->topk = tk;
}

/* GROUP BY coalesce(g, c) (DESIGN.md §10): c and its equality */
static void
lion_begin_coalesce(LionCountScanState *st, const LionCountPriv *priv,
					EState *estate)
{
	if (priv->coalconst != NULL)
	{
		st->coalconst = priv->coalconst;
		st->coaleqop = priv->coaleqop;
		st->coalcoll = priv->coalcoll;
		fmgr_info_cxt(get_opcode(st->coaleqop), &st->coaleqfn,
					  estate->es_query_cxt);
		st->hascoal = true;
	}
}

/*
 * The FK-side join (DESIGN.md §27): which clause is the join key, and
 * which column of the child's rows carries its value.  Every other shape
 * has neither, and no child.
 */
static void
lion_begin_join(LionCountScanState *st, const LionCountPriv *priv)
{
	st->joinclause = -1;
	st->jointype = LION_JOIN_INNER;
	if (priv->hasjoin)
	{
		int			flags = priv->join.flags;

		st->joinclause = priv->join.clause;
		st->jointype = priv->join.type;
		st->joincollect = (flags & LION_JOINFLAG_COLLECT) != 0;
		st->joinrows = (flags & LION_JOINFLAG_ROWS) != 0;
		st->joinsum = (flags & LION_JOINFLAG_SUM) != 0;
		st->joincounts = (flags & LION_JOINFLAG_COUNTS) != 0;
		st->joinouter = (flags & LION_JOINFLAG_OUTER) != 0;
		st->joinordered = (flags & LION_JOINFLAG_ORDERED) != 0;
		st->joinunique = (flags & LION_JOINFLAG_UNIQUE) != 0;
		st->joinwalk = (flags & LION_JOINFLAG_WALK) != 0;
		st->joinsortop = priv->join.sortop;
		st->joinsortcoll = priv->join.sortcoll;
		st->joinkeyresno = priv->join.keyresno;
	}
}

/*
 * The kinds of the plan's target list (LION_TL_*), and what they ask of the
 * scan.
 */
static void
lion_begin_target_list(LionCountScanState *st, CustomScan *cscan,
					   const LionCountPriv *priv)
{
	int			i;

	st->ntlist = priv->ntl;
	st->tlkind = priv->tlkind;

	/*
	 * A summed join's one row stands for no dimension row (LION_JOINFLAG_SUM),
	 * so what the plan's target list reads of it must be counts alone - the
	 * join key's column is in custom_scan_tlist for the clause's value, and is
	 * read by nothing else.  Said, rather than trusted, like a malformed join.
	 */
	if (st->joinsum)
	{
		List	   *vars = pull_var_clause((Node *) cscan->scan.plan.targetlist,
										   PVC_RECURSE_AGGREGATES |
										   PVC_RECURSE_WINDOWFUNCS |
										   PVC_RECURSE_PLACEHOLDERS);
		ListCell   *lc;

		foreach(lc, vars)
		{
			Var		   *var = (Var *) lfirst(lc);

			if (var->varno != INDEX_VAR || var->varattno < 1 ||
				var->varattno > st->ntlist ||
				st->tlkind[var->varattno - 1] != LION_TL_COUNT)
				elog(ERROR, "LionCount: a summed join reads more than its counts");
		}
		list_free(vars);
	}

	/*
	 * Which tests of a count(DISTINCT k) walk have to COUNT rather than stop
	 * at the first visible row (DESIGN.md §26), from what the target list -
	 * the HAVING's counts included - asks for.  Without a GROUP BY every
	 * other count is a sum over k's entries; beside one, count(k) is a sum
	 * over the (g, k) pairs, and count(*) and count(g) are the group's own.
	 */
	for (i = 0; st->distattno != 0 && i < st->ntlist; i++)
	{
		switch (st->tlkind[i])
		{
			case LION_TL_COUNT_DISTCOL:
				st->distfull = true;
				break;
			case LION_TL_COUNT:
			case LION_TL_COUNT_GROUPCOL:
			case LION_TL_COUNT_GROUPCOL2:
				if (st->groupattno == 0)
					st->distfull = true;
				else
					st->distgroupcount = true;
				break;
			default:
				break;
		}
	}
}

/*
 * The aggregates over lion columns' entries (DESIGN.md §37), from the plan's
 * WAGG member and the argument expressions after the clause values: the
 * columns, each aggregate's kind, argument and - for a minimum or a maximum -
 * its sort operator, and where each column's key goes in the scan tuple.
 * Whether the target list has counts besides, which the sum over every row
 * answers.
 */
static void
lion_begin_wagg(LionCountScanState *st, CustomScanState *node,
				const LionCountPriv *priv, List *exprs)
{
	int			i;

	st->nwcol = 0;
	st->nwagg = 0;
	if (priv->nwcol == 0)
		return;

	st->nwcol = priv->nwcol;
	st->wcol = (LionWCol *) palloc0(sizeof(LionWCol) * st->nwcol);
	for (i = 0; i < st->nwcol; i++)
	{
		st->wcol[i].attno = priv->wcol[i].attno;
		st->wcol[i].idxoid = priv->wcol[i].idxoid;
		st->wcol[i].idxcol = priv->wcol[i].idxcol;
		st->wcol[i].slotcol = -1;
	}

	st->nwagg = priv->nwagg;
	st->wagg = (LionWAgg *) palloc0(sizeof(LionWAgg) * Max(st->nwagg, 1));
	for (i = 0; i < st->nwagg; i++)
	{
		LionWAgg   *a = &st->wagg[i];
		Expr	   *arg = (Expr *) list_nth(exprs, st->nclause + i);
		Node	   *bare = lion_strip((Node *) arg);

		a->kind = priv->wagg[i].kind;
		a->col = priv->wagg[i].col;
		a->argwidth = priv->wagg[i].argwidth;
		a->arg = ExecInitExpr(arg, &node->ss.ps);
		a->argiskey = (bare != NULL && IsA(bare, Var));
		get_typlenbyval(exprType((Node *) arg), &a->typlen, &a->typbyval);
		if (a->kind == LION_WAGG_EXTREME)
		{
			Oid			aggfnoid = priv->wagg[i].aggfnoid;
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
			a->collation = priv->wagg[i].collation;
		}
	}

	/* each column's key in the scan tuple, and whether counts are wanted */
	st->wneedcount = false;
	for (i = 0; i < st->ntlist; i++)
	{
		int			kind = st->tlkind[i];

		if (LION_TL_IS_WKEY(kind))
			st->wcol[LION_TL_WKEY_COL(kind)].slotcol = i;
		else if (LION_TL_IS_WAGG(kind))
			continue;
		else
			st->wneedcount = true;
	}
	for (i = 0; i < st->nwagg; i++)
	{
		if (!st->wagg[i].argiskey && st->wcol[st->wagg[i].col].slotcol < 0)
			elog(ERROR, "LionCount: an aggregate's argument without its key");
	}
}

/*
 * The plan's clauses, one LionClauseState each: what kind, on which column
 * and index, under which operator, and how its value is had.
 */
static void
lion_begin_clauses(LionCountScanState *st, CustomScanState *node,
				   const LionCountPriv *priv, List *exprs)
{
	int			i;

	st->clause = (LionClauseState *)
		palloc0(sizeof(LionClauseState) * Max(st->nclause, 1));
	for (i = 0; i < st->nclause; i++)
	{
		LionClauseState *cl = &st->clause[i];

		cl->kind = priv->clause[i].kind;
		cl->idxoid = priv->clause[i].idxoid;
		cl->attno = priv->clause[i].attno;
		cl->opno = priv->clause[i].opno;
		cl->strategy = 0;

		/*
		 * A literal's value is ready now and never changes, so it is taken
		 * straight from the Const; anything else - a Param, the ARRAY[] of a
		 * generic IN list, a stable expression - gets an ExprState and is
		 * evaluated at the start of each scan (lion_eval_clause_values()).
		 */
		cl->valexpr = (Expr *) list_nth(exprs, i);
		cl->valtype = exprType((Node *) cl->valexpr);

		/*
		 * The join key's value is not evaluated at all: it is a column of the
		 * child's current row, read per row (lion_next_join_row()).  Its
		 * expression is kept for EXPLAIN, which deparses it as the dimension
		 * column it references.
		 */
		if (i == st->joinclause)
		{
			cl->con = NULL;
			cl->valstate = NULL;
			continue;
		}

		if (IsA(cl->valexpr, Const))
		{
			cl->con = (Const *) cl->valexpr;
			cl->val = cl->con->constvalue;
			cl->valisnull = cl->con->constisnull;
		}
		else
		{
			cl->con = NULL;
			cl->valstate = ExecInitExpr(cl->valexpr, &node->ss.ps);
		}
	}
}

/*
 * The heap columns the driving walk and the inner walk are on: the driving
 * index's entries' (DESIGN.md §24 needs it to name that index's KEY COLUMN)
 * and the inner side's of a nested loop (§20, §26).  lion_count_priv_check()
 * has seen that the range clauses bound the first.
 */
static void
lion_begin_driving_column(LionCountScanState *st, const LionCountPriv *priv)
{
	st->driveattno = lion_count_priv_drive_attno(priv);
	st->innerattno = lion_count_priv_inner_attno(priv);
}

/*
 * The OR restrictions (DESIGN.md §19) and the sources the clauses make
 * up: one per plain clause, one per OR.  A clause that is an OR leaf has
 * no source of its own - its posting sets go into the OR's, which is the
 * union of the arms - and that is the only thing that tells the two apart
 * anywhere below.
 */
static void
lion_begin_ors_and_items(LionCountScanState *st, const LionCountPriv *priv)
{
	int			i;
	int			k;

	st->nor = priv->nor;
	st->ors = priv->ors;
	st->inor = (bool *) palloc0(sizeof(bool) * Max(st->nclause, 1));
	for (i = 0; i < st->nor; i++)
	{
		for (k = 0; k < st->ors[i].nleaves; k++)
			st->inor[st->ors[i].first + k] = true;
	}

	st->item = (LionSourceItem *)
		palloc0(sizeof(LionSourceItem) * Max(st->nclause + 1, 1));
	st->nitem = 0;
	for (i = 0; i < st->nclause; i++)
	{
		int			orno = -1;

		/* The join key is looked up per child row, into slot 0, not here. */
		if (i == st->joinclause)
			continue;

		/* A range bounds the driving walk and is no source (DESIGN.md §28). */
		if (st->clause[i].kind == LION_CLAUSE_RANGE)
			continue;

		/*
		 * A range taken as a source (DESIGN.md §32) is ONE source however many
		 * bounds it has: the first bound on its column opens it, for all of
		 * them.  Under an OR the same goes per arm (lion_locate_or()).
		 */
		st->item[st->nitem].rangesrc = false;
		if (st->clause[i].kind == LION_CLAUSE_RANGESRC && !st->inor[i])
		{
			for (k = 0; k < i; k++)
			{
				if (st->clause[k].kind == LION_CLAUSE_RANGESRC &&
					!st->inor[k] && st->clause[k].attno == st->clause[i].attno)
					break;
			}
			if (k < i)
				continue;
			st->item[st->nitem].rangesrc = true;
		}

		if (st->inor[i])
		{
			/* Only the FIRST leaf of an OR opens a source, for the whole OR. */
			for (k = 0; k < st->nor; k++)
			{
				if (st->ors[k].first == i)
				{
					orno = k;
					break;
				}
			}
			if (orno < 0)
				continue;
		}

		st->item[st->nitem].clauseno = i;
		st->item[st->nitem].orno = orno;
		st->nitem++;
	}
	st->planitem = st->item;
	st->nplanitem = st->nitem;
}

/* One target per live leaf partition, in the planner's order. */
static void
lion_begin_partitions(LionCountScanState *st, const LionCountPriv *priv)
{
	int			i;

	st->npart = priv->npart;
	if (st->npart > 0)
	{
		st->part = (LionPartState *) palloc0(sizeof(LionPartState) * st->npart);
		for (i = 0; i < st->npart; i++)
		{
			/* InvalidOid for a clause the partition leaves out (§16) */
			st->part[i].heapoid = priv->part[i].heapoid;
			st->part[i].groupidxoid = priv->part[i].groupidxoid;
			st->part[i].groupidxoid2 = priv->part[i].groupidxoid2;
			st->part[i].clauseidxoid = priv->part[i].clauseidxoid;
		}

		/* ... whose items are the plan's, less what each leaves out */
		st->item = (LionSourceItem *)
			palloc0(sizeof(LionSourceItem) * Max(st->nplanitem, 1));
		memcpy(st->item, st->planitem,
			   sizeof(LionSourceItem) * st->nplanitem);
	}
}

/*
 * An FK-side join grouped by a fact column (DESIGN.md §27, "Grouped by a
 * fact column"): the column, and each relation's index for its groups -
 * or, for a partition, the value its bounds give the column.
 */
static void
lion_begin_fact_group(LionCountScanState *st, const LionCountPriv *priv)
{
	int			i;

	if (priv->fgattno != 0)
	{
		st->fgattno = priv->fgattno;
		if (st->npart == 0)
			st->fgidxoid = priv->fgidxoid[0];
		for (i = 0; i < st->npart; i++)
		{
			st->part[i].fgidxoid = priv->fgidxoid[i];
			st->part[i].fgconst = OidIsValid(priv->fgidxoid[i]) ? NULL :
				priv->fgconst[i];
		}
	}
}

/*
 * What a run starts from, and the one shape of a parallel GROUP BY, whose
 * ranges are handed out as it runs.
 */
static void
lion_begin_run_state(LionCountScanState *st, CustomScan *cscan)
{
	st->wherecoll.pinbuf = InvalidBuffer;
	st->gbatchcxt = NULL;		/* made by the first batch */

	/*
	 * A parallel-aware node that is no join is a parallel GROUP BY (DESIGN.md
	 * §10, "A GROUP BY in parallel"), which the planner offers for one shape
	 * alone: one column's entries walked whole, over one table
	 * (lion_count_priv_check()).
	 */
	st->granged = cscan->scan.plan.parallel_aware && st->joinclause < 0;
	st->grange = -1;
	st->grangecxt = NULL;		/* made by the first range */
}

/*
 * The scan's memory contexts, all under the query's: the ones every scan
 * has, and a fact column's, with the source list its counts use.
 */
static void
lion_begin_contexts(LionCountScanState *st, EState *estate)
{
	st->pergroup = AllocSetContextCreate(estate->es_query_cxt,
										 "LionCount per-group",
										 ALLOCSET_SMALL_SIZES);
	st->outercxt = AllocSetContextCreate(estate->es_query_cxt,
										 "LionCount outer group",
										 ALLOCSET_SMALL_SIZES);
	st->innercxt = AllocSetContextCreate(estate->es_query_cxt,
										 "LionCount inner keys",
										 ALLOCSET_SMALL_SIZES);
	st->wherecxt = AllocSetContextCreate(estate->es_query_cxt,
										 "LionCount where keys",
										 ALLOCSET_SMALL_SIZES);
	st->keycxt = AllocSetContextCreate(estate->es_query_cxt,
									   "LionCount clause keys",
									   ALLOCSET_SMALL_SIZES);
	st->valcxt = AllocSetContextCreate(estate->es_query_cxt,
									   "LionCount clause values",
									   ALLOCSET_SMALL_SIZES);

	/*
	 * A fact column's groups (DESIGN.md §27, "Grouped by a fact column"): a
	 * chunk of them located at a time, and the rows counted in them until
	 * they go up.  A count reads the key's set, the fact filters - or their
	 * copy - and one group's set: at most every source of the plan and one.
	 */
	if (st->fgattno != 0)
	{
		st->fgcxt = AllocSetContextCreate(estate->es_query_cxt,
										  "LionCount fact groups",
										  ALLOCSET_DEFAULT_SIZES);
		st->fgrowcxt = AllocSetContextCreate(estate->es_query_cxt,
											 "LionCount fact group rows",
											 ALLOCSET_DEFAULT_SIZES);
		st->fgnsrc = Max(st->nplanitem + 1, 2) + 1;
		st->fgsrc = (LionCountSource *)
			palloc0(sizeof(LionCountSource) * st->fgnsrc);
	}
}

/*
 * The FK-side join's dimension side (DESIGN.md §27) is an ordinary plan,
 * initialised here - EXPLAIN without ANALYZE prints it too - and run
 * under the same snapshot as the counts.
 */
static void
lion_begin_join_child(LionCountScanState *st, CustomScanState *node,
					  CustomScan *cscan, EState *estate, int eflags)
{
	int			i;

	st->joinfilterrows = -1;
	st->joinworkerfilterrows = -1;
	st->joinfilter.pinbuf = InvalidBuffer;
	st->joinchunk = -1;
	lion_join_switch_reset(st);

	/*
	 * Timed only when core times the nodes: under EXPLAIN ANALYZE with its
	 * TIMING option (or auto_explain's), which is what es_instrument says in
	 * every participant, as it says so to InstrAlloc() for each node.
	 */
	st->jointiming = (estate->es_instrument & INSTRUMENT_TIMER) != 0;
	for (i = 0; i < LION_JT_N; i++)
	{
		INSTR_TIME_SET_ZERO(st->jointime[i]);
		INSTR_TIME_SET_ZERO(st->joinworkertime[i]);
	}
	if (st->joinclause >= 0)
	{
		st->child = ExecInitNode((Plan *) linitial(cscan->custom_plans),
								 estate, eflags);
		node->custom_ps = list_make1(st->child);
	}
}

/*
 * A forward semi join over a non-unique key (DESIGN.md §27) sorts the
 * child's rows by their key and compares neighbours with the equality of
 * the sort operator's btree family, which is the join operator's family
 * (lion_fkjoin_recognize()) for the key's own type.
 */
static void
lion_begin_join_distinct_key(LionCountScanState *st, EState *estate)
{
	if (st->joinunique)
	{
		TupleDesc	childdesc = ExecGetResultType(st->child);
		Form_pg_attribute keyatt;
		Oid			eqop;

		/*
		 * Only the key comes out of the sort, and it is the only column of
		 * the child the target list reads (lion_count_priv_check()).
		 */
		if (st->joinkeyresno > childdesc->natts)
			elog(ERROR, "LionCount: malformed join");

		keyatt = TupleDescAttr(childdesc, st->joinkeyresno - 1);
		st->joinkeytype = keyatt->atttypid;
		st->joinkeybyval = keyatt->attbyval;
		st->joinkeylen = keyatt->attlen;
		eqop = get_equality_op_for_ordering_op(st->joinsortop, NULL);
		if (!OidIsValid(eqop))
			elog(ERROR, "could not find equality operator for ordering operator %u",
				 st->joinsortop);
		fmgr_info_cxt(get_opcode(eqop), &st->joineqfn, estate->es_query_cxt);
		st->joinsortslot = ExecInitExtraTupleSlot(estate, childdesc,
												  &TTSOpsVirtual);
		st->joinkeycxt = AllocSetContextCreate(estate->es_query_cxt,
											   "LionCount join key",
											   ALLOCSET_SMALL_SIZES);
	}
}

/*
 * Lookups in key order (DESIGN.md §27): the batches of rows - or of the
 * distinct keys, which the target list reads through joinsortslot above -
 * live in a context of their own, emptied at every batch, and a batched
 * row goes back to the target list through a slot of the child's shape.
 */
static void
lion_begin_join_batches(LionCountScanState *st, EState *estate)
{
	int			i;

	/*
	 * A partitioned fact table's keys are looked up a batch at a time
	 * whatever the plan says (DESIGN.md §27, "A partitioned fact table"):
	 * each batch goes to every partition in turn, with a turn of its own and
	 * a copy of each partition's fact filters kept from batch to batch.
	 */
	if (st->joinclause >= 0 && st->npart > 0)
	{
		st->joinpart = (LionJoinPart *)
			palloc0(sizeof(LionJoinPart) * st->npart);
		st->joinpartcxt = AllocSetContextCreate(estate->es_query_cxt,
												"LionCount partition copies",
												ALLOCSET_DEFAULT_SIZES);
		st->joinvisitcxt = AllocSetContextCreate(estate->es_query_cxt,
												 "LionCount partition turn",
												 ALLOCSET_SMALL_SIZES);
		for (i = 0; i < st->npart; i++)
		{
			LionJoinPart *jp = &st->joinpart[i];

			jp->filter.pinbuf = InvalidBuffer;
			jp->buy = -1;
			jp->qmode = (LionQueryMode *)
				palloc0(sizeof(LionQueryMode) * Max(st->nclause, 1));
			jp->cxt = AllocSetContextCreate(st->joinpartcxt,
											"LionCount partition copy",
											ALLOCSET_DEFAULT_SIZES);
		}
	}

	if (st->joinwalk || st->joinpart != NULL || st->fgattno != 0)
	{
		TupleDesc	childdesc = ExecGetResultType(st->child);
		Form_pg_attribute keyatt;

		if (st->joinkeyresno > childdesc->natts)
			elog(ERROR, "LionCount: malformed join");
		keyatt = TupleDescAttr(childdesc, st->joinkeyresno - 1);
		st->joinkeytype = keyatt->atttypid;
		st->joinkeybyval = keyatt->attbyval;
		st->joinkeylen = keyatt->attlen;
		/*
		 * A batch is full when the context's blocks reach work_mem, and a
		 * block may overshoot it by as much as a block: an eighth of it at
		 * most, rather than the allocator's usual 8 MB.
		 */
		st->joinbatchcxt =
			AllocSetContextCreate(estate->es_query_cxt,
								  "LionCount join batch",
								  ALLOCSET_DEFAULT_MINSIZE,
								  ALLOCSET_DEFAULT_INITSIZE,
								  Min((Size) ALLOCSET_DEFAULT_MAXSIZE,
									  Max((Size) ALLOCSET_DEFAULT_INITSIZE,
										  pg_prevpower2_size_t((Size) work_mem * 1024 / 8))));
		st->joinbatchslot = ExecInitExtraTupleSlot(estate, childdesc,
												   &TTSOpsMinimalTuple);
	}
}

/*
 * A plain table is opened once and stays open.  The executor already
 * holds locks on every range table entry, so the heap is opened without
 * taking another one.  The indexes are not range table entries, so they
 * get their own AccessShareLock.
 *
 * A partitioned one has every leaf partition opened here and kept open the
 * same way, and lion_open_relation() makes one of them at a time the relation
 * counted (DESIGN.md §16).
 */
static void
lion_begin_open_table(LionCountScanState *st, int eflags)
{
	if (st->npart > 0)
		lion_open_parts(st);
	else
	{
		lion_open_relation(st, -1);

		/*
		 * A materialized view created WITH NO DATA has an empty heap and
		 * empty indexes, and counting them would answer 0 where core's scan
		 * refuses to run at all.  So refuse exactly as ExecOpenScanRelation()
		 * does, and under the same exemption for CREATE TABLE AS ... WITH NO
		 * DATA (EXPLAIN without ANALYZE has returned above).  Nothing in the
		 * planner looks at relispopulated, and a REFRESH invalidates the
		 * plan.  A partition is never a materialized view.
		 */
		if ((eflags & EXEC_FLAG_WITH_NO_DATA) == 0 &&
			!RelationIsScannable(st->heap))
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("materialized view \"%s\" has not been populated",
							RelationGetRelationName(st->heap)),
					 errhint("Use the REFRESH MATERIALIZED VIEW command.")));
	}
}

/*
 * The source lists a count is made from: the plan's, a driver's alternative
 * and the one a count against the collected WHERE uses.
 */
static void
lion_begin_sources(LionCountScanState *st)
{
	/*
	 * Slot 0 is the (outer) group's posting set, 1 .. nitem the WHERE items,
	 * and - for a two-column GROUP BY (DESIGN.md §20), or the (g, k) pairs of
	 * a count(DISTINCT k) per group (§26) - slot nitem + 1 the inner one's.
	 */
	st->nsource = st->nitem + 1 + (st->innerattno != 0 ? 1 : 0);
	st->sources = (LionCountSource *)
		palloc0(sizeof(LionCountSource) * st->nsource);
	st->sources[0].nsets = 1;
	st->sources[0].sets = &st->groupset;
	st->sources[0].negated = false;
	if (st->innerattno != 0)
	{
		st->sources[st->nitem + 1].nsets = 1;
		st->sources[st->nitem + 1].sets = &st->groupset2;
		st->sources[st->nitem + 1].negated = false;
	}

	/*
	 * The alternative source list of a driver that makes one WHERE item
	 * redundant (DESIGN.md §14 and §15; see the comment on ingroupitem).  It is
	 * never longer than st->sources, and lion_locate_where() fills it in per
	 * relation.
	 */
	st->ingroupitem = -1;
	st->sumallitem = -1;
	st->dsources = (LionCountSource *)
		palloc0(sizeof(LionCountSource) * st->nsource);
	st->batchitem = -1;

	/*
	 * A count against the collected WHERE (lion_group_count()) takes one slot
	 * for the collected set on top of what it keeps of st->sources.
	 */
	st->wsources = (LionCountSource *)
		palloc0(sizeof(LionCountSource) * (st->nsource + 1));
}

/*
 * The decoded walk of DESIGN.md §34, when the plan's GROUP BY columns are
 * counted by it (LION_FLAG_DECODE): the columns from LION_PRIV_GROUPN, and
 * the run's memory.  Its indexes are opened with the relation's
 * (lion_open_relation()).
 */
static void
lion_begin_decoded_walk(LionCountScanState *st, const LionCountPriv *priv,
						EState *estate)
{
	LionDecodeRun *dr;
	int			c;

	st->decode = NULL;
	if ((priv->flags & LION_FLAG_DECODE) == 0)
		return;

	dr = (LionDecodeRun *) MemoryContextAllocZero(estate->es_query_cxt,
												  sizeof(LionDecodeRun));
	dr->ncol = priv->ngroupn;
	for (c = 0; c < dr->ncol; c++)
	{
		dr->attno[c] = priv->groupn_attno[c];
		dr->idxoid[c] = priv->groupn_idx[c];
		dr->chunkcxt[c] = AllocSetContextCreate(estate->es_query_cxt,
												"LionCount decoded chunk",
												ALLOCSET_DEFAULT_SIZES);
	}
	dr->passcxt = AllocSetContextCreate(estate->es_query_cxt,
										"LionCount decoded pass",
										ALLOCSET_DEFAULT_SIZES);
	dr->tally = lion_decode_tally_create(estate->es_query_cxt,
										 get_hash_memory_limit());
	st->decode = dr;
}

#ifdef USE_ASSERT_CHECKING
/*
 * The mode as the run's tests of the fields begin set would find it, which
 * lion_count_mode_of() has to agree with.
 */
static LionCountMode
lion_begin_legacy_mode(LionCountScanState *st)
{
	if (st->joinclause >= 0)
		return (st->fgattno != 0) ? LION_MODE_JOIN_FACTGROUP : LION_MODE_JOIN;
	if (!st->hasgroupidx)
		return LION_MODE_COUNT;
	if (st->distattno != 0 && st->groupattno == 0)
		return LION_MODE_DISTINCT;
	if (st->sumall)
		return LION_MODE_SUM;
	if (st->granged)
		return LION_MODE_GROUP_RANGED;
	if (st->decode != NULL)
		return LION_MODE_DECODE;
	if (st->distattno != 0)
		return LION_MODE_GROUP_DISTINCT;
	if (st->groupattno2 != 0)
		return LION_MODE_GROUP2;
	return LION_MODE_GROUP;
}
#endif

void
lion_begin_custom_scan(CustomScanState *node, EState *estate, int eflags)
{
	LionCountScanState *st = (LionCountScanState *) node;
	CustomScan *cscan = (CustomScan *) node->ss.ps.plan;
	List	   *exprs = cscan->custom_exprs;
	LionCountPriv priv;

	/*
	 * custom_private is positional: every member's shape is checked before a
	 * single value of it is used, and the rules between the members after
	 * (lion_plan_private.h).
	 */
	lion_count_priv_decode(cscan->custom_private, LION_PRIV_STAGE_PLAN, &priv);

	/* Before anything is opened or read (DESIGN.md §9, "Privileges"). */
	lion_check_replaced_execute(&priv, eflags);

	lion_count_priv_check(&priv, cscan->scan.plan.parallel_aware,
						  list_length(cscan->custom_plans));
	st->mode = lion_count_mode_of(&priv, cscan->scan.plan.parallel_aware);

	/*
	 * One value expression per clause in custom_exprs, then one argument per
	 * aggregate over keys (DESIGN.md §37).  A mismatch is planner/executor
	 * drift, exactly like a wrong shape marker, and is said rather than
	 * decoded.
	 */
	if (list_length(exprs) != priv.nclause + priv.nwagg)
		elog(ERROR, "LionCount: %d clauses but %d value expressions",
			 priv.nclause, list_length(exprs) - priv.nwagg);

	lion_begin_plan(st, &priv);
	lion_begin_topk(st, &priv, estate);
	lion_begin_coalesce(st, &priv, estate);
	lion_begin_join(st, &priv);
	lion_begin_target_list(st, cscan, &priv);
	lion_begin_wagg(st, node, &priv, exprs);
	lion_begin_clauses(st, node, &priv, exprs);
	lion_begin_driving_column(st, &priv);
	lion_begin_ors_and_items(st, &priv);
	lion_begin_partitions(st, &priv);
	lion_begin_fact_group(st, &priv);
	lion_begin_run_state(st, cscan);
	lion_begin_contexts(st, estate);
	lion_begin_decoded_walk(st, &priv, estate);
	st->viscache = lion_vis_cache_create(estate->es_query_cxt);
	st->writtenrels = lion_statement_written_rels(estate);
	lion_begin_join_child(st, node, cscan, estate, eflags);
	lion_begin_join_distinct_key(st, estate);
	lion_begin_join_batches(st, estate);
	Assert(st->mode == lion_begin_legacy_mode(st));

	if ((eflags & EXEC_FLAG_EXPLAIN_ONLY) != 0)
		return;

	lion_begin_open_table(st, eflags);
	lion_begin_sources(st);
}
