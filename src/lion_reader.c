/*-------------------------------------------------------------------------
 *
 * lion_reader.c
 *		What every direct reader of a lion index owes the caller (DESIGN.md
 *		§9, "Direct readers").
 *
 * An index scan that the executor starts through index_beginscan() gets a
 * number of guarantees from PostgreSQL for free: the planner only offers an
 * index the snapshot may use, and row-level security is applied above the
 * scan.  lion reads its indexes without a scan in two kinds of places, and
 * each has to provide what is missing itself:
 *
 *	- the SQL-callable functions that are handed an index by name (the count
 *	  functions, lion_bm25(), lion_bm25_score()), which nothing vetted:
 *	  lion_reader_open() for a whole index-and-table open (which also
 *	  refuses a partial index: its readers rank over the whole table), or
 *	  lion_reader_vet() for a caller that opens them itself;
 *	- the custom scans' executor nodes, whose index the planner vetted.
 *
 * SERIALIZABLE predicate locks are the same for every reader, the AM's own
 * scans included (ampredlocks is true): the shared lookups and walks take
 * them on the pages they read, and the insert path checks them, through the
 * functions at the end of this file (DESIGN.md §9, "SERIALIZABLE").
 *
 * What a reader is vetted for depends on what it returns:
 *
 *	LION_READ_ROWS	rows, or counts of rows, as a query would: the index must
 *					be usable under the snapshot, a table with row-level
 *					security applying to the caller is refused (the policies
 *					cannot be evaluated without reading the rows), and an
 *					unpopulated materialized view is refused as the executor
 *					refuses it.
 *	LION_READ_STATS	only index-wide statistics, as lion_bm25_score() reads
 *					N, df and avgdl: the privilege checks, and the same
 *					refusal of row-level security.  The statistics count
 *					every row, so a score - even of a made-up document, even
 *					per row of a query that applies the policies - tells
 *					whether hidden rows hold a lexeme, and how many.
 *
 * Privileges beyond SELECT on the table - per-column grants, EXECUTE on the
 * functions a call stands for - depend on what the call stands for, and stay
 * with the caller (lion_count_open_indexes()).
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "access/table.h"
#include "access/xact.h"
#include "catalog/index.h"
#include "catalog/pg_class.h"
#include "catalog/pg_index.h"
#include "miscadmin.h"
#include "storage/predicate.h"
#include "utils/acl.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/rls.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"

#include "lion.h"
#include "lion_count.h"

/*
 * Row-level security: the policies would have to be evaluated per row, and a
 * direct reader does not look at rows.  The same count or ranking through
 * the planner still works where it reads rows: lion's custom paths decline
 * relations with security quals, and the ordinary plan applies them.
 */
static void
lion_reader_check_rls(Relation heap, Relation index, const char *action)
{
	if (check_enable_rls(RelationGetRelid(heap), InvalidOid, false) ==
		RLS_ENABLED)
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("cannot %s through index \"%s\" because row-level security is enabled on table \"%s\"",
						action, RelationGetRelationName(index),
						RelationGetRelationName(heap))));
}

/*
 * Refuse what an ordinary scan of index under snapshot would not return
 * (LION_READ_ROWS above).  action reads after "cannot ... through index":
 * "count", "rank".
 */
void
lion_reader_vet(Relation heap, Relation index, Snapshot snapshot,
				const char *action)
{
	const char *why;

	if (!RelationIsScannable(heap))
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("materialized view \"%s\" has not been populated",
						RelationGetRelationName(heap)),
				 errhint("Use the REFRESH MATERIALIZED VIEW command.")));

	lion_reader_check_rls(heap, index, action);

	/*
	 * Snapshot eligibility, which the planner decides for a query: an index
	 * that indcheckxmin makes unusable does not hold the HOT-chain versions
	 * an old snapshot still sees, and no heap recheck can put back a TID
	 * that is not in it.
	 */
	if (!lion_index_usable(index, snapshot, &why))
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("cannot %s through index \"%s\" because %s",
						action, RelationGetRelationName(index), why)));

	/* and none at all under PostgreSQL 16's old_snapshot_threshold */
	lion_check_old_snapshot(index, snapshot);
}

/*
 * Open the lion index indexoid and, with heap given, its table, for a direct
 * read under snapshot of the kind policy says (above).  SELECT on the table
 * is checked before any lock is taken, so a role without it cannot queue for
 * one; the relations are open with AccessShareLock on return, and vetted.
 */
Relation
lion_reader_open(Oid indexoid, LionReadPolicy policy, Snapshot snapshot,
				 const char *action, Relation *heap)
{
	Oid			heapoid;
	Relation	table;
	Relation	index;

	if (get_rel_relkind(indexoid) == '\0')
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_TABLE),
				 errmsg("relation with OID %u does not exist", indexoid)));
	lion_refuse_partitioned_index(indexoid);
	if (get_rel_relkind(indexoid) != RELKIND_INDEX)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("\"%s\" is not an index", get_rel_name(indexoid))));
	heapoid = IndexGetRelation(indexoid, false);
	if (pg_class_aclcheck(heapoid, GetUserId(), ACL_SELECT) != ACLCHECK_OK)
		aclcheck_error(ACLCHECK_NO_PRIV,
					   get_relkind_objtype(get_rel_relkind(heapoid)),
					   get_rel_name(heapoid));
	table = table_open(heapoid, AccessShareLock);
	index = index_open(indexoid, AccessShareLock);
	if (index->rd_rel->relam != lion_get_am_oid())
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("index \"%s\" is not a lion index",
						RelationGetRelationName(index))));
	/*
	 * Both of its readers rank or score over the whole table: a partial
	 * index's walk would miss the rows its predicate leaves out, and its N,
	 * df and avgdl would describe only the rows it holds.  The planner's
	 * LionBm25 path declines a partial index for the same reason.
	 */
	if (!heap_attisnull(index->rd_indextuple, Anum_pg_index_indpred, NULL))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot %s with partial index \"%s\"", action,
						RelationGetRelationName(index)),
				 errdetail("Ranking reads the statistics and candidates of every row of the table.")));
	if (policy == LION_READ_ROWS)
	{
		lion_check_table_am(table);
		lion_reader_vet(table, index, snapshot, action);
	}
	else
	{
		const char *why;

		lion_reader_check_rls(table, index, action);

		/*
		 * Statistics of an index that is not valid, or not ready, describe
		 * a corpus no query reads: a score from them is not the score
		 * lion_bm25() would give the row.
		 */
		if (!lion_index_usable(index, snapshot, &why))
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("cannot %s through index \"%s\" because %s",
							action, RelationGetRelationName(index), why)));
	}

	/* not given: still locked until the transaction ends, as the index is */
	if (heap != NULL)
		*heap = table;
	else
		table_close(table, NoLock);
	return index;
}


/* ---------------------------------------------------------------------
 * SERIALIZABLE (DESIGN.md §9, "SERIALIZABLE")
 *
 * What a read depends on in the index is where it found each key it looked
 * for, or where the key would have been, and the reader predicate-locks the
 * page that stands for that:
 *
 *	- an INLINE entry, or no entry at all: the directory LEAF that holds it,
 *	  or would hold it, as GIN locks an entry-tree leaf for a posting list or
 *	  for a key it did not find;
 *	- a CHAIN entry: its posting tree's ROOT, which stays at its block for the
 *	  life of the set (a root split pushes down, DESIGN.md §22), so that a
 *	  lock on it stands for the key alone, as GIN locks the root of a posting
 *	  tree;
 *	- a walk (a range, a GROUP BY, IS NOT NULL, an ordered walk): every leaf
 *	  it reads, which covers every key an insert could put between the ones it
 *	  saw, and the root of every CHAIN entry of its column on them.
 *
 * Each lock is taken while the leaf the entry or the gap was read from is
 * share-locked, and an insert checks the one page that stands for its key -
 * the leaf for a new or an INLINE entry, the root for a CHAIN one - while it
 * holds that leaf exclusive and before it changes anything
 * (lion_writer_check()).  So either the insert comes first and the reader
 * sees its TID, whose heap tuple then tells SSI of the conflict (the heap
 * visit checks it, and a block counted from the visibility map is never one
 * an uncommitted insert has touched), or the reader's lock is there when the
 * insert looks.  The locks follow the keys: a leaf split copies the leaf's
 * locks to its new right sibling (lion_dir_split()), the spill of an INLINE
 * set copies its leaf's to the new root (lion_entry_spill_pos()), and VACUUM
 * moves a deleted CHAIN entry's root locks to its leaf
 * (lion_vacuum_delete_entries()) and a deleted directory leaf's to its right
 * sibling, which takes its range over (lion_dir_delete.c).
 *
 * Outside a SERIALIZABLE transaction all of this does nothing, and the
 * predicate-lock manager itself returns at once for a transaction that needs
 * no locks (a read-only one found safe) or a relation that takes none (a
 * temporary one).
 * ---------------------------------------------------------------------
 */

/*
 * The snapshot a read's predicate locks are taken under, or NULL outside a
 * SERIALIZABLE transaction.  The lock manager asks only whether it is an MVCC
 * snapshot, and every snapshot a SERIALIZABLE transaction reads rows with is
 * its transaction snapshot - the active one while a query runs.
 */
static Snapshot
lion_reader_predsnap(void)
{
	if (!IsolationIsSerializable())
		return NULL;
	if (ActiveSnapshotSet())
		return GetActiveSnapshot();
	return GetTransactionSnapshot();
}

/* A page lock on blkno: a directory leaf, or the root of a posting tree. */
void
lion_reader_lock_page(Relation index, BlockNumber blkno)
{
	Snapshot	snapshot = lion_reader_predsnap();

	if (snapshot != NULL && BlockNumberIsValid(blkno))
		PredicateLockPage(index, blkno, snapshot);
}

/*
 * The entry a reader takes, read from the directory leaf `leaf` while that is
 * share-locked, or copied from it under the lock: the leaf for an INLINE set,
 * the posting tree's root for a CHAIN one.
 */
void
lion_reader_lock_entry(Relation index, BlockNumber leaf,
					   const LionEntryTuple *entry)
{
	Snapshot	snapshot = lion_reader_predsnap();

	if (snapshot == NULL)
		return;
	if ((entry->flags & LION_ENTRY_CHAIN) != 0 &&
		BlockNumberIsValid(lion_entry_head(entry)))
		PredicateLockPage(index, lion_entry_head(entry), snapshot);
	else
		PredicateLockPage(index, leaf, snapshot);
}

/*
 * A key a lookup did not find: the leaf it would go on, which is the leaf the
 * lookup ended on (buf, still locked) - unless the lookup followed a run of
 * colliding hashes across a page boundary (movedright), and an insert then
 * starts from a leaf to the left of it, or it found no leaf at all
 * (InvalidBuffer: the cross-type fallback of lion_dir_find(), which walks
 * every leaf).  Both are rare, and the whole index is locked for them.
 */
void
lion_reader_lock_absent(Relation index, Buffer buf, bool movedright)
{
	Snapshot	snapshot = lion_reader_predsnap();

	if (snapshot == NULL)
		return;
	if (BufferIsValid(buf) && !movedright)
		PredicateLockPage(index, BufferGetBlockNumber(buf), snapshot);
	else
		PredicateLockRelation(index, snapshot);
}

/*
 * A leaf a walk reads, share-locked in buf, and the root of every CHAIN entry
 * of key column attno on it.  The walk takes its entries from a copy of the
 * page after the lock is gone, and decides on some of them from the copy's
 * counters alone, so they are locked here, while the leaf is held - all of
 * the column's, since only the first and the last leaf of a range hold
 * entries the walk passes over.
 */
void
lion_reader_lock_leaf(Relation index, Buffer buf, AttrNumber attno)
{
	Snapshot	snapshot = lion_reader_predsnap();
	Page		page;
	OffsetNumber off;
	OffsetNumber maxoff;

	if (snapshot == NULL)
		return;
	PredicateLockPage(index, BufferGetBlockNumber(buf), snapshot);

	page = BufferGetPage(buf);
	maxoff = PageGetMaxOffsetNumber(page);
	for (off = lion_page_first_data(page); off <= maxoff; off++)
	{
		ItemId		iid = PageGetItemId(page, off);
		LionEntryTuple *entry;

		if (!ItemIdIsUsed(iid))
			continue;
		entry = lion_entry_at(page, iid);
		if (entry->attno == attno && !LionEntryIsPivot(entry) &&
			(entry->flags & LION_ENTRY_CHAIN) != 0 &&
			BlockNumberIsValid(lion_entry_head(entry)))
			PredicateLockPage(index, lion_entry_head(entry), snapshot);
	}
}

/*
 * The locks on page `from` now stand for page `to` as well: a directory leaf
 * split (the new right sibling), the spill of an INLINE set (its new root).
 * The caller holds `from` exclusive and has not changed it yet, and no record
 * is open: this may ERROR when the lock table is full.
 */
void
lion_predlock_copy(Relation index, BlockNumber from, BlockNumber to)
{
	PredicateLockPageSplit(index, from, to);
}

/*
 * The same when `from` is going away: the root of a CHAIN entry VACUUM
 * deletes, whose key then lives, or would, on its leaf `to`.  PostgreSQL
 * leaves `from`'s locks in place as well, which once the block is reused can
 * only add false conflicts.
 */
void
lion_predlock_move(Relation index, BlockNumber from, BlockNumber to)
{
	PredicateLockPageCombine(index, from, to);
}

/*
 * The insert side: before an insert changes the entry at (leafbuf, off), or
 * with found false adds one there, check the page that stands for its key
 * against the predicate locks of other SERIALIZABLE transactions - the root
 * of a CHAIN entry's posting tree, else the leaf.  The leaf is held EXCLUSIVE,
 * as every writer of a key holds it from its lookup to its last change
 * (DESIGN.md §5), so no reader can take a lock on the page between this
 * check and the change.
 */
void
lion_writer_check(Relation index, Buffer leafbuf, OffsetNumber off,
				  bool found)
{
	BlockNumber target = BufferGetBlockNumber(leafbuf);

	if (found)
	{
		LionEntryTuple *entry = lion_page_entry(BufferGetPage(leafbuf), off);

		if ((entry->flags & LION_ENTRY_CHAIN) != 0 &&
			BlockNumberIsValid(lion_entry_head(entry)))
			target = lion_entry_head(entry);
	}
	CheckForSerializableConflictIn(index, NULL, target);
}

/*
 * The name of relid when it is a partitioned index, palloc'd, and NULL
 * otherwise; *islion says whether it is a lion one.  Such an index is only
 * the definition its partitions' indexes are made from: it has no storage,
 * and every function that reads an index reads one partition's.
 */
static char *
lion_partitioned_index_name(Oid relid, bool *islion)
{
	HeapTuple	tp = SearchSysCache1(RELOID, ObjectIdGetDatum(relid));
	char	   *name = NULL;

	*islion = false;
	if (HeapTupleIsValid(tp))
	{
		Form_pg_class form = (Form_pg_class) GETSTRUCT(tp);

		if (form->relkind == RELKIND_PARTITIONED_INDEX)
		{
			name = pstrdup(NameStr(form->relname));
			*islion = form->relam == lion_get_am_oid();
		}
		ReleaseSysCache(tp);
	}
	return name;
}

/* Is relid a partitioned lion index (above)? */
bool
lion_is_partitioned_index(Oid relid)
{
	bool		islion;

	return lion_partitioned_index_name(relid, &islion) != NULL && islion;
}

/*
 * ERROR when relid is a partitioned index (above), saying what it is: left
 * to the checks for an index, one would be called "not an index", which \d
 * contradicts.  One of another method is not a lion index, as its
 * partitions' indexes are not.
 */
void
lion_refuse_partitioned_index(Oid relid)
{
	bool		islion;
	char	   *name = lion_partitioned_index_name(relid, &islion);

	if (name == NULL)
		return;
	if (!islion)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("index \"%s\" is not a lion index", name)));
	ereport(ERROR,
			(errcode(ERRCODE_WRONG_OBJECT_TYPE),
			 errmsg("\"%s\" is a partitioned index", name),
			 errdetail("A partitioned index has no storage: each partition's index holds the entries of its rows."),
			 errhint("Pass the index of one of its partitions.")));
}
