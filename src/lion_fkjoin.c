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
 *		The semi and anti joins of `EXISTS`, `IN` and `NOT EXISTS` are
 *		recognised here too: the dimension is the outer side, whose rows the
 *		join returns, and the fact the inner side, whose posting sets say
 *		whether a dimension row has a match - DESIGN.md §27, "Semi and anti
 *		joins".  They need no uniqueness at all.
 *
 *		A semi join is recognised the other way round as well: the forward
 *		`SELECT count(*) FROM fact f WHERE EXISTS (SELECT 1 FROM dim d WHERE
 *		d.k = f.fk ...)`, whose rows are the fact's, over a dimension key
 *		nothing proves unique (a unique one has made it an inner join by now).
 *		It is JOIN_UNIQUE_INNER, as core calls the same plan: the dimension's
 *		keys are made distinct, and each is counted as an inner join counts a
 *		dimension row - DESIGN.md §27, "Forward semi joins over a non-unique
 *		key".  What this file asks of it is an order to make the keys distinct
 *		by: a btree family that has the join operator as its equality.
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
 * Is a column of rel provably unique among its rows - at most one row per
 * value, under the equality `opno` compares with?  The FK-side join asks it of
 * the dimension's join column, and the count pushdown of the dimension-free
 * shapes asks it of the column of a count(DISTINCT col), which is then the
 * count(col) of the same rows (DESIGN.md §26, "A unique column").
 *
 * A single-column btree index that is UNIQUE, enforced immediately (a
 * deferrable constraint may hold duplicates inside a transaction), not partial
 * and not hypothetical, on exactly that column, whose opfamily has the
 * operator as its equality strategy - so that "unique" is about the equality
 * the caller compares with, cross-type members included - and whose collation
 * is the caller's.  The last one is the rule core's
 * relation_has_unique_index_for() only acquired in PostgreSQL 19 (before that
 * it carries an XXX): a key that is unique under "C" is not unique under a
 * case-insensitive collation, where 'a' and 'A' are one value.  NULLs do not
 * count against uniqueness here (a unique index lets any number of them in
 * unless it says NULLS NOT DISTINCT); both callers ignore NULL keys anyway.
 *
 * Only rel->indexlist is consulted, which get_relation_info() fills with the
 * VALID indexes a query may rely on - one that CREATE INDEX CONCURRENTLY has
 * not finished, or that is not yet safe to use under this transaction's
 * snapshot, is left out of it.  For a partitioned parent it holds the
 * PARTITIONED indexes (PostgreSQL 16 and later), and a unique one of those is
 * a global proof: such an index must contain every partitioning column, so a
 * single-column one is on the partition key itself, which keeps equal values
 * in one partition, whose own unique index then holds them apart - the proof
 * core's own join removal takes from the same list.  A unique index on a
 * partition alone proves nothing about the parent.
 *
 * Nothing records that a plan relied on the index: dropping it (or the
 * constraint behind it) sends a relcache invalidation for its table, which
 * invalidates every cached plan that reads the table, exactly as for core's
 * own uses of the proof.
 *
 * A multi-column unique index whose other columns the query pins to constants
 * would prove it too; v1 does not look for one.
 */
bool
lion_column_is_unique(RelOptInfo *rel, AttrNumber attno, Oid opno,
					  Oid collation)
{
	ListCell   *lc;

	foreach(lc, rel->indexlist)
	{
		IndexOptInfo *ind = (IndexOptInfo *) lfirst(lc);

		if (!ind->unique || !ind->immediate || ind->hypothetical)
			continue;
		if (ind->indpred != NIL || ind->relam != BTREE_AM_OID)
			continue;
		if (ind->nkeycolumns != 1 || ind->indexkeys[0] != attno)
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
	JoinType	jointype = JOIN_INNER;
	int			semifact = 0;
	int			n = 0;
	int			i;
	int			k;
	ListCell   *lc;

	/* ---- two plain tables, joined, nothing else in the query ---- */
	if (joinrel->reloptkind != RELOPT_JOINREL)
		return 0;
	if (bms_num_members(joinrel->relids) != 2)
		return 0;
	if (IS_DUMMY_REL(joinrel))
		return 0;

	/*
	 * A PlaceHolderVar is an expression evaluated at some join level, which
	 * the node has no level to evaluate at.  A pseudoconstant qual is gated at
	 * the join the node replaces, where it would be lost.
	 */
	if (root->placeholder_list != NIL)
		return 0;
	if (root->hasPseudoConstantQuals || root->hasLateralRTEs)
		return 0;

	/*
	 * An outer join makes a SpecialJoinInfo, and a count over it is not the
	 * inner join's (a LEFT JOIN keeps the fact rows with no dimension row).
	 * So does a semi or an anti join - EXISTS, IN, NOT EXISTS pulled up into
	 * the join tree - and that one IS a shape the node answers: its rows are
	 * the outer side's, each once, that have (or have not) a match on the
	 * inner side, so with the outer side as the dimension and the inner one
	 * as the fact it is the same lookup per dimension row with an existence
	 * test in place of the count.  An EXISTS whose inner side is provably
	 * unique on the join key never gets here as one: the planner has made it
	 * an inner join already (reduce_unique_semijoins()), which is the
	 * equivalence the forward direction - fact rows whose dimension row
	 * qualifies - rests on, and the one a non-unique key must not get.  Over
	 * a non-unique key the forward direction does get here, as a semi join
	 * whose inner side is the dimension, and is counted over the dimension's
	 * DISTINCT keys (JOIN_UNIQUE_INNER, below).
	 *
	 * Only the join between these two tables, then, and one whose relation
	 * set is exactly them: an anti join made from `LEFT JOIN ... WHERE
	 * f.fk IS NULL` has a range table entry of its own (ojrelid), which the
	 * NOT EXISTS form does not, and is left alone.
	 */
	if (root->join_info_list != NIL)
	{
		SpecialJoinInfo *sjinfo;

		if (list_length(root->join_info_list) != 1)
			return 0;
		sjinfo = (SpecialJoinInfo *) linitial(root->join_info_list);
		if (sjinfo->jointype != JOIN_SEMI && sjinfo->jointype != JOIN_ANTI)
			return 0;
		if (sjinfo->ojrelid != 0)
			return 0;
		if (bms_membership(sjinfo->min_lefthand) != BMS_SINGLETON ||
			!bms_get_singleton_member(sjinfo->min_righthand, &semifact) ||
			!bms_equal(sjinfo->syn_lefthand, sjinfo->min_lefthand) ||
			!bms_equal(sjinfo->syn_righthand, sjinfo->min_righthand) ||
			bms_overlap(sjinfo->min_lefthand, sjinfo->min_righthand) ||
			!bms_is_subset(sjinfo->min_lefthand, joinrel->relids) ||
			!bms_is_member(semifact, joinrel->relids))
			return 0;
		jointype = sjinfo->jointype;
	}

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
	 * unique is this file's.  An anti join goes one way only, the fact its
	 * inner side, and asks nothing about uniqueness: its count is one per
	 * dimension row, however many dimension rows share a key.  A semi join
	 * goes that way too, and the other way as JOIN_UNIQUE_INNER, the forward
	 * semi join, whose dimension's keys are made distinct instead of proved
	 * so.
	 */
	for (k = 0; k < 2; k++)
	{
		RelOptInfo *fact = rels[k];
		RelOptInfo *dim = rels[1 - k];
		int			fi = (argvar[0]->varno == (int) fact->relid) ? 0 : 1;
		int			di = 1 - fi;
		LionFkJoin *fj = &out[n];
		JoinType	thisjoin = jointype;
		Oid			sortop = InvalidOid;

		if (argvar[fi]->varno != (int) fact->relid ||
			argvar[di]->varno != (int) dim->relid)
			return 0;

		if (jointype == JOIN_SEMI && (int) fact->relid != semifact)
		{
			/*
			 * The forward semi join: the fact is the outer side and the
			 * dimension the inner one, whose keys the node sorts and keeps
			 * once each.  The sort is by the `<` of a btree family whose
			 * equality is the join operator, for the dimension key's type -
			 * the order core's own unique-ification of a semi join's inner
			 * side sorts by (create_unique_path()) - so that keys equal
			 * under the join's operator, and only those, come out next to
			 * each other.  An operator no btree family knows has no such
			 * order, and the shape is left to the ordinary plan.
			 */
			sortop = get_ordering_op_for_equality_op(op->opno, di == 0);
			if (!OidIsValid(sortop))
				continue;
			thisjoin = JOIN_UNIQUE_INNER;
		}
		else if (jointype != JOIN_INNER)
		{
			if ((int) fact->relid != semifact)
				continue;
		}
		else if (!lion_column_is_unique(dim, argvar[di]->varattno, op->opno,
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
		fj->jointype = thisjoin;
		fj->joinrel = joinrel;
		fj->uniqsortop = sortop;
		n++;
	}

	return n;
}
