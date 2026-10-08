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
 *	  lion_hooktest.rmgr_id, RM_EXPERIMENTAL_ID by default - pg_lion's own
 *	  default, so that the collision is the default case;
 *	- a GUC prefix of its own, reserved the way pg_lion reserves "pg_lion".
 *
 * It also provides lion_hooktest_heapcopy_handler(), a table access method
 * whose routine is a copy of the heap's: a table AM that stores heap tuples
 * but is not, by pointer, the heap, which is the only kind of "other" table
 * AM a test can create without shipping a real one.
 *
 * And lion_hooktest_priv_decode(), which feeds pg_lion's decoder of a
 * LionCount plan's custom_private (src/lion_plan_private.h) lists no planner
 * makes.  It has nothing to do with the hooks: this module is simply the
 * test-only C code the checks already build.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/tableam.h"
#include "access/xlog_internal.h"
#include "fmgr.h"
#include "miscadmin.h"
#include "nodes/extensible.h"
#include "nodes/pathnodes.h"
#include "optimizer/paths.h"
#include "optimizer/planner.h"
#include "utils/guc.h"

#include "lion_plan_private.h"

PG_MODULE_MAGIC;

typedef void (*lion_priv_decode_fn) (List *priv, LionPrivStage stage,
									 LionCountPriv *out);

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
	MarkGUCPrefixReserved("lion_hooktest");

	prev_create_upper_paths_hook = create_upper_paths_hook;
	create_upper_paths_hook = hooktest_create_upper_paths;
	prev_set_rel_pathlist_hook = set_rel_pathlist_hook;
	set_rel_pathlist_hook = hooktest_set_rel_pathlist;
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
 * pg_lion's decoder of a LionCount plan's custom_private
 * (lion_count_priv_decode(), src/lion_plan_private.h), handed a list a
 * planner of this build would make - one equality clause, a count - with one
 * thing changed, named by `variant`; the number of clauses it decoded.  Every
 * variant but "plan" and "path" is a list it must refuse: no plan carries
 * one, so this is the only way to see that it does.
 */
Datum
lion_hooktest_priv_decode(PG_FUNCTION_ARGS)
{
	char	   *variant = text_to_cstring(PG_GETARG_TEXT_PP(0));
	lion_priv_decode_fn decode;
	LionPrivStage stage = LION_PRIV_STAGE_PLAN;
	LionCountPriv out;
	List	   *priv = NIL;
	int			n;

	decode = (lion_priv_decode_fn)
		load_external_function("$libdir/pg_lion", "lion_count_priv_decode",
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
			list_make1_int(LION_CLAUSE_NE + 1);
	else if (strcmp(variant, "path join at plan") == 0)
		lfirst(list_nth_cell(priv, LION_PRIV_JOIN)) =
			list_make5_int(0, LION_JOIN_INNER, 0, 0, 0);
	else if (strcmp(variant, "or length") == 0)
		lfirst(list_nth_cell(priv, LION_PRIV_ORS)) =
			list_make1(list_make3_int(0, 2, 1));
	else if (strcmp(variant, "partition length") == 0)
		lfirst(list_nth_cell(priv, LION_PRIV_PARTS)) =
			list_make1(list_make3_oid(RelationRelationId, InvalidOid,
									  InvalidOid));
	else if (strcmp(variant, "path wagg at plan") == 0)
		lfirst(list_nth_cell(priv, LION_PRIV_WAGG)) =
			list_make3(list_make1_int(1), list_make1_oid(ClassOidIndexId),
					   list_make1_int(1));
	else
		elog(ERROR, "unknown variant \"%s\"", variant);

	decode(priv, stage, &out);
	PG_RETURN_INT32(out.nclause);
}
