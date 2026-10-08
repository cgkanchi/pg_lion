/*-------------------------------------------------------------------------
 *
 * lion_plan_private.h
 *		The custom_private of a LionCount plan as a C struct: the positional
 *		list lion_customscan.h describes (LION_PRIV_*), decoded and checked
 *		in one place.
 *
 * custom_private has to be a list of copyable, serialisable nodes, so the
 * planner writes it positionally, behind a shape marker (DESIGN.md §10).
 * lion_count_priv_decode() is the one reader of that list: it checks every
 * member's node type and every per-clause and per-partition length before
 * anything takes a value out of it, and lion_count_priv_check() holds the
 * rules BETWEEN members - a top k is one column's walk, a fact group needs
 * an inner join - that a plan this build wrote always keeps.  Either one
 * failing is planner/executor drift, or a plan from another build, and is
 * an ERROR rather than a misread Oid.  lion_count_priv_encode() writes a
 * struct back as that list, the same members in the same positions.
 *
 * The list has two stages.  The PATH's is 19 members (its marker already
 * says 20): the clause values in CONSTS and the HAVING in HAVING, the JOIN
 * member without the child's column and WAGG without its aggregates.  The
 * PLAN's has CONSTS and HAVING empty - lion_plan_custom_path() moves them to
 * custom_exprs and plan.qual - JOIN and WAGG complete, and the target-list
 * kinds appended as member 19.
 *
 * Part of the LionCount custom scan: lion_customscan.h describes the node
 * and declares what its files share.
 *
 *-------------------------------------------------------------------------
 */
#ifndef LION_PLAN_PRIVATE_H
#define LION_PLAN_PRIVATE_H

#include "lion_customscan.h"

/* Which of the two lists above is being read. */
typedef enum LionPrivStage
{
	LION_PRIV_STAGE_PATH,		/* a CustomPath's: 19 members */
	LION_PRIV_STAGE_PLAN		/* a CustomScan's: 20 members */
} LionPrivStage;

/*
 * The decoded list itself, LionCountPriv and the structs it is made of, is
 * in lion_customscan.h: the scan state keeps the plan's as st->plan.
 */

/*
 * Exported, unlike the rest of the library's internals, for the decoder's
 * own test: test/modules/lion_hooktest feeds it malformed lists.
 */
extern PGDLLEXPORT void lion_count_priv_decode(List *priv, LionPrivStage stage,
											   LionCountPriv *out);
extern List *lion_count_priv_encode(const LionCountPriv *p,
								   LionPrivStage stage);
extern void lion_count_priv_check(const LionCountPriv *p, bool parallel_aware,
								  int ncustom_plans);
extern AttrNumber lion_count_priv_drive_attno(const LionCountPriv *p);
extern AttrNumber lion_count_priv_inner_attno(const LionCountPriv *p);
extern LionCountMode lion_count_mode_of(const LionCountPriv *p,
										bool parallel_aware);

/* For the path builders: the planner's lists, into the struct */
extern void lion_count_priv_set_where(LionCountPriv *p, List *whereidx,
									  List *whereattnos, List *whereconsts,
									  List *wherekinds, List *whereopnos,
									  List *ors);
extern void lion_count_priv_set_parts(LionCountPriv *p, List *targets,
									  bool groupidx);

#endif							/* LION_PLAN_PRIVATE_H */
