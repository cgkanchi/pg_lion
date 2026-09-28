/*-------------------------------------------------------------------------
 *
 * lion_fkjoin.h
 *		Recognising the FK-side join the count pushdown answers (DESIGN.md
 *		§27): a fact table joined to a dimension table on one equality, the
 *		dimension's column provably unique - or made unique, for a forward
 *		semi join.
 *
 *		lion_fkjoin.c finds the join clause and proves the dimension key
 *		unique; everything about the FACT side - its WHERE clauses, the lion
 *		index that answers the join column, the path, the plan and the
 *		executor - is lion_customscan.c's, which treats the join key as one
 *		more equality clause whose value comes from the dimension's rows.
 *
 *		The dimension may be a join itself: one base table that carries the
 *		key, and other tables joined to it only as the inner side of semi and
 *		anti joins, which never duplicate its rows (DESIGN.md §27, "A
 *		dimension that is a join").  Its rows are then the rows of that join
 *		rel's cheapest path, which the node runs as its child.
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
 *
 * A semi or anti join (`WHERE [NOT] EXISTS (SELECT 1 FROM fact ...)`, `pk IN
 * (SELECT fk FROM fact ...)`) qualifies with its outer side the dimension,
 * whose rows the join returns, and its inner side the fact, whose posting
 * sets say whether a dimension row has a match.  Its dimension key need not be
 * unique: each dimension row is tested on its own.
 *
 * A semi join qualifies the other way round too, as JOIN_UNIQUE_INNER: the
 * FORWARD semi join `SELECT count(*) FROM fact f WHERE EXISTS (SELECT 1 FROM
 * dim d WHERE d.k = f.fk ...)` over a key that nothing proves unique, whose
 * rows are the fact's.  Its dimension - the inner side - is made unique first,
 * the keys sorted by uniqsortop and each kept once, and each distinct key is
 * then counted as an inner join counts a dimension row: distinct keys have
 * disjoint posting sets, so the counts add up to the fact rows that have a
 * match, each once.
 *
 * The dimension is `dimrel` alone, or - when the query joins more tables -
 * the join rel `dimchild` of dimrel and the tables semi or anti joined to it
 * (DESIGN.md §27, "A dimension that is a join"): its rows are dimrel's rows,
 * each at most once, so everything this struct says about dimrel's key holds
 * of the join's rows.  dimrel is what the dimension's columns and its key's
 * uniqueness are asked of, dimchild what the child path is of.  A forward
 * semi join over such a dimension whose key an index proves unique is an
 * inner join, as core makes of one over a single table
 * (reduce_unique_semijoins()), which cannot see through a join.
 */
typedef struct LionFkJoin
{
	RelOptInfo *factrel;		/* the side whose posting sets are counted */
	RelOptInfo *dimrel;			/* the dimension's table, whose column is the
								 * key */
	RelOptInfo *dimchild;		/* the rel the child plan is a path of: dimrel,
								 * or the join rel of the dimension */
	Var		   *fkvar;			/* the fact's join column */
	Var		   *pkvar;			/* the dimension's join column */
	Node	   *pkexpr;			/* ... as the clause compares it: pkvar, or a
								 * binary-coercion relabel of it, whose type is
								 * the type the lookup is made with */
	Oid			opno;			/* the join clause's operator */
	Oid			collation;		/* and its input collation, or InvalidOid */
	Node	   *clause;			/* the join clause, for selectivity */
	Path	   *dimpath;		/* dimchild's cheapest total path */
	JoinType	jointype;		/* JOIN_INNER; or JOIN_SEMI or JOIN_ANTI, the
								 * dimension the outer side and the fact the
								 * inner one; or JOIN_UNIQUE_INNER, a semi
								 * join the other way round */
	RelOptInfo *joinrel;		/* the join rel: a semi or anti join's rows */
	Oid			uniqsortop;		/* JOIN_UNIQUE_INNER: the `<` of the join
								 * operator's btree family for the dimension
								 * key's type, which the keys are sorted by
								 * to make them distinct; else InvalidOid */
} LionFkJoin;

/*
 * How many ways of taking joinrel apart qualify, one per table that may be the
 * fact - each way round of a join of two tables, and every fact of a joined
 * dimension - in a palloc'd array set into *out.  Everything returned has
 * passed every check that is about the JOIN and the DIMENSION; whether the
 * fact side has a lion index that can answer the join operator, and whether
 * its own quals can be pushed down, is for the caller to decide.
 */
extern int	lion_fkjoin_recognize(PlannerInfo *root, RelOptInfo *joinrel,
								  LionFkJoin **out);

/*
 * Whether rel's column attno is provably unique under the equality opno
 * compares with, under that collation: the dimension-key proof of the join,
 * which the count pushdown reuses for count(DISTINCT) of a unique column
 * (DESIGN.md §26, "A unique column").
 */
extern bool lion_column_is_unique(RelOptInfo *rel, AttrNumber attno, Oid opno,
								  Oid collation);

#endif							/* LION_FKJOIN_H */
