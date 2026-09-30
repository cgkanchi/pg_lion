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
 *		row's key ANDed with the fact filters (lion_exec_fkjoin.c).  This file
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
 *		The dimension may be a join itself: one table that carries the key,
 *		and other tables joined to it only as the inner side of semi and anti
 *		joins, whose rows are that table's rows, each at most once - DESIGN.md
 *		§27, "A dimension that is a join".  The node's child is then core's
 *		cheapest path of that join rel, and each table of the query that
 *		qualifies as the fact is offered as one.  What this file checks for it
 *		is in the SpecialJoinInfos: which of them joins the fact, that none of
 *		the others involves the fact, and that exactly one table of the
 *		dimension is outside every inner side.
 *
 *		The semi and anti joins are also offered as JOIN paths, whose rows
 *		are the outer side's - DESIGN.md §27, "The semi and anti join as a
 *		join path".  lion_fkjoin_recognize_join() decides, for one call of
 *		set_join_pathlist_hook, whether the join it is asked about is one: a
 *		plain semi or anti join, the fact alone its inner side, one fk
 *		equality its only clause, whatever the outer side is.
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
 * lion_plan_match.c does: a varchar column compared with a text one arrives
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
 * Is one side of the join a table this node can take apart?  A plain table or
 * a materialized view, or - for the fact side only (lion_fkjoin_is_parent()) -
 * a partitioned table, whose live leaf partitions the node counts one by one
 * (DESIGN.md §27, "A partitioned fact table"); never an old-style inheritance
 * parent, whose children need not share its columns.  With no LATERAL
 * references, and not proved empty.
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
	if (rte == NULL || rte->rtekind != RTE_RELATION)
		return false;
	if (rte->inh)
	{
		if (rte->relkind != RELKIND_PARTITIONED_TABLE ||
			!IS_PARTITIONED_REL(rel))
			return false;
	}
	else if (rte->relkind != RELKIND_RELATION &&
			 rte->relkind != RELKIND_MATVIEW)
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
 * Is rel a partitioned table?  It may be the FACT side of the join, never the
 * dimension: a partitioned or inheritance dimension has no index list of its
 * own to prove its key unique from, and the node runs the dimension as one
 * child plan whatever it is.
 */
static bool
lion_fkjoin_is_parent(PlannerInfo *root, RelOptInfo *rel)
{
	return root->simple_rte_array[rel->relid]->inh;
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

/*
 * Is `relid` a relation of the join the FK-side join may take apart: a base
 * relation with no LATERAL references and no TABLESAMPLE (whose sample need
 * not be the same in every participant of a parallel plan that runs the
 * dimension whole)?  The fact and the dimension's own table are asked more
 * (lion_fkjoin_rel_ok()); the other tables of a joined dimension are core's to
 * plan, of whatever kind.
 */
static bool
lion_fkjoin_member_ok(PlannerInfo *root, int relid)
{
	RelOptInfo *r;
	RangeTblEntry *rte;

	if (relid <= 0 || relid >= root->simple_rel_array_size)
		return false;
	r = root->simple_rel_array[relid];
	rte = root->simple_rte_array[relid];
	if (r == NULL || rte == NULL || r->reloptkind != RELOPT_BASEREL)
		return false;
	if (!bms_is_empty(r->lateral_relids) || r->lateral_vars != NIL)
		return false;
	if (rte->rtekind == RTE_RELATION && rte->tablesample != NULL)
		return false;
	return true;
}

/*
 * The clauses that join `a` and `b` - two base relations - with nothing else:
 * what an equivalence class gives for the pair, generated the way a join's
 * own restrict list is built (build_joinrel_restrictlist()), and a's joininfo
 * clauses that need nothing but the two.  `a` is the one with the lower
 * relid, as the pair has always been taken: which operand of a derived clause
 * comes first decides its operator between two types (`int48eq` or
 * `int84eq`).
 */
static List *
lion_fkjoin_pair_clauses(PlannerInfo *root, RelOptInfo *a, RelOptInfo *b)
{
	Relids		pair = bms_union(a->relids, b->relids);
	List	   *clauses;
	ListCell   *lc;

	clauses = generate_join_implied_equalities(root, pair, a->relids, b, NULL);
	foreach(lc, a->joininfo)
	{
		RestrictInfo *ri = (RestrictInfo *) lfirst(lc);

		if (bms_is_subset(ri->required_relids, pair))
			clauses = lappend(clauses, ri);
	}
	return clauses;
}

/* Does the target carry the column `var` is of, as a plain Var? */
static bool
lion_fkjoin_target_has(PathTarget *target, Var *var)
{
	ListCell   *lc;

	foreach(lc, target->exprs)
	{
		Var		   *v = (Var *) lfirst(lc);

		if (IsA(v, Var) && v->varno == var->varno &&
			v->varattno == var->varattno && v->varlevelsup == 0)
			return true;
	}
	return false;
}

/*
 * One way of taking `joinrel` apart: the relation `factrelid` as the fact,
 * everything else as the dimension (DESIGN.md §27, "A dimension that is a
 * join").  Fills *fj and returns true when it qualifies.
 *
 * The join between the fact and the dimension is one of three, told apart by
 * the special joins - the SpecialJoinInfos core made of the query's semi and
 * anti joins, the only kind lion_fkjoin_recognize() lets through:
 *
 *	- the fact is the whole inner side of a semi or anti join, its minimal
 *	  and its syntactic right-hand side the fact alone: the reverse semi join
 *	  or the anti join of "Semi and anti joins", the dimension its outer side;
 *	- the fact is the whole outer side of a semi join whose inner side is the
 *	  whole dimension: the forward semi join;
 *	- neither: an inner join.
 *
 * Every other special join is the dimension's own, and must not involve the
 * fact: not on its inner side, and not among the relations its condition
 * needs on its outer side (min_lefthand), which would make it a condition on
 * the fact's rows that no posting set answers.  The fact may be in its
 * syntactic outer side, which only says where the EXISTS was written: core
 * computes the semi join over its minimal outer side, and so may the child.
 *
 * The dimension's rows are the rows of ONE of its tables, each at most once,
 * when every other table of it is inside the inner side of one of those semi
 * or anti joins: a semi or anti join returns rows of its outer side, each at
 * most once, and the tables inside its inner side add none to them, nested
 * semi joins and inner joins in there included.  So the dimension's own table
 * is the one table of it that no special join has on its inner side, and
 * there has to be exactly one: two would be an inner join inside the
 * dimension, which may repeat a row.
 */
static bool
lion_fkjoin_try_fact(PlannerInfo *root, RelOptInfo *joinrel, int factrelid,
					 LionFkJoin *fj)
{
	RelOptInfo *fact = root->simple_rel_array[factrelid];
	RelOptInfo *dim;
	RelOptInfo *dimchild;
	Relids		dimrels;
	Relids		hidden = NULL;
	SpecialJoinInfo *key = NULL;
	bool		forward = false;
	bool		joined;
	int			dimrelid;
	List	   *clauses;
	RestrictInfo *rinfo;
	OpExpr	   *op;
	Node	   *argexpr[2];
	Var		   *argvar[2];
	JoinType	jointype;
	Oid			sortop = InvalidOid;
	int			fi;
	int			di;
	int			i;
	ListCell   *lc;

	if (!lion_fkjoin_rel_ok(root, fact))
		return false;
	dimrels = bms_del_member(bms_copy(joinrel->relids), factrelid);
	joined = (bms_membership(dimrels) == BMS_MULTIPLE);

	foreach(lc, root->join_info_list)
	{
		SpecialJoinInfo *sjinfo = (SpecialJoinInfo *) lfirst(lc);

		if (bms_equal(sjinfo->min_righthand, fact->relids) &&
			bms_equal(sjinfo->syn_righthand, fact->relids))
		{
			/* the fact alone inside an EXISTS, an IN or a NOT EXISTS */
			if (key != NULL)
				return false;
			key = sjinfo;
			continue;
		}
		if (sjinfo->jointype == JOIN_SEMI &&
			bms_equal(sjinfo->min_lefthand, fact->relids) &&
			bms_equal(sjinfo->syn_lefthand, fact->relids) &&
			bms_equal(sjinfo->syn_righthand, dimrels))
		{
			/* the whole dimension inside the fact's EXISTS or IN */
			if (key != NULL)
				return false;
			key = sjinfo;
			forward = true;
			continue;
		}

		/* one of the dimension's own */
		if (bms_is_member(factrelid, sjinfo->min_lefthand) ||
			bms_is_member(factrelid, sjinfo->min_righthand) ||
			bms_is_member(factrelid, sjinfo->syn_righthand))
			return false;
		hidden = bms_add_members(hidden, sjinfo->syn_righthand);
	}

	/* ---- the dimension's own table: the one no semi or anti join hides ---- */
	if (!bms_get_singleton_member(bms_difference(dimrels, hidden), &dimrelid))
		return false;
	dim = root->simple_rel_array[dimrelid];
	if (!lion_fkjoin_rel_ok(root, dim) || lion_fkjoin_is_parent(root, dim))
		return false;

	/*
	 * ---- exactly one join clause ----
	 *
	 * A mergejoinable equality between the fact and the dimension lives in an
	 * equivalence class, not in joininfo, and is generated here the way the
	 * join's own restrict list is built; anything else that mentions both is
	 * in the fact's joininfo.  Together they must be ONE clause: a composite
	 * key, a second condition (`f.a < d.b`) or an OR across the tables is not
	 * a lookup of one key.
	 *
	 * Over a joined dimension that is asked of the dimension as a whole, and
	 * the clause is then taken between the fact and the dimension's own table,
	 * which carries the key.  The dimension may hold other members of the
	 * key's class - `f1.fk` of a semi-joined table, when `f1.fk = d.pk` and
	 * `f2.fk = d.pk` went into one class - and the class's clause for the
	 * dimension as a whole may name that column where the key is `d.pk`, the
	 * same rows by the class's transitivity.  The pair's clause has to be of
	 * that same class, or be the same joininfo clause.
	 */
	clauses = (dimrelid < factrelid) ?
		lion_fkjoin_pair_clauses(root, dim, fact) :
		lion_fkjoin_pair_clauses(root, fact, dim);
	if (list_length(clauses) != 1)
		return false;
	rinfo = (RestrictInfo *) linitial(clauses);
	if (!IsA(rinfo, RestrictInfo))
		return false;
	if (joined)
	{
		List	   *whole;
		RestrictInfo *wri;

		whole = generate_join_implied_equalities(root, joinrel->relids,
												 dimrels, fact, NULL);
		foreach(lc, fact->joininfo)
		{
			RestrictInfo *ri = (RestrictInfo *) lfirst(lc);

			if (bms_is_subset(ri->required_relids, joinrel->relids))
				whole = lappend(whole, ri);
		}
		if (list_length(whole) != 1)
			return false;
		wri = (RestrictInfo *) linitial(whole);
		if (!IsA(wri, RestrictInfo))
			return false;
		if (wri->parent_ec != NULL ? rinfo->parent_ec != wri->parent_ec :
			rinfo != wri)
			return false;
	}

	if (rinfo->pseudoconstant)
		return false;
	if (!IsA(rinfo->clause, OpExpr))
		return false;
	op = (OpExpr *) rinfo->clause;
	if (list_length(op->args) != 2 || !op_strict(op->opno))
		return false;

	for (i = 0; i < 2; i++)
	{
		Node	   *arg = (Node *) list_nth(op->args, i);
		Node	   *stripped = lion_fkjoin_strip(arg);

		if (stripped == NULL || !IsA(stripped, Var))
			return false;
		argexpr[i] = arg;
		argvar[i] = (Var *) stripped;
		if (argvar[i]->varattno <= 0 || argvar[i]->varlevelsup != 0)
			return false;
	}
	fi = (argvar[0]->varno == factrelid) ? 0 : 1;
	di = 1 - fi;
	if (argvar[fi]->varno != factrelid || argvar[di]->varno != dimrelid)
		return false;

	/*
	 * The semi or anti join with the fact has to be made on the dimension's
	 * own table, which its condition needs: an equivalence class can give a
	 * clause between the fact and that table where the query's condition was
	 * on another member of the class - an EXISTS on the fact nested inside an
	 * EXISTS on another table, whose key it names - and the join is then that
	 * other table's, inside the dimension.
	 */
	if (key != NULL &&
		!bms_is_member(dimrelid, forward ? key->min_righthand :
					   key->min_lefthand))
		return false;

	/*
	 * ---- the kind of join, and what it asks of the key ----
	 *
	 * An anti join goes one way only, the fact its inner side, and asks
	 * nothing about uniqueness: its count is one per dimension row, however
	 * many dimension rows share a key.  Nor does the reverse semi join.  The
	 * forward semi join - the fact its outer side - is JOIN_UNIQUE_INNER, whose
	 * dimension's keys are made distinct instead of proved so; or, over a
	 * joined dimension whose key an index proves unique, the inner join core
	 * makes of it over a single table and cannot over a join.  An inner join
	 * needs the proof.  A joined dimension's rows are its own table's, each at
	 * most once (above), so a key unique in the table is unique in them.
	 */
	if (key == NULL)
	{
		if (!lion_column_is_unique(dim, argvar[di]->varattno, op->opno,
								   op->inputcollid))
			return false;
		jointype = JOIN_INNER;
	}
	else if (forward)
	{
		if (joined &&
			lion_column_is_unique(dim, argvar[di]->varattno, op->opno,
								  op->inputcollid))
			jointype = JOIN_INNER;
		else
		{
			/*
			 * The fact is the outer side and the dimension the inner one,
			 * whose keys the node sorts and keeps once each.  The sort is by
			 * the `<` of a btree family whose equality is the join operator,
			 * for the dimension key's type - the order core's own
			 * unique-ification of a semi join's inner side sorts by
			 * (create_unique_path()) - so that keys equal under the join's
			 * operator, and only those, come out next to each other.  An
			 * operator no btree family knows has no such order, and the
			 * shape is left to the ordinary plan.
			 */
			sortop = get_ordering_op_for_equality_op(op->opno, di == 0);
			if (!OidIsValid(sortop))
				return false;
			jointype = JOIN_UNIQUE_INNER;
		}
	}
	else
		jointype = key->jointype;

	/*
	 * ---- the child: the dimension's cheapest path ----
	 *
	 * Of its table, or of the join rel of all of its tables, which the join
	 * search made if that order is legal - with every semi and anti join of
	 * the dimension on its own table's side, it is, but nothing here assumes
	 * it.  Its rows have to carry the key, which a dimension joined to the
	 * fact by it does: the join above the dimension needs it.
	 */
	if (joined)
	{
		dimchild = find_join_rel(root, dimrels);
		if (dimchild == NULL || IS_DUMMY_REL(dimchild))
			return false;
	}
	else
		dimchild = dim;
	if (!lion_fkjoin_target_has(dimchild->reltarget, argvar[di]))
		return false;
	if (dimchild->cheapest_total_path == NULL ||
		dimchild->cheapest_total_path->param_info != NULL)
		return false;

	fj->factrel = fact;
	fj->dimrel = dim;
	fj->dimchild = dimchild;
	fj->fkvar = argvar[fi];
	fj->pkvar = argvar[di];
	fj->pkexpr = argexpr[di];
	fj->opno = op->opno;
	fj->collation = op->inputcollid;
	fj->clause = (Node *) op;
	fj->dimpath = dimchild->cheapest_total_path;
	fj->jointype = jointype;
	fj->joinrel = joinrel;
	fj->uniqsortop = sortop;
	return true;
}

int
lion_fkjoin_recognize(PlannerInfo *root, RelOptInfo *joinrel, LionFkJoin **out)
{
	LionFkJoin *fj;
	int			n = 0;
	int			i;
	ListCell   *lc;

	*out = NULL;
	if (joinrel->reloptkind != RELOPT_JOINREL)
		return 0;
	if (IS_DUMMY_REL(joinrel))
		return 0;

	/*
	 * A PlaceHolderVar is an expression evaluated at some join level, which
	 * the node has no level to evaluate at.  A pseudoconstant qual is gated at
	 * the join the node replaces, where it would be lost.  A LATERAL reference
	 * would make the dimension's paths depend on rows the node never gives
	 * them.
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
	 * test in place of the count.  An EXISTS whose inner side is a table
	 * provably unique on the join key never gets here as one: the planner has
	 * made it an inner join already (reduce_unique_semijoins()), which is the
	 * equivalence the forward direction - fact rows whose dimension row
	 * qualifies - rests on, and the one a non-unique key must not get.  Over
	 * a non-unique key the forward direction does get here, as a semi join
	 * whose inner side is the dimension, and is counted over the dimension's
	 * DISTINCT keys (JOIN_UNIQUE_INNER).  And a semi or anti join inside the
	 * dimension is what lets the dimension be a join (DESIGN.md §27, "A
	 * dimension that is a join"): it never repeats a row of its outer side.
	 *
	 * So semi and anti joins only, within the join: an anti join made from
	 * `LEFT JOIN ... WHERE f.fk IS NULL` has a range table entry of its own
	 * (ojrelid), which the NOT EXISTS form does not, and is left alone.
	 */
	foreach(lc, root->join_info_list)
	{
		SpecialJoinInfo *sjinfo = (SpecialJoinInfo *) lfirst(lc);

		if (sjinfo->jointype != JOIN_SEMI && sjinfo->jointype != JOIN_ANTI)
			return 0;
		if (sjinfo->ojrelid != 0)
			return 0;
		if (!bms_is_subset(sjinfo->syn_lefthand, joinrel->relids) ||
			!bms_is_subset(sjinfo->syn_righthand, joinrel->relids) ||
			bms_overlap(sjinfo->min_lefthand, sjinfo->min_righthand))
			return 0;
	}

	/* ---- every table of the join one the node may take apart ---- */
	i = -1;
	while ((i = bms_next_member(joinrel->relids, i)) >= 0)
	{
		if (!lion_fkjoin_member_ok(root, i))
			return 0;
	}

	/*
	 * ---- each table that may be the fact, the rest the dimension ----
	 *
	 * Equality commutes, so either side of the key may be either table's, and
	 * a join of two tables is tried both ways round.  Over a joined dimension
	 * each table that could be the fact is tried: a dimension with an EXISTS
	 * on each of two facts is either fact counted against the dimension
	 * semi-joined to the other.  Which of them has a lion index that answers
	 * the operator is the caller's question (lion_collect_targets(), per
	 * clause), and the cost model chooses among what qualifies.
	 */
	fj = (LionFkJoin *) palloc0(sizeof(LionFkJoin) *
								bms_num_members(joinrel->relids));
	i = -1;
	while ((i = bms_next_member(joinrel->relids, i)) >= 0)
	{
		if (lion_fkjoin_try_fact(root, joinrel, i, &fj[n]))
			n++;
	}
	*out = fj;
	return n;
}

/*
 * The FK-side semi or anti join as a JOIN path (DESIGN.md §27, "The semi and
 * anti join as a join path"): the rows of `outer ⋉ fact` or `outer ▷ fact`
 * themselves, for whatever needs them above - another join, a sort, an
 * aggregate, the node itself as its joined dimension - where the upper node
 * answers only an aggregate over the whole query's join.
 *
 * set_join_pathlist_hook is called once for each way core takes joinrel
 * apart into two rels it may join, and for each kind of join it tries for
 * them.  For a semi join (EXISTS, IN) that is JOIN_SEMI with the special
 * join's left side outer; JOIN_RIGHT_SEMI the other way round (PostgreSQL 18
 * and later); and the right side made unique and inner-joined, which up to 18
 * is JOIN_UNIQUE_INNER and JOIN_UNIQUE_OUTER and from 19 a JOIN_INNER with a
 * rel of unique-ified paths.  For an anti join (NOT EXISTS) it is JOIN_ANTI,
 * and JOIN_RIGHT_ANTI the other way round (16 and later).  Only the plain
 * JOIN_SEMI and JOIN_ANTI are taken, with the fact as innerrel: their rows are
 * the outer side's, each at most once, which is what the node emits - each
 * outer row whose key the fact's posting sets say has a match, or has none.
 * So nothing is asked of the key's uniqueness: two outer rows with one key are
 * two rows, each tested and each emitted once.  The other forms have the fact
 * outer (whose rows the node never emits) or a unique-ified side (which only
 * repeats what JOIN_SEMI answers).
 *
 * What the special join and the join rel must be:
 *
 *	- the special join is this call's own (extra->sjinfo), a semi or anti join
 *	  with no range table entry of its own: an anti join made from `LEFT JOIN
 *	  ... WHERE f.fk IS NULL` has one (ojrelid), and is left alone as the
 *	  upper node leaves it;
 *	- its minimal and syntactic inner sides are the fact alone, and innerrel is
 *	  the fact: a plain or partitioned table the node may count
 *	  (lion_fkjoin_rel_ok()), with no LATERAL reference and no TABLESAMPLE;
 *	- the join's clauses (extra->restrictlist, everything the join has to
 *	  evaluate) are exactly ONE, a strict operator between a plain column of
 *	  the fact and a plain column of a table of the outer side - a second
 *	  correlation, an outer-side qual left inside a NOT EXISTS, a
 *	  pseudoconstant qual anywhere in the query all decline;
 *	- the outer side needs nothing from outside it (no LATERAL reference left
 *	  unsatisfied) and its rows carry the key: the node reads it from them.
 *
 * The outer side may be any rel: a base relation of any kind, or a join rel
 * of any shape, since only core plans and runs it.  What the join rel's
 * target may name, and the fact side's indexes, are the caller's to check.
 */
bool
lion_fkjoin_recognize_join(PlannerInfo *root, RelOptInfo *joinrel,
						   RelOptInfo *outerrel, RelOptInfo *innerrel,
						   JoinType jointype, JoinPathExtraData *extra,
						   LionFkJoin *fj)
{
	SpecialJoinInfo *sjinfo = extra->sjinfo;
	RestrictInfo *rinfo;
	OpExpr	   *op;
	Node	   *argexpr[2];
	Var		   *argvar[2];
	RelOptInfo *dim;
	int			fi;
	int			di;
	int			i;

	if (jointype != JOIN_SEMI && jointype != JOIN_ANTI)
		return false;
	if (sjinfo == NULL || sjinfo->jointype != jointype || sjinfo->ojrelid != 0)
		return false;
	if (joinrel->reloptkind != RELOPT_JOINREL || IS_DUMMY_REL(joinrel) ||
		IS_DUMMY_REL(outerrel))
		return false;

	/* ---- the fact alone on the inner side ---- */
	if (!bms_equal(sjinfo->min_righthand, innerrel->relids) ||
		!bms_equal(sjinfo->syn_righthand, innerrel->relids))
		return false;
	if (!lion_fkjoin_rel_ok(root, innerrel))
		return false;

	/*
	 * A pseudoconstant qual is gated where core puts it, which may be this
	 * join (PostgreSQL 16 does not even call the hook then); a LATERAL
	 * reference out of the outer side would make its paths depend on rows the
	 * node never gives them.
	 */
	if (root->hasPseudoConstantQuals)
		return false;
	if (!bms_is_empty(outerrel->lateral_relids) ||
		!bms_is_empty(joinrel->lateral_relids))
		return false;

	/* ---- exactly one join clause: the key ---- */
	if (list_length(extra->restrictlist) != 1)
		return false;
	rinfo = (RestrictInfo *) linitial(extra->restrictlist);
	if (!IsA(rinfo, RestrictInfo) || rinfo->pseudoconstant)
		return false;
	if (!IsA(rinfo->clause, OpExpr))
		return false;
	op = (OpExpr *) rinfo->clause;
	if (list_length(op->args) != 2 || !op_strict(op->opno))
		return false;
	for (i = 0; i < 2; i++)
	{
		Node	   *arg = (Node *) list_nth(op->args, i);
		Node	   *stripped = lion_fkjoin_strip(arg);

		if (stripped == NULL || !IsA(stripped, Var))
			return false;
		argexpr[i] = arg;
		argvar[i] = (Var *) stripped;
		if (argvar[i]->varattno <= 0 || argvar[i]->varlevelsup != 0)
			return false;
	}
	fi = (argvar[0]->varno == (int) innerrel->relid) ? 0 : 1;
	di = 1 - fi;
	if (argvar[fi]->varno != (int) innerrel->relid ||
		!bms_is_member(argvar[di]->varno, outerrel->relids))
		return false;
	dim = root->simple_rel_array[argvar[di]->varno];
	if (dim == NULL || dim->reloptkind != RELOPT_BASEREL)
		return false;

	/* ---- the outer side's rows carry the key ---- */
	if (!lion_fkjoin_target_has(outerrel->reltarget, argvar[di]))
		return false;
	if (outerrel->cheapest_total_path == NULL ||
		outerrel->cheapest_total_path->param_info != NULL)
		return false;

	memset(fj, 0, sizeof(LionFkJoin));
	fj->factrel = innerrel;
	fj->dimrel = dim;
	fj->dimchild = outerrel;
	fj->fkvar = argvar[fi];
	fj->pkvar = argvar[di];
	fj->pkexpr = argexpr[di];
	fj->opno = op->opno;
	fj->collation = op->inputcollid;
	fj->clause = (Node *) op;
	fj->dimpath = outerrel->cheapest_total_path;
	fj->jointype = jointype;
	fj->joinrel = joinrel;
	fj->uniqsortop = InvalidOid;
	fj->joinpath = true;
	return true;
}
