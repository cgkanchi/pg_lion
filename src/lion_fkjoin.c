/*-------------------------------------------------------------------------
 *
 * lion_fkjoin.c
 *		Recognising the FK-side join of DESIGN.md §27:
 *
 *			SELECT d.attr, count(*)
 *			FROM fact f JOIN dim d ON f.fk = d.pk
 *			[WHERE <pushdown clauses on f>] [AND <anything on d>]
 *			GROUP BY d.attr
 *
 *		The LionCount node answers it by running the dimension side as a
 *		child plan and counting, per dimension row, the fk posting set of that
 *		row's key ANDed with the fact filters (lion_customscan.c).  This file
 *		decides only what is about the JOIN: that there is exactly one join
 *		clause, an equality between a plain column of each table, and which
 *		orientations of it have a dimension column that is provably unique.
 *
 *		Why the count is exact is DESIGN.md §27's argument: the join's count
 *		for a group is the sum over the group's dimension rows of each row's
 *		matching fact rows, so it is additive over dimension rows whatever the
 *		key holds.  Uniqueness is required anyway, as a scope decision - it is
 *		what bounds the node's work by the dimension's key count, which the
 *		cost model assumes - and it is proved here rather than taken from
 *		core's relation_has_unique_index_for(), which before PostgreSQL 19
 *		does not compare collations.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/stratnum.h"
#include "catalog/pg_am.h"
#include "catalog/pg_class.h"
#include "nodes/nodeFuncs.h"
#include "nodes/pathnodes.h"
#include "optimizer/pathnode.h"
#include "optimizer/paths.h"
#include "parser/parsetree.h"
#include "utils/lsyscache.h"

#include "lion_fkjoin.h"

/*
 * Peel binary-coercion relabels off an expression, as lion_strip() in
 * lion_customscan.c does: a varchar column compared with a text one arrives
 * as RelabelType(Var).
 */
static Node *
lion_fkjoin_strip(Node *node)
{
	while (node != NULL && IsA(node, RelabelType))
		node = (Node *) ((RelabelType *) node)->arg;
	return node;
}

/*
 * Is one side of the join an ordinary table this node can take apart?  A plain
 * table or a materialized view, not an inheritance or partitioned parent (the
 * fact side of §16 is not in v1, and a parent has no index list to prove a
 * dimension key unique from), with no LATERAL references, and not proved
 * empty.
 */
static bool
lion_fkjoin_rel_ok(PlannerInfo *root, RelOptInfo *rel)
{
	RangeTblEntry *rte;

	if (rel == NULL || rel->reloptkind != RELOPT_BASEREL)
		return false;
	if (rel->relid == 0 || rel->relid >= (Index) root->simple_rel_array_size)
		return false;
	rte = root->simple_rte_array[rel->relid];
	if (rte == NULL || rte->rtekind != RTE_RELATION || rte->inh)
		return false;
	if (rte->relkind != RELKIND_RELATION && rte->relkind != RELKIND_MATVIEW)
		return false;
	if (rte->tablesample != NULL)
		return false;
	if (!bms_is_empty(rel->lateral_relids) || rel->lateral_vars != NIL)
		return false;
	if (IS_DUMMY_REL(rel))
		return false;
	return true;
}

/*
 * Is the dimension's join column provably unique among the rows the child plan
 * can return - one dimension row per key value, under the join's equality?
 *
 * A single-column btree index that is UNIQUE, enforced immediately (a
 * deferrable constraint may hold duplicates inside a transaction), not partial
 * and not hypothetical, on exactly that column, whose opfamily has the join
 * operator as its equality strategy - so that "unique" is about the equality
 * the join compares with, cross-type members included - and whose collation
 * is the join clause's input collation.  The last one is the rule core's
 * relation_has_unique_index_for() only acquired in PostgreSQL 19 (before that
 * it carries an XXX): a key that is unique under "C" is not unique under a
 * case-insensitive collation, where 'a' and 'A' are one value.
 *
 * A multi-column unique index whose other columns the query pins to constants
 * would prove it too; v1 does not look for one.
 */
static bool
lion_fkjoin_dim_unique(RelOptInfo *dimrel, Var *pkvar, Oid opno,
					   Oid collation)
{
	ListCell   *lc;

	foreach(lc, dimrel->indexlist)
	{
		IndexOptInfo *ind = (IndexOptInfo *) lfirst(lc);

		if (!ind->unique || !ind->immediate || ind->hypothetical)
			continue;
		if (ind->indpred != NIL || ind->relam != BTREE_AM_OID)
			continue;
		if (ind->nkeycolumns != 1 || ind->indexkeys[0] != pkvar->varattno)
			continue;
		if (get_op_opfamily_strategy(opno, ind->opfamily[0]) !=
			BTEqualStrategyNumber)
			continue;
		if (OidIsValid(collation) && ind->indexcollations[0] != collation)
			continue;
		return true;
	}

	return false;
}

int
lion_fkjoin_recognize(PlannerInfo *root, RelOptInfo *joinrel, LionFkJoin *out)
{
	RelOptInfo *rels[2];
	List	   *clauses;
	RestrictInfo *rinfo;
	OpExpr	   *op;
	Node	   *argexpr[2];
	Var		   *argvar[2];
	int			n = 0;
	int			i;
	int			k;
	ListCell   *lc;

	/* ---- two plain tables, inner-joined, nothing else in the query ---- */
	if (joinrel->reloptkind != RELOPT_JOINREL)
		return 0;
	if (bms_num_members(joinrel->relids) != 2)
		return 0;
	if (IS_DUMMY_REL(joinrel))
		return 0;

	/*
	 * An outer, semi or anti join anywhere makes a SpecialJoinInfo, and a
	 * count over it is not the inner join's (a LEFT JOIN keeps the fact rows
	 * with no dimension row).  A PlaceHolderVar is an expression evaluated at
	 * some join level, which the node has no level to evaluate at.  A
	 * pseudoconstant qual is gated at the join the node replaces, where it
	 * would be lost.
	 */
	if (root->join_info_list != NIL || root->placeholder_list != NIL)
		return 0;
	if (root->hasPseudoConstantQuals || root->hasLateralRTEs)
		return 0;

	i = -1;
	k = 0;
	while ((i = bms_next_member(joinrel->relids, i)) >= 0)
	{
		if (k >= 2)
			return 0;
		rels[k] = find_base_rel(root, i);
		if (!lion_fkjoin_rel_ok(root, rels[k]))
			return 0;
		k++;
	}
	if (k != 2)
		return 0;

	/*
	 * ---- exactly one join clause ----
	 *
	 * A mergejoinable equality between the two tables lives in an equivalence
	 * class, not in joininfo, and is generated here the way the join's own
	 * restrict list is built (build_joinrel_restrictlist()).  Anything else
	 * that mentions both tables is in each one's joininfo.  Together they must
	 * be ONE clause: a composite key, a second condition (`f.a < d.b`) or an
	 * OR across the tables is not a lookup of one key.
	 */
	clauses = generate_join_implied_equalities(root, joinrel->relids,
											   rels[0]->relids, rels[1],
											   NULL);
	foreach(lc, rels[0]->joininfo)
	{
		RestrictInfo *ri = (RestrictInfo *) lfirst(lc);

		if (bms_is_subset(ri->required_relids, joinrel->relids))
			clauses = lappend(clauses, ri);
	}
	if (list_length(clauses) != 1)
		return 0;

	rinfo = (RestrictInfo *) linitial(clauses);
	if (!IsA(rinfo, RestrictInfo) || rinfo->pseudoconstant)
		return 0;
	if (!IsA(rinfo->clause, OpExpr))
		return 0;
	op = (OpExpr *) rinfo->clause;
	if (list_length(op->args) != 2 || !op_strict(op->opno))
		return 0;

	for (i = 0; i < 2; i++)
	{
		Node	   *arg = (Node *) list_nth(op->args, i);
		Node	   *stripped = lion_fkjoin_strip(arg);

		if (stripped == NULL || !IsA(stripped, Var))
			return 0;
		argexpr[i] = arg;
		argvar[i] = (Var *) stripped;
		if (argvar[i]->varattno <= 0 || argvar[i]->varlevelsup != 0)
			return 0;
	}
	if (argvar[0]->varno == argvar[1]->varno)
		return 0;

	/*
	 * ---- each way round: one side is the fact, the other the dimension ----
	 *
	 * Equality commutes, so either operand may be either table's.  Which of
	 * the two has a lion index that answers the operator is the caller's
	 * question (lion_collect_targets(), per clause); which one's column is
	 * unique is this file's.
	 */
	for (k = 0; k < 2; k++)
	{
		RelOptInfo *fact = rels[k];
		RelOptInfo *dim = rels[1 - k];
		int			fi = (argvar[0]->varno == (int) fact->relid) ? 0 : 1;
		int			di = 1 - fi;
		LionFkJoin *fj = &out[n];

		if (argvar[fi]->varno != (int) fact->relid ||
			argvar[di]->varno != (int) dim->relid)
			return 0;

		if (!lion_fkjoin_dim_unique(dim, argvar[di], op->opno,
									op->inputcollid))
			continue;
		if (dim->cheapest_total_path == NULL ||
			dim->cheapest_total_path->param_info != NULL)
			continue;

		fj->factrel = fact;
		fj->dimrel = dim;
		fj->fkvar = argvar[fi];
		fj->pkvar = argvar[di];
		fj->pkexpr = argexpr[di];
		fj->opno = op->opno;
		fj->collation = op->inputcollid;
		fj->clause = (Node *) op;
		fj->dimpath = dim->cheapest_total_path;
		n++;
	}

	return n;
}
