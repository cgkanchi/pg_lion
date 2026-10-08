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

/* One WHERE clause: CLAUSEKINDS, OIDS[3 + i], INTS[4 + i] and CLAUSEOPS */
typedef struct LionCountPrivClause
{
	int			kind;			/* LION_CLAUSE_* */
	Oid			idxoid;			/* InvalidOid for a partitioned table */
	AttrNumber	attno;			/* the PARENT's */
	Oid			opno;			/* InvalidOid for a null test */
} LionCountPrivClause;

/* One live leaf partition (DESIGN.md §16): an OidList of PARTS */
typedef struct LionCountPrivPart
{
	Oid			heapoid;
	Oid			groupidxoid;
	Oid			groupidxoid2;
	Oid		   *clauseidxoid;	/* nclause; InvalidOid for a clause the
								 * partition's bounds imply */
} LionCountPrivPart;

/* The FK-side join (DESIGN.md §27): JOIN */
typedef struct LionCountPrivJoin
{
	int			clause;			/* the join key's clause */
	int			type;			/* LION_JOIN_* */
	int			flags;			/* LION_JOINFLAG_* */
	Oid			sortop;			/* LION_JOINFLAG_UNIQUE's, or InvalidOid */
	Oid			sortcoll;
	AttrNumber	keyresno;		/* the key's column of the child: PLAN
								 * stage only, 0 at the PATH stage */
} LionCountPrivJoin;

/* A column the aggregates of DESIGN.md §37 are taken over: WAGG's lists */
typedef struct LionCountPrivWCol
{
	AttrNumber	attno;
	Oid			idxoid;
	AttrNumber	idxcol;
} LionCountPrivWCol;

/* ... and one of those aggregates, from WAGG's fourth list (PLAN stage) */
typedef struct LionCountPrivWAgg
{
	int			kind;			/* LION_WAGG_* */
	int			col;			/* its column, in wcol */
	int			argwidth;
	Oid			aggfnoid;		/* LION_WAGG_EXTREME: the aggregate ... */
	Oid			collation;		/* ... and its collation */
} LionCountPrivWAgg;

/*
 * The whole list.  Lists and nodes point into the plan, which outlives
 * every reader; the arrays are palloc'd in the caller's context.  A member
 * the plan leaves empty decodes as zero, NULL or NIL.
 */
typedef struct LionCountPriv
{
	/* OIDS and INTS: the relation and its drivers */
	Oid			heapoid;		/* the PARENT's, for a partitioned table */
	Oid			groupidxoid;	/* InvalidOid if none, or partitioned */
	Oid			groupidxoid2;	/* ... and InvalidOid for the decoded walk */
	Index		scanrelid;
	AttrNumber	groupattno;
	AttrNumber	groupattno2;	/* 0 for the decoded walk too */
	int			flags;			/* LION_FLAG_* */

	int			nclause;
	LionCountPrivClause *clause;

	int			npart;			/* PARTS: 0 for a plain table */
	LionCountPrivPart *part;

	int			nor;			/* ORS (DESIGN.md §19), nleaves summed */
	LionOrState *ors;

	AttrNumber	distattno;		/* DISTINCT (§26), or 0 */

	bool		hasjoin;		/* JOIN (§27) */
	LionCountPrivJoin join;

	/* EXECUTE (DESIGN.md §9, "Privileges"): NIL or an OidList each */
	List	   *exec_aggs;		/* the target list's and HAVING's aggregates */
	List	   *exec_funcs;		/* the WHERE clauses' and join clause's */
	List	   *exec_groupfuncs;	/* the GROUP BY's equality functions */

	Const	   *coalconst;		/* COALESCE (§10), or NULL */
	Oid			coaleqop;
	Oid			coalcoll;

	List	   *implied;		/* IMPLIED (§16): String nodes */

	/* FACTGROUP (§27, "Grouped by a fact column"): Max(npart, 1) each */
	AttrNumber	fgattno;		/* or 0 */
	Oid		   *fgidxoid;		/* InvalidOid where fgconst is the value */
	Const	  **fgconst;

	/* GROUPN (§34): the decoded walk's columns, or ngroupn 0 */
	int			ngroupn;
	AttrNumber	groupn_attno[LION_MAX_GROUPCOLS];
	Oid			groupn_idx[LION_MAX_GROUPCOLS];

	AttrNumber	allattno;		/* ALLROWS (§35), or 0 */

	int64		topkn;			/* TOPK (§36), or 0 */
	int			topkcand;
	bool		topkstrict;

	int			nwcol;			/* WAGG (§37), or 0 */
	LionCountPrivWCol *wcol;
	int			nwagg;			/* PLAN stage only */
	LionCountPrivWAgg *wagg;

	int			ntl;			/* TLKINDS: PLAN stage only */
	int		   *tlkind;

	List	   *consts;			/* CONSTS: PATH stage only */
	List	   *having;			/* HAVING: PATH stage only */
} LionCountPriv;

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
