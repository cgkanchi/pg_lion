/*-------------------------------------------------------------------------
 *
 * lion_defer.c
 *		Statement-level insert batching (DESIGN.md §55).
 *
 * Every writer of a key holds the directory leaf that files the key's entry,
 * EXCLUSIVE, for the whole of its insert (DESIGN.md §5, §21).  A column of a
 * few values keeps all of its entries on one leaf, so concurrent statements
 * that insert into it take turns on that leaf row by row, and the lock
 * handoffs - not the inserts - become the cost.  So a statement's rows are
 * kept here, per index, as (key, TID) pairs, and written sorted by key, each
 * key's rows under one hold of its leaf (lion_insert_apply()).
 *
 * The rows are written before anything in this backend can need them:
 *
 *	- at the end of the statement: aminsertcleanup, which ExecCloseIndices()
 *	  calls for every index the executor opened for inserts (and
 *	  validate_index() for CREATE INDEX CONCURRENTLY);
 *	- when a query starts whose range table names the table: a trigger's or a
 *	  function's query, run in the middle of the statement, sees the rows the
 *	  statement wrote before it - and a lion path of that query reads only
 *	  indexes of the tables its range table holds;
 *	- when a scan of the index begins (lionbeginscan()), or a direct reader
 *	  opens it (lion_open_index(), lion_reader_open(), the count and facet
 *	  functions): readers that no range table names;
 *	- before a utility command, which may rewrite, truncate or drop the table
 *	  or the index;
 *	- when the buffer outgrows work_mem;
 *	- when a row comes from another subtransaction than the rows the buffer
 *	  holds, so that each buffer is one subtransaction's rows: its abort
 *	  drops them, as the rows themselves are dead from then on;
 *	- at PRE_COMMIT and PRE_PREPARE, for a caller that inserts through
 *	  index_insert() and never calls index_insert_cleanup().  An insert after
 *	  that goes straight in.
 *
 * Nothing is buffered (lion_defer_allowed(), and lioninsert() per row and
 * column):
 *
 *	- on PostgreSQL 16, which has no aminsertcleanup;
 *	- for the index of an exclusion constraint, whose check scans the index
 *	  for the rows inserted before;
 *	- while an INSERT ... ON CONFLICT runs.  A speculative row that loses its
 *	  race is super-deleted, which makes it dead to every snapshot at once:
 *	  VACUUM may remove it and give its TID to another row before a batch
 *	  holding it is written, and the batch would then file that other row
 *	  under this row's key;
 *	- for the first row of a statement, so that a one-row statement costs
 *	  what it did;
 *	- for a column with stored positions, summaries or bit slices, whose
 *	  inserts write more than the key's entry;
 *	- in the rest of the transaction that loads the library, when it is not
 *	  preloaded (lion_defer_init()).
 *
 * A write that fails leaves every row in the buffer: an insert of a TID the
 * posting set already holds changes nothing, so the next write - or the
 * pre-commit one, whose failure aborts the transaction - redoes them all.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/parallel.h"
#include "access/table.h"
#include "access/xact.h"
#include "executor/executor.h"
#include "storage/lmgr.h"
#include "fmgr.h"
#include "miscadmin.h"
#include "nodes/plannodes.h"
#include "tcop/utility.h"
#include "utils/guc.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "varatt.h"

#include "lion.h"
#include "lion_compat.h"
#include "lion_defer.h"

/* One index's rows, waiting. */
struct LionDefer
{
	Oid			indexoid;
	Oid			heapoid;
	SubTransactionId owner;		/* the subtransaction the rows are from */
	bool		writing;		/* lion_defer_write() under way */
	MemoryContext cxt;			/* rows[] */
	MemoryContext keycxt;		/* the by-reference keys rows[] points to */
	LionInsertRow *rows;
	int			nrows;
	int			maxrows;
	Size		bytes;			/* charged against work_mem */
	LionDefer  *next;
};

bool		lion_batch_inserts = true;

/* the transaction's buffers, in TopTransactionContext */
static LionDefer *lion_defers = NULL;
static LionDefer *lion_defer_last = NULL;	/* lion_defer_get()'s last answer */
static int64 lion_defer_pending = 0;	/* rows in all of them */
/*
 * Nothing more is buffered in this transaction: its pre-commit write is
 * done, or the library was loaded inside it (lion_defer_init()).
 */
static bool lion_defer_closed = false;

/* An INSERT ... ON CONFLICT under way, and the subtransaction it started in. */
typedef struct LionConflictRun
{
	QueryDesc  *qd;
	SubTransactionId owner;
} LionConflictRun;

static LionConflictRun *lion_conflict_runs = NULL;	/* TopMemoryContext */
static int	lion_conflict_nruns = 0;
static int	lion_conflict_maxruns = 0;

static ExecutorStart_hook_type lion_prev_ExecutorStart = NULL;
static ExecutorEnd_hook_type lion_prev_ExecutorEnd = NULL;
static ProcessUtility_hook_type lion_prev_ProcessUtility = NULL;

/* lion_debug_defer_cleanup(false): aminsertcleanup leaves the rows */
static bool lion_defer_skip_cleanup = false;

PG_FUNCTION_INFO_V1(lion_debug_defer_pending);
PG_FUNCTION_INFO_V1(lion_debug_defer_cleanup);

/* rows[] starts this long and doubles */
#define LION_DEFER_MIN_ROWS		64

/*
 * Above this many (buffer, range table entry) pairs, a query's start writes
 * every buffer instead of matching them to its tables one by one.
 */
#define LION_DEFER_MATCH_MAX	4096

/*
 * May lioninsert() buffer this row's keys, rather than write them?  The
 * per-column and first-row conditions are lioninsert()'s.
 */
bool
lion_defer_allowed(IndexInfo *indexInfo)
{
#if PG_VERSION_NUM >= 170000
	return lion_batch_inserts &&
		!lion_defer_closed &&
		lion_conflict_nruns == 0 &&
		indexInfo->ii_ExclusionOps == NULL &&
		!IsParallelWorker();
#else
	return false;
#endif
}

static LionDefer *
lion_defer_find(Oid indexoid)
{
	LionDefer  *d;

	if (lion_defer_last != NULL && lion_defer_last->indexoid == indexoid)
		return lion_defer_last;
	for (d = lion_defers; d != NULL; d = d->next)
	{
		if (d->indexoid == indexoid)
		{
			lion_defer_last = d;
			return d;
		}
	}
	return NULL;
}

/* Empty d, keeping it for the transaction's next rows. */
static void
lion_defer_clear(LionDefer *d)
{
	lion_defer_pending -= d->nrows;
	Assert(lion_defer_pending >= 0);
	d->rows = NULL;
	d->nrows = 0;
	d->maxrows = 0;
	d->bytes = 0;
	MemoryContextReset(d->cxt);
	MemoryContextReset(d->keycxt);
}

/*
 * Write d's rows into index, whose table is heap, and empty d.  On an error
 * the rows stay: see the head of the file.
 */
static void
lion_defer_write(LionDefer *d, Relation index, Relation heap)
{
	if (d->nrows == 0)
		return;

	/*
	 * Nothing a write calls starts another, and an error in one aborts the
	 * (sub)transaction it ran in, which clears the flag; one still set means
	 * that an error was swallowed without that, and the rows cannot be known
	 * to be in.
	 */
	if (d->writing)
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("lion index \"%s\": an earlier write of buffered rows did not finish",
						RelationGetRelationName(index))));

	d->writing = true;
	lion_insert_apply(index, heap, d->rows, d->nrows);
	d->writing = false;
	lion_defer_clear(d);
}

/* Write d's rows, opening its relations; drop them if the index is gone. */
static void
lion_defer_write_oid(LionDefer *d)
{
	Relation	index;
	Relation	heap;

	if (d->nrows == 0)
		return;

	/*
	 * The rows' transaction holds RowExclusiveLock on the table until it
	 * ends, and on the index while their statement runs; the index's lock
	 * taken here goes with the write (lion_defer_flush_index()).  An index
	 * dropped in this transaction - which a utility command's write, below,
	 * makes all but impossible - takes its rows with it.
	 */
	index = try_index_open(d->indexoid, RowExclusiveLock);
	if (index == NULL)
	{
		lion_defer_clear(d);
		return;
	}
	heap = table_open(d->heapoid, RowExclusiveLock);
	lion_defer_write(d, index, heap);
	table_close(heap, NoLock);
	index_close(index, RowExclusiveLock);
}

/*
 * The buffer for index, whose table is heap, made if it is the first of the
 * transaction.  Rows from an earlier subtransaction than the current one are
 * written first.
 */
LionDefer *
lion_defer_get(Relation index, Relation heap)
{
	SubTransactionId cur = GetCurrentSubTransactionId();
	LionDefer  *d = lion_defer_find(RelationGetRelid(index));

	if (d == NULL)
	{
		d = (LionDefer *) MemoryContextAllocZero(TopTransactionContext,
												 sizeof(LionDefer));
		d->indexoid = RelationGetRelid(index);
		d->heapoid = RelationGetRelid(heap);
		d->owner = cur;
		d->cxt = AllocSetContextCreate(TopTransactionContext,
									   "lion insert batch",
									   ALLOCSET_DEFAULT_SIZES);
#if PG_VERSION_NUM >= 170000
		d->keycxt = BumpContextCreate(TopTransactionContext,
									  "lion insert batch keys",
									  ALLOCSET_DEFAULT_SIZES);
#else
		d->keycxt = AllocSetContextCreate(TopTransactionContext,
										  "lion insert batch keys",
										  ALLOCSET_DEFAULT_SIZES);
#endif
		d->next = lion_defers;
		lion_defers = d;
		lion_defer_last = d;
	}

	if (d->owner != cur)
	{
		lion_defer_write(d, index, heap);
		d->owner = cur;
	}
	return d;
}

/*
 * Buffer one (key, TID) pair of column col (0-based), whose state is state:
 * key as lion_key_prepare() made it, for flag 0, or a reserved entry's flag.
 */
void
lion_defer_add(LionDefer *d, Relation index, Relation heap, LionState *state,
			   int col, Datum key, uint16 flag, uint64 code)
{
	LionInsertRow *row;
	uint32		hash = LION_NULLKEY_HASH;
	Size		bytes = sizeof(LionInsertRow);

	Assert(!d->writing);

	if (d->nrows >= d->maxrows)
	{
		int			newmax = (d->maxrows == 0) ? LION_DEFER_MIN_ROWS :
			d->maxrows * 2;

		if ((Size) newmax * sizeof(LionInsertRow) > MaxAllocSize)
		{
			lion_defer_write(d, index, heap);
			newmax = LION_DEFER_MIN_ROWS;
		}
		if (d->rows == NULL)
			d->rows = (LionInsertRow *)
				MemoryContextAlloc(d->cxt, newmax * sizeof(LionInsertRow));
		else
			d->rows = (LionInsertRow *)
				repalloc(d->rows, newmax * sizeof(LionInsertRow));
		d->maxrows = newmax;
	}

	if (flag == 0)
	{
		hash = lion_hash_key(state, key);
		if (!state->typbyval)
		{
			Size		len = (state->typlen == -1) ?
				VARSIZE_ANY(DatumGetPointer(key)) :
				(state->typlen == -2) ?
				strlen(DatumGetCString(key)) + 1 : (Size) state->typlen;
			char	   *copy = MemoryContextAlloc(d->keycxt, len);

			memcpy(copy, DatumGetPointer(key), len);
			key = PointerGetDatum(copy);
			bytes += MAXALIGN(len);
		}
	}
	else
		key = (Datum) 0;

	row = &d->rows[d->nrows];
	row->key = key;
	row->code = code;
	row->hash = hash;
	row->flag = flag;
	row->col = (uint16) col;
	d->nrows++;
	d->bytes += bytes;
	lion_defer_pending++;

	if (d->bytes >= (Size) work_mem * 1024)
		lion_defer_write(d, index, heap);
}

/* Write the rows buffered for index. */
void
lion_defer_flush_index(Relation index)
{
	LionDefer  *d;
	Relation	heap;

	if (lion_defer_pending == 0)
		return;
	d = lion_defer_find(RelationGetRelid(index));
	if (d == NULL || d->nrows == 0)
		return;

	/*
	 * A writer of the index holds RowExclusiveLock on it, which is what
	 * lion_index_verify() waits for (lion_verify_wait_for_writers()) - and a
	 * reader's lock may be all this backend holds of it now, when the rows'
	 * statement has ended.  The lock goes with the write, as the executor's
	 * goes at the end of the statement (ExecCloseIndices()): held to the end
	 * of the transaction, it would have verify() wait for a transaction that
	 * writes nothing more, which is what lets it settle a statement's splits
	 * once that statement is over.
	 */
	LockRelation(index, RowExclusiveLock);
	heap = table_open(d->heapoid, RowExclusiveLock);
	lion_defer_write(d, index, heap);
	table_close(heap, NoLock);
	UnlockRelation(index, RowExclusiveLock);
}

/* Write the rows buffered for every index of the table heapoid. */
void
lion_defer_flush_heap(Oid heapoid)
{
	LionDefer  *d;

	if (lion_defer_pending == 0)
		return;
	for (d = lion_defers; d != NULL; d = d->next)
	{
		if (d->heapoid == heapoid)
			lion_defer_write_oid(d);
	}
}

/* Write every buffered row. */
void
lion_defer_flush_all(void)
{
	LionDefer  *d;

	if (lion_defer_pending == 0)
		return;
	for (d = lion_defers; d != NULL; d = d->next)
		lion_defer_write_oid(d);
}

/*
 * aminsertcleanup: the statement - or validate_index() - is done inserting
 * into index.
 */
void
lioninsertcleanup(Relation index, IndexInfo *indexInfo)
{
	if (!lion_defer_skip_cleanup)
		lion_defer_flush_index(index);
}

/*
 * lion_debug_defer_pending(idx regclass) returns int8: the (key, TID) pairs
 * this backend has buffered for the index, written or not.  A query that
 * names no table - SELECT lion_debug_defer_pending('i') - writes none.
 */
Datum
lion_debug_defer_pending(PG_FUNCTION_ARGS)
{
	LionDefer  *d;

	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("lion_debug_defer_pending() is for superusers")));
	d = lion_defer_find(PG_GETARG_OID(0));
	PG_RETURN_INT64((d == NULL) ? 0 : d->nrows);
}

/*
 * lion_debug_defer_cleanup(on bool) returns bool: with false, the end of a
 * statement leaves its rows buffered, as a caller of index_insert() that
 * never calls index_insert_cleanup() would, for the other ways in to be
 * tested; the setting it replaces is returned.
 */
Datum
lion_debug_defer_cleanup(PG_FUNCTION_ARGS)
{
	bool		was = !lion_defer_skip_cleanup;

	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("lion_debug_defer_cleanup() is for superusers")));
	lion_defer_skip_cleanup = !PG_GETARG_BOOL(0);
	PG_RETURN_BOOL(was);
}

/* Write the rows of every table of rtable that has any. */
static void
lion_defer_flush_rtable(List *rtable)
{
	ListCell   *lc;
	LionDefer  *d;
	int			nbuf = 0;

	for (d = lion_defers; d != NULL; d = d->next)
	{
		if (d->nrows > 0)
			nbuf++;
	}
	if ((int64) nbuf * list_length(rtable) > LION_DEFER_MATCH_MAX)
	{
		lion_defer_flush_all();
		return;
	}

	foreach(lc, rtable)
	{
		RangeTblEntry *rte = lfirst_node(RangeTblEntry, lc);

		if (rte->rtekind != RTE_RELATION)
			continue;
		lion_defer_flush_heap(rte->relid);
		if (lion_defer_pending == 0)
			return;
	}
}

/*
 * Is plan an INSERT ... ON CONFLICT?  Its ModifyTable is the top of the plan
 * of the statement or of a data-modifying WITH query, which is a subplan.
 */
static bool
lion_plan_is_conflict(Plan *plan)
{
	return plan != NULL && IsA(plan, ModifyTable) &&
		((ModifyTable *) plan)->onConflictAction != ONCONFLICT_NONE;
}

static bool
lion_stmt_has_conflict(PlannedStmt *stmt)
{
	ListCell   *lc;

	if (stmt->commandType != CMD_INSERT && !stmt->hasModifyingCTE)
		return false;
	if (lion_plan_is_conflict(stmt->planTree))
		return true;
	foreach(lc, stmt->subplans)
	{
		if (lion_plan_is_conflict((Plan *) lfirst(lc)))
			return true;
	}
	return false;
}

static void
lion_defer_ExecutorStart(QueryDesc *queryDesc, int eflags)
{
	if (lion_defer_pending > 0)
		lion_defer_flush_rtable(queryDesc->plannedstmt->rtable);

	if (lion_prev_ExecutorStart != NULL)
		lion_prev_ExecutorStart(queryDesc, eflags);
	else
		standard_ExecutorStart(queryDesc, eflags);

	if (lion_stmt_has_conflict(queryDesc->plannedstmt))
	{
		if (lion_conflict_nruns >= lion_conflict_maxruns)
		{
			int			newmax = Max(8, lion_conflict_maxruns * 2);

			if (lion_conflict_runs == NULL)
				lion_conflict_runs = (LionConflictRun *)
					MemoryContextAlloc(TopMemoryContext,
									   newmax * sizeof(LionConflictRun));
			else
				lion_conflict_runs = (LionConflictRun *)
					repalloc(lion_conflict_runs,
							 newmax * sizeof(LionConflictRun));
			lion_conflict_maxruns = newmax;
		}
		lion_conflict_runs[lion_conflict_nruns].qd = queryDesc;
		lion_conflict_runs[lion_conflict_nruns].owner =
			GetCurrentSubTransactionId();
		lion_conflict_nruns++;
	}
}

static void
lion_defer_ExecutorEnd(QueryDesc *queryDesc)
{
	int			i;

	for (i = lion_conflict_nruns - 1; i >= 0; i--)
	{
		if (lion_conflict_runs[i].qd == queryDesc)
		{
			lion_conflict_runs[i] = lion_conflict_runs[--lion_conflict_nruns];
			break;
		}
	}

	if (lion_prev_ExecutorEnd != NULL)
		lion_prev_ExecutorEnd(queryDesc);
	else
		standard_ExecutorEnd(queryDesc);
}

static void
lion_defer_ProcessUtility(PlannedStmt *pstmt, const char *queryString,
						  bool readOnlyTree, ProcessUtilityContext context,
						  ParamListInfo params, QueryEnvironment *queryEnv,
						  DestReceiver *dest, QueryCompletion *qc)
{
	/*
	 * Not before a transaction command: COMMIT and PREPARE write at
	 * PRE_COMMIT, a SAVEPOINT's rows start a buffer of their own, and a
	 * ROLLBACK must not fail on rows it is about to drop.
	 */
	if (lion_defer_pending > 0 && !IsA(pstmt->utilityStmt, TransactionStmt))
		lion_defer_flush_all();

	if (lion_prev_ProcessUtility != NULL)
		lion_prev_ProcessUtility(pstmt, queryString, readOnlyTree, context,
								 params, queryEnv, dest, qc);
	else
		standard_ProcessUtility(pstmt, queryString, readOnlyTree, context,
								params, queryEnv, dest, qc);
}

static void
lion_defer_xact_callback(XactEvent event, void *arg)
{
	switch (event)
	{
		case XACT_EVENT_PRE_COMMIT:
		case XACT_EVENT_PRE_PREPARE:
			lion_defer_flush_all();
			lion_defer_closed = true;
			break;
		case XACT_EVENT_PARALLEL_PRE_COMMIT:
			break;
		default:
			/* the buffers go with TopTransactionContext */
			Assert(lion_defer_pending == 0 || event == XACT_EVENT_ABORT ||
				   event == XACT_EVENT_PARALLEL_ABORT);
			lion_defers = NULL;
			lion_defer_last = NULL;
			lion_defer_pending = 0;
			lion_defer_closed = false;
			lion_conflict_nruns = 0;
			break;
	}
}

static void
lion_defer_subxact_callback(SubXactEvent event, SubTransactionId mySubid,
							SubTransactionId parentSubid, void *arg)
{
	LionDefer  *d;
	int			i;

	switch (event)
	{
		case SUBXACT_EVENT_ABORT_SUB:
			for (d = lion_defers; d != NULL; d = d->next)
			{
				/* any write under way was inside the subtransaction */
				d->writing = false;
				if (d->owner == mySubid)
					lion_defer_clear(d);
			}
			for (i = lion_conflict_nruns - 1; i >= 0; i--)
			{
				if (lion_conflict_runs[i].owner == mySubid)
					lion_conflict_runs[i] =
						lion_conflict_runs[--lion_conflict_nruns];
			}
			break;
		case SUBXACT_EVENT_COMMIT_SUB:
			for (d = lion_defers; d != NULL; d = d->next)
			{
				if (d->owner == mySubid)
					d->owner = parentSubid;
			}
			for (i = 0; i < lion_conflict_nruns; i++)
			{
				if (lion_conflict_runs[i].owner == mySubid)
					lion_conflict_runs[i].owner = parentSubid;
			}
			break;
		default:
			break;
	}
}

void
lion_defer_init(void)
{
	DefineCustomBoolVariable("pg_lion.batch_inserts",
							 "Writes the rows a statement inserts into a lion index in sorted batches.",
							 "Each key's rows go in under one hold of its directory leaf, "
							 "so concurrent statements inserting into a few keys stop "
							 "taking turns on it row by row.  PostgreSQL 17 and later.",
							 &lion_batch_inserts,
							 true,
							 PGC_USERSET,
							 0,
							 NULL, NULL, NULL);

	RegisterXactCallback(lion_defer_xact_callback, NULL);
	RegisterSubXactCallback(lion_defer_subxact_callback, NULL);

	/*
	 * Loaded inside a transaction - not preloaded, so loaded by the first
	 * use of a lion index in this backend - the hooks below may be late for
	 * a statement already under way: an INSERT ... ON CONFLICT DO NOTHING
	 * with no conflict target opens its indexes, and so loads the library,
	 * only in its ExecutorStart, which then never registers as one.  So
	 * this transaction buffers nothing; its end (the callback above) opens
	 * the next.
	 */
	if (IsTransactionState())
		lion_defer_closed = true;

	lion_prev_ExecutorStart = ExecutorStart_hook;
	ExecutorStart_hook = lion_defer_ExecutorStart;
	lion_prev_ExecutorEnd = ExecutorEnd_hook;
	ExecutorEnd_hook = lion_defer_ExecutorEnd;
	lion_prev_ProcessUtility = ProcessUtility_hook;
	ProcessUtility_hook = lion_defer_ProcessUtility;
}
