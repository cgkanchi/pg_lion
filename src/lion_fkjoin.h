/*-------------------------------------------------------------------------
 *
 * lion_fkjoin.h
 *		Recognising the FK-side join the count pushdown answers (DESIGN.md
 *		§27): a fact table joined to a dimension table on one equality, the
 *		dimension's column provably unique.
 *
 *		lion_fkjoin.c finds the join clause and proves the dimension key
 *		unique; everything about the FACT side - its WHERE clauses, the lion
 *		index that answers the join column, the path, the plan and the
 *		executor - is lion_customscan.c's, which treats the join key as one
 *		more equality clause whose value comes from the dimension's rows.
 *
 *-------------------------------------------------------------------------
 */
#ifndef LION_FKJOIN_H
#define LION_FKJOIN_H

#include "nodes/pathnodes.h"

/*
 * One way round of a recognised join.  A join of two tables may qualify both
 * ways - each side's column unique and indexed - and then the caller gets two
 * of these and tries both, letting the cost model choose.
 */
typedef struct LionFkJoin
{
	RelOptInfo *factrel;		/* the side whose posting sets are counted */
	RelOptInfo *dimrel;			/* the side run as the node's child plan */
	Var		   *fkvar;			/* the fact's join column */
	Var		   *pkvar;			/* the dimension's join column */
	Node	   *pkexpr;			/* ... as the clause compares it: pkvar, or a
								 * binary-coercion relabel of it, whose type is
								 * the type the lookup is made with */
	Oid			opno;			/* the join clause's operator */
	Oid			collation;		/* and its input collation, or InvalidOid */
	Node	   *clause;			/* the join clause, for selectivity */
	Path	   *dimpath;		/* the dimension's cheapest total path */
} LionFkJoin;

/*
 * How many orientations of joinrel qualify (0, 1 or 2), filled into out[0]
 * and out[1].  Everything returned has passed every check that is about the
 * JOIN and the DIMENSION; whether the fact side has a lion index that can
 * answer the join operator, and whether its own quals can be pushed down, is
 * for the caller to decide.
 */
extern int	lion_fkjoin_recognize(PlannerInfo *root, RelOptInfo *joinrel,
								  LionFkJoin *out);

#endif							/* LION_FKJOIN_H */
