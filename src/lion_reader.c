/*-------------------------------------------------------------------------
 *
 * lion_reader.c
 *		What every direct reader of a lion index owes the caller (DESIGN.md
 *		§9, "Direct readers").
 *
 * An index scan that the executor starts through index_beginscan() gets a
 * number of guarantees from PostgreSQL for free: the planner only offers an
 * index the snapshot may use, row-level security is applied above the scan,
 * and index_beginscan() takes a relation-level predicate lock for an AM
 * without ampredlocks.  lion reads its indexes without a scan in two kinds
 * of places, and each has to provide what is missing itself:
 *
 *	- the SQL-callable functions that are handed an index by name (the count
 *	  functions, lion_bm25(), lion_bm25_score()), which nothing vetted:
 *	  lion_reader_open() for a whole index-and-table open, or
 *	  lion_reader_vet() for a caller that opens them itself;
 *	- the custom scans' executor nodes, whose index the planner vetted: for
 *	  them only the predicate lock is missing, which lion_reader_lock()
 *	  takes.
 *
 * What a reader is vetted for depends on what it returns:
 *
 *	LION_READ_ROWS	rows, or counts of rows, as a query would: the index must
 *					be usable under the snapshot, a table with row-level
 *					security applying to the caller is refused (the policies
 *					cannot be evaluated without reading the rows), an
 *					unpopulated materialized view is refused as the executor
 *					refuses it, and the predicate lock is taken before the
 *					first read, so that a SERIALIZABLE read that finds
 *					nothing still conflicts with a later insert of it.
 *	LION_READ_STATS	only index-wide statistics, as lion_bm25_score() reads
 *					N, df and avgdl: the privilege checks alone.  It can be
 *					called per row of a query that applies the policies.
 *
 * Privileges beyond SELECT on the table - per-column grants, EXECUTE on the
 * functions a call stands for - depend on what the call stands for, and stay
 * with the caller (lion_count_open_indexes()).
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/table.h"
#include "catalog/index.h"
#include "catalog/pg_class.h"
#include "miscadmin.h"
#include "storage/predicate.h"
#include "utils/acl.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/rls.h"

#include "lion.h"
#include "lion_count.h"

/*
 * Refuse what an ordinary scan of index under snapshot would not return, and
 * take the predicate lock index_beginscan() would (LION_READ_ROWS above).
 * action reads after "cannot ... through index": "count", "rank".
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

	/*
	 * Row-level security: the policies would have to be evaluated per row,
	 * and a direct reader does not look at rows.  The same query through the
	 * planner still works: lion's custom paths decline relations with
	 * security quals, and the ordinary plan applies them.
	 */
	if (check_enable_rls(RelationGetRelid(heap), InvalidOid, false) ==
		RLS_ENABLED)
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("cannot %s through index \"%s\" because row-level security is enabled on table \"%s\"",
						action, RelationGetRelationName(index),
						RelationGetRelationName(heap))));

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

	lion_reader_lock(index, snapshot);
}

/*
 * The relation-level predicate lock index_beginscan() takes on an index whose
 * AM has no ampredlocks (indexam.c), for a node or function that reads a lion
 * index without a scan.  Taken before the first read, so that a SERIALIZABLE
 * reader that finds nothing is covered too.
 */
void
lion_reader_lock(Relation index, Snapshot snapshot)
{
	PredicateLockRelation(index, snapshot);
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
	if (policy == LION_READ_ROWS)
	{
		lion_check_table_am(table);
		lion_reader_vet(table, index, snapshot, action);
	}

	/* not given: still locked until the transaction ends, as the index is */
	if (heap != NULL)
		*heap = table;
	else
		table_close(table, NoLock);
	return index;
}
