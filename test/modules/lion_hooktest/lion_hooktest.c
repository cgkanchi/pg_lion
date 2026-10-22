/*-------------------------------------------------------------------------
 *
 * lion_hooktest.c
 *		A stand-in for "another extension" in the hook-coexistence checks of
 *		DESIGN.md §23 (test/hook-check.sh).  Test-only; the pg_lion build
 *		never builds or installs it.
 *
 * It does to the hooks pg_lion uses what a planner extension such as Citus
 * or TimescaleDB does, and nothing more:
 *
 *	- create_upper_paths_hook, chained to whatever was installed before it,
 *	  counting its calls and noting whether a LionCount path was already in
 *	  the grouped rel when the previous hook returned - which tells in which
 *	  order the two hooks were installed;
 *	- set_rel_pathlist_hook, the same way for the LionOrdered path pg_lion
 *	  adds to a base rel (DESIGN.md §30);
 *	- when preloaded, a custom WAL resource manager under the id in
 *	  lion_hooktest.rmgr_id, RM_EXPERIMENTAL_ID by default (pg_lion's own
 *	  is its reserved 170; test/hook-check.sh moves this one onto it for
 *	  the collision);
 *	- a GUC prefix of its own, reserved the way pg_lion reserves "pg_lion";
 *	- object_access_hook, refusing the execution of the one function named by
 *	  OID in lion_hooktest.deny_execute, as a security module such as sepgsql
 *	  refuses one: the hook that InvokeFunctionExecuteHook() calls, which a
 *	  lion node that stands in for an expression must call as the executor
 *	  would have (sql/exec_hook.sql).
 *
 * It also provides lion_hooktest_heapcopy_handler(), a table access method
 * whose routine is a copy of the heap's: a table AM that stores heap tuples
 * but is not, by pointer, the heap, which is the only kind of "other" table
 * AM a test can create without shipping a real one.
 *
 * And lion_hooktest_priv_decode(), which feeds pg_lion's decoder and check of a
 * LionCount plan's custom_private (src/lion_plan_private.h) lists no planner
 * makes.  It has nothing to do with the hooks: this module is simply the
 * test-only C code the checks already build.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/tableam.h"
#include "access/xlog_internal.h"
#include "catalog/objectaccess.h"
#include "catalog/pg_proc.h"
#include "fmgr.h"
#include "miscadmin.h"
#include "nodes/extensible.h"
#include "nodes/pathnodes.h"
#include "optimizer/paths.h"
#include "optimizer/planner.h"
#include "utils/guc.h"
#include "utils/regproc.h"

#include "lion_plan_private.h"

PG_MODULE_MAGIC;

typedef void (*lion_priv_decode_fn) (List *priv, LionPrivStage stage,
									 LionCountPriv *out);
typedef void (*lion_priv_check_fn) (const LionCountPriv *p,
									bool parallel_aware, int ncustom_plans);

PG_FUNCTION_INFO_V1(lion_hooktest_calls);
PG_FUNCTION_INFO_V1(lion_hooktest_saw_lion);
PG_FUNCTION_INFO_V1(lion_hooktest_reset);
PG_FUNCTION_INFO_V1(lion_hooktest_rel_calls);
PG_FUNCTION_INFO_V1(lion_hooktest_rel_saw_ordered);
PG_FUNCTION_INFO_V1(lion_hooktest_heapcopy_handler);
PG_FUNCTION_INFO_V1(lion_hooktest_priv_decode);

static create_upper_paths_hook_type prev_create_upper_paths_hook = NULL;
static int64 hook_calls = 0;
static int64 hook_saw_lion = 0;
static set_rel_pathlist_hook_type prev_set_rel_pathlist_hook = NULL;
static int64 rel_hook_calls = 0;
static int64 rel_hook_saw_ordered = 0;
static int	hooktest_rmgr_id = RM_EXPERIMENTAL_ID;
static TableAmRoutine heapcopy_routine;
static object_access_hook_type prev_object_access_hook = NULL;
static char *hooktest_deny_execute = NULL;

static void
hooktest_redo(XLogReaderState *record)
{
	elog(PANIC, "lion_hooktest writes no WAL");
}

static const RmgrData hooktest_rmgr = {
	.rm_name = "lion_hooktest",
	.rm_redo = hooktest_redo,
};

static void
hooktest_create_upper_paths(PlannerInfo *root, UpperRelationKind stage,
							RelOptInfo *input_rel, RelOptInfo *output_rel,
							void *extra)
{
	ListCell   *lc;

	hook_calls++;

	if (prev_create_upper_paths_hook != NULL)
		prev_create_upper_paths_hook(root, stage, input_rel, output_rel, extra);

	if (stage != UPPERREL_GROUP_AGG || output_rel == NULL)
		return;

	/*
	 * pg_lion adds its path to the grouped rel directly, or under a Finalize
	 * Aggregate for a partitioned GROUP BY.  Either way it is there now only
	 * if pg_lion's hook has already run, i.e. was installed before this one
	 * and is the hook this one chained to.
	 */
	foreach(lc, output_rel->pathlist)
	{
		Path	   *path = (Path *) lfirst(lc);

		if (IsA(path, AggPath))
			path = ((AggPath *) path)->subpath;
		if (IsA(path, CustomPath) &&
			strcmp(((CustomPath *) path)->methods->CustomName, "LionCount") == 0)
		{
			hook_saw_lion++;
			break;
		}
	}
}

/*
 * The same for set_rel_pathlist_hook: pg_lion's LionOrdered path is in the
 * base rel's path list when the previous hook returns only if pg_lion's hook
 * was installed before this one.
 */
static void
hooktest_set_rel_pathlist(PlannerInfo *root, RelOptInfo *rel, Index rti,
						  RangeTblEntry *rte)
{
	ListCell   *lc;

	rel_hook_calls++;

	if (prev_set_rel_pathlist_hook != NULL)
		prev_set_rel_pathlist_hook(root, rel, rti, rte);

	foreach(lc, rel->pathlist)
	{
		Path	   *path = (Path *) lfirst(lc);

		if (IsA(path, CustomPath) &&
			strcmp(((CustomPath *) path)->methods->CustomName, "LionOrdered") == 0)
		{
			rel_hook_saw_ordered++;
			break;
		}
	}
}

/*
 * The function lion_hooktest.deny_execute names (an OID, empty for none) may
 * not be executed: refused as a privilege error, as sepgsql refuses one.
 */
static void
hooktest_object_access(ObjectAccessType access, Oid classId, Oid objectId,
					   int subId, void *arg)
{
	if (prev_object_access_hook != NULL)
		prev_object_access_hook(access, classId, objectId, subId, arg);

	if (access == OAT_FUNCTION_EXECUTE && classId == ProcedureRelationId &&
		hooktest_deny_execute != NULL && hooktest_deny_execute[0] != '\0' &&
		objectId == atooid(hooktest_deny_execute))
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("lion_hooktest: execution of %s denied",
						format_procedure(objectId))));
}

void
_PG_init(void)
{
	if (process_shared_preload_libraries_in_progress)
	{
		DefineCustomIntVariable("lion_hooktest.rmgr_id",
								"WAL resource manager id lion_hooktest registers under.",
								NULL,
								&hooktest_rmgr_id,
								RM_EXPERIMENTAL_ID,
								RM_MIN_CUSTOM_ID, RM_MAX_CUSTOM_ID,
								PGC_POSTMASTER,
								0,
								NULL, NULL, NULL);
		RegisterCustomRmgr((RmgrId) hooktest_rmgr_id, &hooktest_rmgr);
	}
	DefineCustomStringVariable("lion_hooktest.deny_execute",
							   "OID of a function whose execution the object access hook refuses.",
							   NULL,
							   &hooktest_deny_execute,
							   "",
							   PGC_SUSET,
							   0,
							   NULL, NULL, NULL);
	MarkGUCPrefixReserved("lion_hooktest");

	prev_create_upper_paths_hook = create_upper_paths_hook;
	create_upper_paths_hook = hooktest_create_upper_paths;
	prev_set_rel_pathlist_hook = set_rel_pathlist_hook;
	set_rel_pathlist_hook = hooktest_set_rel_pathlist;
	prev_object_access_hook = object_access_hook;
	object_access_hook = hooktest_object_access;
}

Datum
lion_hooktest_calls(PG_FUNCTION_ARGS)
{
	PG_RETURN_INT64(hook_calls);
}

Datum
lion_hooktest_saw_lion(PG_FUNCTION_ARGS)
{
	PG_RETURN_INT64(hook_saw_lion);
}

Datum
lion_hooktest_reset(PG_FUNCTION_ARGS)
{
	hook_calls = 0;
	hook_saw_lion = 0;
	rel_hook_calls = 0;
	rel_hook_saw_ordered = 0;
	PG_RETURN_VOID();
}

Datum
lion_hooktest_rel_calls(PG_FUNCTION_ARGS)
{
	PG_RETURN_INT64(rel_hook_calls);
}

Datum
lion_hooktest_rel_saw_ordered(PG_FUNCTION_ARGS)
{
	PG_RETURN_INT64(rel_hook_saw_ordered);
}

Datum
lion_hooktest_heapcopy_handler(PG_FUNCTION_ARGS)
{
	heapcopy_routine = *GetHeapamTableAmRoutine();
	PG_RETURN_POINTER(&heapcopy_routine);
}

/*
 * The CELLS member a planner of this build makes for a count by cells of
 * `oid = 1` alone (DESIGN.md §10, "Counts by cells"), at the plan stage: one
 * scalar dimension of pg_class.oid, NOT NULL, its value outcome 1 and every
 * other value outcome 0; one predicate, true in outcome 1 alone; no driver
 * among the dimensions.
 */
static List *
lion_hooktest_cells(void)
{
	List	   *dim;

	dim = list_make5(list_make5_int(LION_CELLDIM_SCALAR, Anum_pg_class_oid,
									2, 0, 0),
					 list_make3_oid(ClassOidIndexId, (Oid) 607, InvalidOid),
					 list_make1(makeConst(OIDOID, -1, InvalidOid,
										  sizeof(Oid), ObjectIdGetDatum(1),
										  false, true)),
					 list_make1_int(1),
					 NIL);
	dim = lappend(dim, NIL);
	return list_make3(list_make3_int(1, 1, -1), list_make1(dim),
					  list_make1(list_make5_int(0, LION_CELLPRED_TABLE, 0,
												LION_TRUTH_FALSE,
												LION_TRUTH_TRUE)));
}

/*
 * The ANTI member of a plan of the anti join over a small inner side
 * (DESIGN.md §27) looking `nkey` keys up in pg_class's oid index, its one
 * join clause and no restriction.
 */
static List *
lion_hooktest_anti(int nkey)
{
	List	   *ints = list_make3_int(nkey, 1, 0);
	List	   *oids = list_make1_oid(ClassOidIndexId);
	int			i;

	for (i = 0; i < nkey; i++)
		ints = lappend_int(ints, 1);
	for (i = 0; i < nkey; i++)
		oids = lappend_oid(oids, (Oid) 607);	/* oid = oid */
	for (i = 0; i < nkey; i++)
		oids = lappend_oid(oids, InvalidOid);
	for (i = 0; i < nkey; i++)
		oids = lappend_oid(oids, (Oid) 609);	/* oid < oid */
	for (i = 0; i < 6; i++)
		oids = lappend_oid(oids, InvalidOid);
	return list_make2(ints, oids);
}

/*
 * A count by cells driven by pg_class.oid's walk, beside the anti join over
 * a small inner side (DESIGN.md §27, "Counts by cells beside the anti
 * join"), at the plan stage: the decoded walk of the driver, the CELLS and
 * ANTI members, and a target list of the predicate's truth, the count and
 * the columns the anti join reads - the table's (RESID) and the inner
 * side's (ANTI).  `ngroupn` columns walked, one the plan's.
 */
static void
lion_hooktest_cells_anti(List *priv, int ngroupn)
{
	List	   *attnos = list_make1_int(Anum_pg_class_oid);
	List	   *oids = list_make1_oid(ClassOidIndexId);

	if (ngroupn > 1)
	{
		attnos = lappend_int(attnos, Anum_pg_class_relname);
		oids = lappend_oid(oids, ClassNameNspIndexId);
	}
	lfirst(list_nth_cell(priv, LION_PRIV_OIDS)) =
		list_make4_oid(RelationRelationId, ClassOidIndexId, InvalidOid,
					   ClassOidIndexId);
	lfirst(list_nth_cell(priv, LION_PRIV_INTS)) =
		list_make5_int(1, Anum_pg_class_oid, 0,
					   LION_FLAG_DECODE | LION_FLAG_GROUPIDX,
					   Anum_pg_class_oid);
	lfirst(list_nth_cell(priv, LION_PRIV_GROUPN)) = list_make2(attnos, oids);
	lfirst(list_nth_cell(priv, LION_PRIV_CELLS)) = lion_hooktest_cells();
	lfirst(list_nth_cell(priv, LION_PRIV_ANTI)) = lion_hooktest_anti(1);
	lfirst(list_nth_cell(priv, LION_PRIV_TLKINDS)) =
		list_make4_int(LION_TL_CELLPRED(0), LION_TL_COUNT,
					   LION_TL_RESID(Anum_pg_class_oid), LION_TL_ANTI(1));
}

/*
 * pg_lion's decoder of a LionCount plan's custom_private, and the rules
 * between its members (lion_count_priv_decode() and lion_count_priv_check(),
 * src/lion_plan_private.h), handed a list a planner of this build would make
 * - one equality clause, a count - with one thing changed, named by
 * `variant`; the number of clauses it decoded.  Every variant but "plan",
 * "path" and the counts by cells beside an anti join, serial and parallel,
 * is a list it must refuse: no plan carries one, so this is the only way to
 * see that it does.
 */
Datum
lion_hooktest_priv_decode(PG_FUNCTION_ARGS)
{
	char	   *variant = text_to_cstring(PG_GETARG_TEXT_PP(0));
	lion_priv_decode_fn decode;
	lion_priv_check_fn check;
	LionPrivStage stage = LION_PRIV_STAGE_PLAN;
	LionCountPriv out;
	List	   *priv = NIL;
	bool		parallel = false;
	int			nplans = 0;		/* the CustomScan's children */
	int			n;

	decode = (lion_priv_decode_fn)
		load_external_function("$libdir/pg_lion", "lion_count_priv_decode",
							   true, NULL);
	check = (lion_priv_check_fn)
		load_external_function("$libdir/pg_lion", "lion_count_priv_check",
							   true, NULL);

	for (n = 0; n < LION_PRIV_NMEMBERS; n++)
	{
		List	   *m = NIL;

		switch (n)
		{
			case LION_PRIV_VERSION:
				m = list_make2_int(LION_PRIV_MAGIC, LION_PRIV_NMEMBERS);
				break;
			case LION_PRIV_OIDS:
				m = list_make4_oid(RelationRelationId, InvalidOid, InvalidOid,
								   ClassOidIndexId);
				break;
			case LION_PRIV_INTS:
				m = list_make5_int(1, 0, 0, 0, Anum_pg_class_oid);
				break;
			case LION_PRIV_CLAUSEKINDS:
				m = list_make1_int(LION_CLAUSE_EQ);
				break;
			case LION_PRIV_CLAUSEOPS:
				m = list_make1_oid((Oid) 607); /* oid = oid */
				break;
			case LION_PRIV_EXECUTE:
				m = list_make3(NIL, NIL, NIL);
				break;
			case LION_PRIV_TLKINDS:
				m = list_make1_int(LION_TL_COUNT);
				break;
		}
		priv = lappend(priv, m);
	}

	if (strcmp(variant, "plan") == 0)
		;
	else if (strcmp(variant, "path") == 0)
	{
		/* the path's list: no target-list kinds, and the clause's value */
		stage = LION_PRIV_STAGE_PATH;
		priv = list_truncate(priv, LION_PRIV_NMEMBERS - 1);
		lfirst(list_nth_cell(priv, LION_PRIV_CONSTS)) =
			list_make1(makeConst(OIDOID, -1, InvalidOid, sizeof(Oid),
								 ObjectIdGetDatum(InvalidOid), false, true));
	}
	else if (strcmp(variant, "magic") == 0)
		lfirst(list_nth_cell(priv, LION_PRIV_VERSION)) =
			list_make2_int(LION_PRIV_MAGIC - 1, LION_PRIV_NMEMBERS);
	else if (strcmp(variant, "path at plan") == 0)
		priv = list_truncate(priv, LION_PRIV_NMEMBERS - 1);
	else if (strcmp(variant, "short oids") == 0)
		lfirst(list_nth_cell(priv, LION_PRIV_OIDS)) =
			list_make3_oid(RelationRelationId, InvalidOid, InvalidOid);
	else if (strcmp(variant, "oids as ints") == 0)
		lfirst(list_nth_cell(priv, LION_PRIV_OIDS)) =
			list_make4_int(RelationRelationId, 0, 0, ClassOidIndexId);
	else if (strcmp(variant, "clause kind") == 0)
		lfirst(list_nth_cell(priv, LION_PRIV_CLAUSEKINDS)) =
			list_make1_int(LION_CLAUSE_MAXKIND + 1);
	else if (strcmp(variant, "path join at plan") == 0)
		lfirst(list_nth_cell(priv, LION_PRIV_JOIN)) =
			list_make5_int(0, LION_JOIN_INNER, 0, 0, 0);
	else if (strcmp(variant, "join filter on its key") == 0)
	{
		List	   *join = list_make5_int(0, LION_JOIN_INNER, 0, 0, 0);

		join = lappend_int(join, 1);	/* the key's column of the child */
		join = lappend_int(join, 0);	/* a filter on the key's clause */
		join = lappend_int(join, 1);
		lfirst(list_nth_cell(priv, LION_PRIV_JOIN)) = join;
	}
	else if (strcmp(variant, "or length") == 0)
		lfirst(list_nth_cell(priv, LION_PRIV_ORS)) =
			list_make1(list_make3_int(0, 2, 1));
	else if (strcmp(variant, "partition length") == 0)
		lfirst(list_nth_cell(priv, LION_PRIV_PARTS)) =
			list_make1(list_make3_oid(RelationRelationId, InvalidOid,
									  InvalidOid));
	else if (strcmp(variant, "expression column") == 0)
		lfirst(list_nth_cell(priv, LION_PRIV_INTS)) =
			list_make5_int(1, 0, 0, 0, LION_VCOL_ATTNO(0));
	else if (strcmp(variant, "bare expression column") == 0)
	{
		lfirst(list_nth_cell(priv, LION_PRIV_INTS)) =
			list_make5_int(1, 0, 0, 0, LION_VCOL_ATTNO(0));
		lfirst(list_nth_cell(priv, LION_PRIV_VCOLS)) =
			list_make1(makeVar(1, Anum_pg_class_oid, OIDOID, -1, InvalidOid, 0));
	}
	else if (strcmp(variant, "path wagg at plan") == 0)
		lfirst(list_nth_cell(priv, LION_PRIV_WAGG)) =
			list_make3(list_make1_int(1), list_make1_oid(ClassOidIndexId),
					   list_make1_int(1));
	else if (strcmp(variant, "aggregate without aggregates") == 0)
		lfirst(list_nth_cell(priv, LION_PRIV_TLKINDS)) =
			list_make1_int(LION_TL_WAGG(0));
	else if (strcmp(variant, "distinct count without its column") == 0)
		lfirst(list_nth_cell(priv, LION_PRIV_TLKINDS)) =
			list_make1_int(LION_TL_COUNT_DISTINCT);
	else if (strcmp(variant, "cells without the decoded walk") == 0)
		lfirst(list_nth_cell(priv, LION_PRIV_CELLS)) = lion_hooktest_cells();
	else if (strcmp(variant, "path cells at plan") == 0)
	{
		List	   *cells = lion_hooktest_cells();

		cells = lappend(cells, NIL);	/* the predicates' expressions */
		cells = lappend(cells, NIL);	/* ... and the WHERE over them */
		lfirst(list_nth_cell(priv, LION_PRIV_CELLS)) = cells;
	}
	else if (strcmp(variant, "cells header") == 0)
	{
		List	   *cells = lion_hooktest_cells();

		linitial(cells) = list_make3_int(2, 1, -1);
		lfirst(list_nth_cell(priv, LION_PRIV_CELLS)) = cells;
	}
	else if (strcmp(variant, "cells value outcome") == 0)
	{
		List	   *cells = lion_hooktest_cells();
		List	   *dim = (List *) linitial((List *) lsecond(cells));

		lfirst(list_nth_cell(dim, 3)) = list_make1_int(2);
		lfirst(list_nth_cell(priv, LION_PRIV_CELLS)) = cells;
	}
	else if (strcmp(variant, "cells predicate dimension") == 0)
	{
		List	   *cells = lion_hooktest_cells();

		lthird(cells) = list_make1(list_make5_int(1, LION_CELLPRED_TABLE, 0,
												  LION_TRUTH_FALSE,
												  LION_TRUTH_TRUE));
		lfirst(list_nth_cell(priv, LION_PRIV_CELLS)) = cells;
	}
	else if (strcmp(variant, "cells predicate truths") == 0)
	{
		List	   *cells = lion_hooktest_cells();

		lthird(cells) = list_make1(list_make4_int(0, LION_CELLPRED_TABLE, 0,
												  LION_TRUTH_TRUE));
		lfirst(list_nth_cell(priv, LION_PRIV_CELLS)) = cells;
	}
	else if (strcmp(variant, "cell predicate without cells") == 0)
		lfirst(list_nth_cell(priv, LION_PRIV_TLKINDS)) =
			list_make2_int(LION_TL_CELLPRED(0), LION_TL_COUNT);
	else if (strcmp(variant, "anti join without its child") == 0)
		lfirst(list_nth_cell(priv, LION_PRIV_ANTI)) = lion_hooktest_anti(1);
	else if (strcmp(variant, "anti join of no key") == 0)
		lfirst(list_nth_cell(priv, LION_PRIV_ANTI)) = lion_hooktest_anti(0);
	else if (strcmp(variant, "anti join of fewer join clauses than keys") == 0)
	{
		List	   *anti = lion_hooktest_anti(2);

		/* two scan keys, each a join clause, and one join clause */
		lfirst(list_nth_cell(priv, LION_PRIV_ANTI)) = anti;
		nplans = 1;
	}
	else if (strcmp(variant, "anti join of more join clauses than a list holds") == 0 ||
			 strcmp(variant, "anti join of more restrictions than a list holds") == 0 ||
			 strcmp(variant, "anti join of more expressions than a list holds") == 0)
	{
		List	   *anti = lion_hooktest_anti(1);
		List	   *ints = (List *) linitial(anti);

		/*
		 * The counts after the number of keys: the join clauses' and the
		 * restrictions'.  Each of the last pair fits a List alone, and their
		 * sum, with the key's, does not.
		 */
		if (strstr(variant, "join clauses") != NULL)
			lfirst_int(list_nth_cell(ints, 1)) = PG_INT32_MAX;
		else if (strstr(variant, "restrictions") != NULL)
			lfirst_int(list_nth_cell(ints, 2)) = PG_INT32_MAX;
		else
		{
			lfirst_int(list_nth_cell(ints, 1)) = LION_MAX_ANTI_EXPRS / 2 + 1;
			lfirst_int(list_nth_cell(ints, 2)) = LION_MAX_ANTI_EXPRS / 2;
		}
		lfirst(list_nth_cell(priv, LION_PRIV_ANTI)) = anti;
		nplans = 1;
	}
	else if (strcmp(variant, "anti join of a key without its order") == 0 ||
			 strcmp(variant, "anti join of a grouping equality without its order") == 0)
	{
		List	   *anti = lion_hooktest_anti(1);
		List	   *oids = (List *) lsecond(anti);

		/* the key's order, after the B-tree and its operator and collation */
		if (strstr(variant, "a key") != NULL)
			lfirst_oid(list_nth_cell(oids, 3)) = InvalidOid;
		else
			lfirst_oid(list_nth_cell(oids, 4)) = (Oid) 607;
		lfirst(list_nth_cell(priv, LION_PRIV_ANTI)) = anti;
		nplans = 1;
	}
	else if (strcmp(variant, "inner column without an anti join") == 0)
		lfirst(list_nth_cell(priv, LION_PRIV_TLKINDS)) =
			list_make2_int(LION_TL_COUNT, LION_TL_ANTI(1));
	else if (strcmp(variant, "cells beside an anti join") == 0)
	{
		lion_hooktest_cells_anti(priv, 1);
		nplans = 1;
	}
	else if (strcmp(variant, "parallel cells beside an anti join") == 0)
	{
		lion_hooktest_cells_anti(priv, 1);
		nplans = 1;
		parallel = true;
	}
	else if (strcmp(variant, "cells beside an anti join with a grouping equality") == 0)
	{
		List	   *anti = lion_hooktest_anti(1);
		List	   *oids = (List *) lsecond(anti);

		lion_hooktest_cells_anti(priv, 1);
		/*
		 * the first GROUP BY column's equality and its order, after the
		 * B-tree and the key's operator, collation and order
		 */
		lfirst_oid(list_nth_cell(oids, 4)) = (Oid) 607;
		lfirst_oid(list_nth_cell(oids, 6)) = (Oid) 609;
		lfirst(list_nth_cell(priv, LION_PRIV_ANTI)) = anti;
		nplans = 1;
	}
	else if (strcmp(variant, "anti join beside the decoded walk") == 0)
	{
		lion_hooktest_cells_anti(priv, 2);
		lfirst(list_nth_cell(priv, LION_PRIV_CELLS)) = NIL;
		lfirst(list_nth_cell(priv, LION_PRIV_TLKINDS)) =
			list_make1_int(LION_TL_COUNT);
		nplans = 1;
	}
	else if (strcmp(variant, "inner column past the last attribute") == 0)
	{
		/* a column a child's row has none of, nor an AttrNumber holds */
		lion_hooktest_cells_anti(priv, 1);
		lfirst(list_nth_cell(priv, LION_PRIV_TLKINDS)) =
			list_make4_int(LION_TL_CELLPRED(0), LION_TL_COUNT,
						   LION_TL_RESID(Anum_pg_class_oid),
						   LION_TL_ANTI(PG_INT16_MAX + 1));
		nplans = 1;
	}
	else if (strcmp(variant, "table column of a count by cells without an anti join") == 0)
	{
		lion_hooktest_cells_anti(priv, 1);
		lfirst(list_nth_cell(priv, LION_PRIV_ANTI)) = NIL;
		lfirst(list_nth_cell(priv, LION_PRIV_TLKINDS)) =
			list_make3_int(LION_TL_CELLPRED(0), LION_TL_COUNT,
						   LION_TL_RESID(Anum_pg_class_oid));
	}
	else
		elog(ERROR, "unknown variant \"%s\"", variant);

	decode(priv, stage, &out);
	check(&out, parallel, nplans);
	PG_RETURN_INT32(out.nclause);
}
