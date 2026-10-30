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
 *		key holds.  Uniqueness is proved here only for what rests on it - a
 *		GROUP BY of the key, its keys united, the dimension LEFT JOIN the fact
 *		(keyunique) - rather than taken from core's
 *		relation_has_unique_index_for(), which before PostgreSQL 19 does not
 *		compare collations.
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
 *		The dimension may be any join of the other tables: tables inner-joined
 *		to one another, and tables joined to them as the inner side of semi
 *		and anti joins - a NOT EXISTS or a `LEFT JOIN ... IS NULL` - whose
 *		rows the dimension's are not - DESIGN.md §27, "A dimension that is a
 *		join" and "A dimension that is any join".  The node's child is then
 *		core's cheapest path of that join rel, and each table of the query
 *		that qualifies as the fact is offered as one.  What this file checks
 *		for it is in the SpecialJoinInfos: which of them joins the fact, that
 *		none of the others involves the fact, and which table outside every
 *		inner side the key is taken from (lion_fkjoin_pick_dim()).
 *
 *		A table inner-joined to the fact on a unique key of its own, nothing of
 *		it wanted above the join, only filters the fact's rows: it is folded
 *		into a fact filter on the fk column (lion_fkjoin_filter_of()), and the
 *		other tables are the dimension - DESIGN.md §27, "A second dimension
 *		that only filters".
 *
 *		The semi and anti joins are also offered as JOIN paths, whose rows
 *		are the outer side's - DESIGN.md §27, "The semi and anti join as a
 *		join path".  lion_fkjoin_recognize_join() decides, for one call of
 *		set_join_pathlist_hook, whether the join it is asked about is one: a
 *		plain semi or anti join, the fact alone its inner side, one fk
 *		equality its only clause, whatever the outer side is.
 *
 *		And the anti join the other way round: `SELECT count(*) FROM t WHERE
 *		... AND NOT EXISTS (SELECT 1 FROM s WHERE s.a = t.k ...)`, whose rows
 *		are the counted table's - its count less the rows some row of a small
 *		inner side matches, looked up through a B-tree of t (DESIGN.md §27,
 *		"The anti join over a small inner side").  What this file decides of
 *		it is the shape: one table, one NOT EXISTS of everything else, and
 *		the clauses between them, at least one a key
 *		(lion_fkjoin_recognize_small_anti()).
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/stratnum.h"
#include "catalog/pg_am.h"
#include "catalog/pg_class.h"
#include "nodes/nodeFuncs.h"
#include "nodes/pathnodes.h"
#include "optimizer/clauses.h"
#include "optimizer/optimizer.h"
#include "optimizer/pathnode.h"
#include "optimizer/paths.h"
#include "parser/parsetree.h"
#include "utils/lsyscache.h"

#include "lion_count.h"
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
 * Is `sjinfo` the anti join core makes of a NOT EXISTS (or of a NOT IN it
 * proves the same), rather than the one it makes of `LEFT JOIN ... WHERE f.fk
 * IS NULL`?
 *
 * The second always has a relid of its own (ojrelid): the LEFT JOIN's range
 * table entry, which the parser made of the FROM clause.  The first had none
 * until PostgreSQL commit 0d6103e ("Give antijoins made from sublinks a
 * relid", in 19 and back-patched to the minor releases of 16, 17 and 18): it
 * now gets a join entry of its own too, made by the planner and so not
 * inFromCl, so that a PlaceHolderVar's qual cannot be pushed into its inner
 * side.  Nothing above the join can name that entry or a column of the inner
 * side either way, so the two spellings of a NOT EXISTS are the same join to
 * the node, on every release before and after the change.
 */
static bool
lion_fkjoin_sublink_anti(PlannerInfo *root, SpecialJoinInfo *sjinfo)
{
	RangeTblEntry *rte;

	if (sjinfo->jointype != JOIN_ANTI)
		return false;
	if (sjinfo->ojrelid == 0)
		return true;
	if (sjinfo->ojrelid >= (Index) root->simple_rel_array_size)
		return false;
	rte = root->simple_rte_array[sjinfo->ojrelid];
	return rte != NULL && rte->rtekind == RTE_JOIN && !rte->inFromCl;
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
	else if (lion_rel_is_unexpanded_parent(rte))
		return false;
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
 * would prove it too; this does not look for one.  A key set of
 * LionOrderedJoin's, whose keys are those of the rows its own clauses let
 * through, does (oj_keyset_unique(), lion_ordjoin.c).
 *
 * The index that proves it, or NULL: the first of rel->indexlist that does,
 * which LionProbeJoin (DESIGN.md §46) and LionOrderedJoin (§47) descend.
 */
IndexOptInfo *
lion_column_unique_index(RelOptInfo *rel, AttrNumber attno, Oid opno,
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
		return ind;
	}

	return NULL;
}

/* Is it unique: does lion_column_unique_index() find an index that proves it? */
bool
lion_column_is_unique(RelOptInfo *rel, AttrNumber attno, Oid opno,
					  Oid collation)
{
	return lion_column_unique_index(rel, attno, opno, collation) != NULL;
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
List *
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
 * May the table `xrelid` of joinrel be folded into a filter of the fact
 * (DESIGN.md §27, "A second dimension that only filters")?  Fills *out when
 * it may:
 *
 *	- a plain table or a materialized view, in no special join - no semi or
 *	  anti join's side and no outer join's - so that it is inner-joined;
 *	- nothing of it wanted above the join: no column of it in the join rel's
 *	  target, which holds every column the query reads above the join (its
 *	  target list, GROUP BY, HAVING, ORDER BY), so its columns are read by
 *	  its own filters and the join clause alone;
 *	- exactly one clause joins it to the rest of the join, and that clause is
 *	  between it and the fact: the equivalence classes' clauses for it
 *	  against everything else, and its joininfo clauses, are one, and the
 *	  pair (fact, it) gives that same clause.  A table also joined to the
 *	  dimension, or by two clauses, is a dimension's, not a filter;
 *	- that clause is a strict operator between a plain column of each, and
 *	  the table's column is provably unique under it
 *	  (lion_column_is_unique()), so that no fact row is repeated: each has
 *	  one fk value, and each value at most one row of the table;
 *	- the table's cheapest path is unparameterized and carries the key, and
 *	  the key's type has an array type for the keys to be collected into;
 *	- the rows that path is expected to return are no more than a count
 *	  locates of one IN list at once (lion_array_batch_size(), a work_mem of
 *	  located sets): the node holds the list's sets whole, one a key, where
 *	  the ordinary plan would hash them within its memory.
 *
 * Then the join keeps the fact rows whose fk equals the key of one of the
 * table's rows that pass its filters, each once, and nothing else of the
 * table reaches the result: `fk = ANY (<those keys>)`, a fact filter.
 */
static bool
lion_fkjoin_filter_of(PlannerInfo *root, RelOptInfo *joinrel,
					  RelOptInfo *fact, int xrelid, LionFkFilter *out)
{
	RelOptInfo *x = root->simple_rel_array[xrelid];
	Relids		others;
	List	   *clauses;
	List	   *pair;
	RestrictInfo *rinfo;
	RestrictInfo *pri;
	OpExpr	   *op;
	Node	   *argexpr[2];
	Var		   *argvar[2];
	int			fi;
	int			di;
	int			i;
	ListCell   *lc;

	if (!lion_fkjoin_rel_ok(root, x) || lion_fkjoin_is_parent(root, x))
		return false;
	foreach(lc, root->join_info_list)
	{
		SpecialJoinInfo *sjinfo = (SpecialJoinInfo *) lfirst(lc);

		if (bms_is_member(xrelid, sjinfo->min_lefthand) ||
			bms_is_member(xrelid, sjinfo->min_righthand) ||
			bms_is_member(xrelid, sjinfo->syn_lefthand) ||
			bms_is_member(xrelid, sjinfo->syn_righthand))
			return false;
	}
	foreach(lc, joinrel->reltarget->exprs)
	{
		if (bms_is_member(xrelid, pull_varnos(root, (Node *) lfirst(lc))))
			return false;
	}

	/* ---- one clause joins it to the rest, and it is the fact's ---- */
	others = bms_del_member(bms_copy(joinrel->relids), xrelid);
	clauses = generate_join_implied_equalities(root, joinrel->relids, others,
											   x, NULL);
	foreach(lc, x->joininfo)
	{
		RestrictInfo *ri = (RestrictInfo *) lfirst(lc);

		if (bms_is_subset(ri->required_relids, joinrel->relids))
			clauses = lappend(clauses, ri);
	}
	if (list_length(clauses) != 1)
		return false;
	rinfo = (RestrictInfo *) linitial(clauses);
	pair = (xrelid < (int) fact->relid) ?
		lion_fkjoin_pair_clauses(root, x, fact) :
		lion_fkjoin_pair_clauses(root, fact, x);
	if (list_length(pair) != 1)
		return false;
	pri = (RestrictInfo *) linitial(pair);
	if (!IsA(rinfo, RestrictInfo) || !IsA(pri, RestrictInfo))
		return false;
	if (rinfo->parent_ec != NULL ? pri->parent_ec != rinfo->parent_ec :
		pri != rinfo)
		return false;
	if (pri->pseudoconstant || !IsA(pri->clause, OpExpr))
		return false;
	op = (OpExpr *) pri->clause;
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
	fi = (argvar[0]->varno == (int) fact->relid) ? 0 : 1;
	di = 1 - fi;
	if (argvar[fi]->varno != (int) fact->relid || argvar[di]->varno != xrelid)
		return false;

	/* ---- its key unique, its path one the node may run ---- */
	if (!lion_column_is_unique(x, argvar[di]->varattno, op->opno,
							   op->inputcollid))
		return false;
	if (!OidIsValid(get_array_type(exprType(argexpr[di]))))
		return false;
	if (x->cheapest_total_path == NULL ||
		x->cheapest_total_path->param_info != NULL ||
		!lion_fkjoin_target_has(x->reltarget, argvar[di]))
		return false;
	if (x->cheapest_total_path->rows > (double) lion_array_batch_size())
		return false;

	out->rel = x;
	out->fkvar = argvar[fi];
	out->pkvar = argvar[di];
	out->pkexpr = argexpr[di];
	out->opno = op->opno;
	out->collation = op->inputcollid;
	out->clause = (Node *) op;
	out->path = x->cheapest_total_path;
	return true;
}

/*
 * Which of the dimension's tables `visible` - those whose rows its rows are,
 * none of them inside a semi or anti join - holds its key: the one table of
 * them joined to the fact by exactly one clause, a strict operator between a
 * plain column of each, and the one the semi or anti join with the fact is
 * made on (`key`, its outer side, or the forward one's inner side).  Several
 * may qualify where the key's equivalence class holds a column of each - the
 * fact joined to `d ⋈ x` on `f.fk = d.pk = x.pk` - and any of them is the key,
 * the class holding them equal in every row; one whose column an index
 * proves unique is taken first, so that a dimension of one table keeps its
 * proof.  0 when none does.
 */
static int
lion_fkjoin_pick_dim(PlannerInfo *root, RelOptInfo *fact, Relids visible,
					 SpecialJoinInfo *key, bool forward)
{
	int			first = 0;
	int			i = -1;

	while ((i = bms_next_member(visible, i)) >= 0)
	{
		RelOptInfo *cand = root->simple_rel_array[i];
		List	   *clauses;
		RestrictInfo *rinfo;
		OpExpr	   *op;
		Var		   *dv = NULL;
		bool		factside = false;
		ListCell   *lc;

		if (!lion_fkjoin_rel_ok(root, cand))
			continue;
		if (key != NULL &&
			!bms_is_member(i, forward ? key->min_righthand :
						   key->min_lefthand))
			continue;
		clauses = (i < (int) fact->relid) ?
			lion_fkjoin_pair_clauses(root, cand, fact) :
			lion_fkjoin_pair_clauses(root, fact, cand);
		if (list_length(clauses) != 1)
			continue;
		rinfo = (RestrictInfo *) linitial(clauses);
		if (!IsA(rinfo, RestrictInfo) || rinfo->pseudoconstant ||
			!IsA(rinfo->clause, OpExpr))
			continue;
		op = (OpExpr *) rinfo->clause;
		if (list_length(op->args) != 2 || !op_strict(op->opno))
			continue;
		foreach(lc, op->args)
		{
			Node	   *arg = lion_fkjoin_strip((Node *) lfirst(lc));

			if (arg == NULL || !IsA(arg, Var) ||
				((Var *) arg)->varattno <= 0 ||
				((Var *) arg)->varlevelsup != 0)
				break;
			if (((Var *) arg)->varno == (int) fact->relid)
				factside = true;
			else if (((Var *) arg)->varno == i)
				dv = (Var *) arg;
		}
		if (lc != NULL || !factside || dv == NULL)
			continue;
		if (lion_column_is_unique(cand, dv->varattno, op->opno,
								  op->inputcollid))
			return i;
		if (first == 0)
			first = i;
	}
	return first;
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
 * The dimension's rows are rows of its VISIBLE tables, those no semi or anti
 * join has on its inner side: a semi or anti join returns rows of its outer
 * side, each at most once, and the tables inside its inner side add none to
 * them.  With one visible table they are that table's rows, each at most
 * once; with several, inner-joined, a row of one may repeat, which an inner
 * join's count is exact over (DESIGN.md §27, "A dimension that is any join")
 * and only a GROUP BY of the key, or its keys united, asks about (keyunique).  The key is taken
 * from one of them (lion_fkjoin_pick_dim()).
 *
 * The tables of `folded` are dimensions that only filter, `filters` their
 * LionFkFilters: not the dimension's, and taken only beside an inner join of
 * the fact and the dimension.
 */
static bool
lion_fkjoin_try_fact(PlannerInfo *root, RelOptInfo *joinrel, int factrelid,
					 Index leftoj, Relids folded, List *filters,
					 LionFkJoin *fj)
{
	RelOptInfo *fact = root->simple_rel_array[factrelid];
	RelOptInfo *dim;
	RelOptInfo *dimchild;
	Relids		dimrels;
	Relids		hidden = NULL;
	Relids		visible;
	SpecialJoinInfo *key = NULL;
	bool		forward = false;
	bool		joined;
	bool		keyunique;
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
	if (leftoj != 0)
		dimrels = bms_del_member(dimrels, leftoj);

	/*
	 * The relid of the NOT EXISTS whose inner side is the fact is the join's
	 * of the fact and the dimension, not the dimension's.  A NOT EXISTS
	 * inside the dimension keeps its own: the dimension's join rel has it.
	 */
	foreach(lc, root->join_info_list)
	{
		SpecialJoinInfo *sjinfo = (SpecialJoinInfo *) lfirst(lc);

		if (sjinfo->ojrelid != 0 &&
			bms_equal(sjinfo->syn_righthand, fact->relids))
			dimrels = bms_del_member(dimrels, sjinfo->ojrelid);
	}
	dimrels = bms_del_members(dimrels, folded);
	if (bms_is_empty(dimrels))
		return false;
	joined = (bms_membership(dimrels) == BMS_MULTIPLE);

	foreach(lc, root->join_info_list)
	{
		SpecialJoinInfo *sjinfo = (SpecialJoinInfo *) lfirst(lc);

		if (sjinfo->jointype == JOIN_LEFT)
		{
			/*
			 * The dimension LEFT JOIN the fact, the one special join there
			 * is (lion_fkjoin_recognize()): the fact has to be its whole
			 * nullable side, and the dimension, one table, its whole other
			 * side.  The fact on the other side keeps fact rows with no
			 * dimension row, which no lookup per dimension row counts.
			 */
			if (joined ||
				!bms_equal(sjinfo->min_righthand, fact->relids) ||
				!bms_equal(sjinfo->syn_righthand, fact->relids) ||
				!bms_equal(sjinfo->min_lefthand, dimrels) ||
				!bms_equal(sjinfo->syn_lefthand, dimrels))
				return false;
			key = sjinfo;
			continue;
		}
		if (bms_equal(sjinfo->min_righthand, fact->relids) &&
			bms_equal(sjinfo->syn_righthand, fact->relids))
		{
			/*
			 * the fact alone inside an EXISTS, an IN or a NOT EXISTS - not
			 * the anti join of a `LEFT JOIN fact ... WHERE f.fk IS NULL`,
			 * which is left to the ordinary plan
			 */
			if (key != NULL)
				return false;
			if (sjinfo->jointype == JOIN_ANTI &&
				!lion_fkjoin_sublink_anti(root, sjinfo))
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

	/*
	 * ---- the dimension's own table ----
	 *
	 * Of the tables no semi or anti join hides, whose rows the dimension's
	 * are: the one whose column the key is (lion_fkjoin_pick_dim()).
	 */
	visible = bms_difference(bms_difference(dimrels, hidden),
							 root->outer_join_rels);
	dimrelid = lion_fkjoin_pick_dim(root, fact, visible, key, forward);
	if (dimrelid <= 0)
		return false;
	dim = root->simple_rel_array[dimrelid];

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
		Relids		dimfact = bms_add_member(bms_copy(dimrels), factrelid);
		List	   *whole;
		RestrictInfo *wri;

		whole = generate_join_implied_equalities(root, dimfact, dimrels, fact,
												 NULL);
		foreach(lc, fact->joininfo)
		{
			RestrictInfo *ri = (RestrictInfo *) lfirst(lc);

			if (bms_is_subset(ri->required_relids, dimfact))
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

	/*
	 * A LEFT JOIN's other clauses are no restrictions of either table the
	 * node could apply: a condition of its ON clause on the dimension alone
	 * decides which dimension rows are null-extended, and a WHERE condition
	 * on the fact that is not strict (`f.x IS NULL`, `coalesce(f.x, 0) = 0`)
	 * is evaluated above the join, over the null-extended rows too.  Both are
	 * join clauses of the two tables - the second with the outer join's own
	 * relid among the relations it needs, which the pair's clauses above do
	 * not look at - so the one join clause must be every join clause either
	 * table has.  A condition of the ON clause on the fact alone is pushed
	 * down to the fact's scan, where it is a fact filter like any other: it
	 * only decides which fact rows match.
	 */
	if (key != NULL && key->jointype == JOIN_LEFT)
	{
		foreach(lc, fact->joininfo)
		{
			if (lfirst(lc) != rinfo)
				return false;
		}
		foreach(lc, dim->joininfo)
		{
			if (lfirst(lc) != rinfo)
				return false;
		}
	}
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
	keyunique = bms_membership(visible) == BMS_SINGLETON &&
		lion_column_is_unique(dim, argvar[di]->varattno, op->opno,
							  op->inputcollid);
	if (key == NULL || key->jointype == JOIN_LEFT)
	{
		/*
		 * An inner join's count is a sum over the dimension rows, whatever
		 * they hold: a key two of them share is counted for each, as the
		 * join pairs it with each.  So any join of the dimension's tables
		 * will do, and its rows are core's to make; only a GROUP BY of the
		 * key, or its keys united, asks that no key repeat (keyunique,
		 * lion_plan_fkjoin.c).  A
		 * LEFT JOIN's count is a sum over the dimension rows too, each row's
		 * own count or - with no fact row - its one null-extended row, and
		 * keeps the proof as its scope guard.
		 */
		if (key != NULL && !keyunique)
			return false;
		jointype = (key == NULL) ? JOIN_INNER : JOIN_LEFT;
	}
	else if (forward)
	{
		if (joined && keyunique)
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

	/* a dimension that only filters is folded beside an inner join alone */
	if (filters != NIL && jointype != JOIN_INNER)
		return false;

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
	fj->filters = filters;
	fj->dimvisible = visible;
	fj->keyunique = keyunique;
	return true;
}

int
lion_fkjoin_recognize(PlannerInfo *root, RelOptInfo *joinrel, LionFkJoin **out)
{
	LionFkJoin *fj;
	int			n = 0;
	int			i;
	Index		leftoj = 0;		/* a LEFT JOIN's own relid, or 0 */
	Relids		tables;			/* joinrel's relids less outer joins' */
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
	 * inner join's: a fact LEFT JOIN its dimension keeps the fact rows with
	 * no dimension row, which no lookup per dimension row finds.  The other
	 * way round, the dimension LEFT JOIN the fact keeps the dimension rows
	 * with no fact row, each null-extended once, and that is the lookup per
	 * dimension row with a row emitted for a count of 0 too (DESIGN.md §27,
	 * "The dimension LEFT JOIN the fact"); it is taken when it is the only
	 * special join.  So does a semi or an anti join - EXISTS, IN, NOT EXISTS pulled up into
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
	 * So semi and anti joins, within the join, or that one LEFT JOIN.  An
	 * anti join made from `LEFT JOIN ... WHERE x.k IS NULL` is let through
	 * too, for the dimension's own (lion_fkjoin_try_fact()): inside the
	 * dimension it is core's to run, as a NOT EXISTS is, and its rows are its
	 * outer side's.  With the fact its inner side it is left alone.  The NOT
	 * EXISTS one is told apart from it by lion_fkjoin_sublink_anti(), and
	 * either one's relid is no table of the join.
	 */
	foreach(lc, root->join_info_list)
	{
		SpecialJoinInfo *sjinfo = (SpecialJoinInfo *) lfirst(lc);

		/*
		 * ... or one LEFT JOIN of a dimension table and the fact, the only
		 * special join of the query, whose rows are every dimension row with
		 * its fact rows, or with none (DESIGN.md §27, "The dimension LEFT
		 * JOIN the fact").  Its relid is one of the join rel's.
		 */
		if (sjinfo->jointype == JOIN_LEFT && sjinfo->ojrelid != 0 &&
			list_length(root->join_info_list) == 1 &&
			bms_membership(sjinfo->min_lefthand) == BMS_SINGLETON &&
			bms_membership(sjinfo->min_righthand) == BMS_SINGLETON &&
			bms_is_member(sjinfo->ojrelid, joinrel->relids) &&
			bms_num_members(joinrel->relids) == 3)
		{
			leftoj = sjinfo->ojrelid;
			continue;
		}
		if (sjinfo->jointype != JOIN_SEMI && sjinfo->jointype != JOIN_ANTI)
			return 0;
		if (!bms_is_subset(sjinfo->syn_lefthand, joinrel->relids) ||
			!bms_is_subset(sjinfo->syn_righthand, joinrel->relids) ||
			bms_overlap(sjinfo->min_lefthand, sjinfo->min_righthand))
			return 0;
	}

	/*
	 * ---- every table of the join one the node may take apart ----
	 *
	 * The tables, that is: an outer join's relid among the join rel's - the
	 * LEFT JOIN's, or a NOT EXISTS's (lion_fkjoin_sublink_anti()) - is none.
	 */
	tables = bms_difference(joinrel->relids, root->outer_join_rels);
	i = -1;
	while ((i = bms_next_member(tables, i)) >= 0)
	{
		if (!lion_fkjoin_member_ok(root, i))
			return 0;
	}

	/*
	 * ---- each table that may be the fact, the rest the dimension ----
	 *
	 * Less the tables that only filter that fact (lion_fkjoin_filter_of()),
	 * which are folded into its filters - beside no LEFT JOIN, which is a
	 * join of two tables.  Where every other table only filters, each of
	 * them is tried as the dimension in turn, the others folded: a count of
	 * the fact joined to two dimensions that each filter it is either one
	 * looked up and the other folded.
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
								bms_num_members(joinrel->relids) *
								bms_num_members(joinrel->relids));
	i = -1;
	while ((i = bms_next_member(tables, i)) >= 0)
	{
		RelOptInfo *fact = root->simple_rel_array[i];
		Relids		folded = NULL;
		List	   *filters = NIL;
		int			j;

		if (leftoj == 0 && lion_fkjoin_rel_ok(root, fact))
		{
			j = -1;
			while ((j = bms_next_member(tables, j)) >= 0)
			{
				LionFkFilter filt;

				if (j == i ||
					!lion_fkjoin_filter_of(root, joinrel, fact, j, &filt))
					continue;
				folded = bms_add_member(folded, j);
				filters = lappend(filters, palloc(sizeof(LionFkFilter)));
				memcpy(llast(filters), &filt, sizeof(LionFkFilter));
			}
		}

		if (!bms_is_subset(tables, bms_add_member(bms_copy(folded), i)))
		{
			/* a dimension besides them */
			if (lion_fkjoin_try_fact(root, joinrel, i, leftoj, folded,
									 filters, &fj[n]))
				n++;
			continue;
		}

		/* every other table only filters: each of them the dimension */
		foreach(lc, filters)
		{
			LionFkFilter *dim = (LionFkFilter *) lfirst(lc);
			List	   *rest = list_delete_ptr(list_copy(filters), dim);

			if (lion_fkjoin_try_fact(root, joinrel, i, leftoj,
									 bms_del_member(bms_copy(folded),
													dim->rel->relid),
									 rest, &fj[n]))
				n++;
		}
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
 *	- the special join is this call's own (extra->sjinfo), a semi join or the
 *	  anti join of a NOT EXISTS (lion_fkjoin_sublink_anti()): an anti join
 *	  made from `LEFT JOIN ... WHERE f.fk IS NULL` is left alone as the upper
 *	  node leaves it;
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
	if (sjinfo == NULL || sjinfo->jointype != jointype)
		return false;
	if (sjinfo->ojrelid != 0 && !lion_fkjoin_sublink_anti(root, sjinfo))
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
	fj->dimvisible = bms_difference(outerrel->relids, root->outer_join_rels);
	fj->keyunique = false;
	return true;
}

/*
 * Is `node` an expression of the inner side alone - Vars of `inner` and
 * nothing of `outerrelid` - with no volatile function and no subquery, which
 * the node may evaluate once per row of its child?
 */
static bool
lion_anti_inner_expr(PlannerInfo *root, Node *node, Relids inner,
					 int outerrelid)
{
	Relids		varnos = pull_varnos(root, node);

	return !bms_is_empty(varnos) && bms_is_subset(varnos, inner) &&
		!bms_is_member(outerrelid, varnos) &&
		!contain_volatile_functions(node) && !contain_subplans(node);
}

/*
 * The anti join over a small inner side (DESIGN.md §27, "The anti join over a
 * small inner side"): joinrel is the whole FROM of a query over one table `t`
 * with a NOT EXISTS correlated with it, and nothing else -
 *
 *	- the special join is the anti join core makes of a NOT EXISTS - or,
 *	  from PostgreSQL 19, of a NOT IN whose two sides it proves never NULL,
 *	  which is then the same join (16 to 18 leave every NOT IN a subplan) -
 *	  and never one of `LEFT JOIN ... IS NULL` (lion_fkjoin_sublink_anti());
 *	  its outer side t alone, minimal and syntactic, and its syntactic inner
 *	  side every other table of the join; every other special join is inside
 *	  that inner side - an EXISTS, a NOT EXISTS or an outer join of the
 *	  subquery's own, which the child plans;
 *	- t is a plain table or a materialized view (lion_fkjoin_rel_ok(), and
 *	  not a partitioned table) and the inner side's tables have no LATERAL
 *	  reference and no TABLESAMPLE (lion_fkjoin_member_ok());
 *	- the inner side is a rel the join search built, not proved empty, whose
 *	  cheapest path is unparameterized: the child, run once;
 *	- every clause the anti join evaluates - an equivalence class's for the
 *	  pair (none, for an outer join's clauses, which stay in t's joininfo), and
 *	  t's joininfo clauses within the join, a clause of the subquery on t alone
 *	  among them - reads plain columns of t and columns the inner side's rows
 *	  carry, has no volatile function, subquery or pseudoconstant, and at
 *	  least one of them is a key: a strict operator between a column of t and
 *	  an expression of the inner side alone (LionAntiKey).
 *
 * A row of t is then a row of the join when no row of the inner side makes
 * every clause true, and the count is t's count less the rows some inner row
 * does: which rows those are is the caller's to find, by the keys.  A
 * PlaceHolderVar, a pseudoconstant qual or a LATERAL reference anywhere
 * declines, as for the FK-side join.
 */
bool
lion_fkjoin_recognize_small_anti(PlannerInfo *root, RelOptInfo *joinrel,
								 LionSmallAnti *out)
{
	SpecialJoinInfo *anti = NULL;
	RelOptInfo *outer;
	RelOptInfo *inner;
	Relids		tables;
	Relids		innertables = NULL;
	List	   *rinfos;
	List	   *clauses = NIL;
	List	   *keys = NIL;
	int			outerrelid = -1;
	int			innerrelid;
	int			i;
	ListCell   *lc;

	if (joinrel->reloptkind != RELOPT_JOINREL || IS_DUMMY_REL(joinrel))
		return false;
	if (root->placeholder_list != NIL)
		return false;
	if (root->hasPseudoConstantQuals || root->hasLateralRTEs)
		return false;

	/* ---- the anti join: t alone outside, every other table inside ---- */
	tables = bms_difference(joinrel->relids, root->outer_join_rels);
	foreach(lc, root->join_info_list)
	{
		SpecialJoinInfo *sjinfo = (SpecialJoinInfo *) lfirst(lc);
		Relids		lt;
		Relids		rt;
		int			x;

		if (!lion_fkjoin_sublink_anti(root, sjinfo))
			continue;
		lt = bms_difference(sjinfo->syn_lefthand, root->outer_join_rels);
		rt = bms_difference(sjinfo->syn_righthand, root->outer_join_rels);
		if (!bms_get_singleton_member(lt, &x) ||
			!bms_equal(bms_union(lt, rt), tables))
			continue;
		anti = sjinfo;
		outerrelid = x;
		innertables = rt;
		break;
	}
	if (anti == NULL || bms_is_empty(innertables) ||
		!bms_equal(anti->min_lefthand, anti->syn_lefthand))
		return false;
	foreach(lc, root->join_info_list)
	{
		SpecialJoinInfo *sjinfo = (SpecialJoinInfo *) lfirst(lc);

		if (sjinfo == anti)
			continue;
		if (!bms_is_subset(sjinfo->syn_lefthand, anti->syn_righthand) ||
			!bms_is_subset(sjinfo->syn_righthand, anti->syn_righthand))
			return false;
	}

	/* ---- the table counted, and the inner side's tables ---- */
	outer = root->simple_rel_array[outerrelid];
	if (!lion_fkjoin_rel_ok(root, outer) || lion_fkjoin_is_parent(root, outer))
		return false;
	i = -1;
	while ((i = bms_next_member(innertables, i)) >= 0)
	{
		if (!lion_fkjoin_member_ok(root, i))
			return false;
	}

	/* ---- the inner side: the child ---- */
	if (bms_get_singleton_member(innertables, &innerrelid) &&
		bms_equal(anti->syn_righthand, innertables))
		inner = root->simple_rel_array[innerrelid];
	else
		inner = find_join_rel(root, anti->syn_righthand);
	if (inner == NULL || IS_DUMMY_REL(inner))
		return false;
	if (!bms_is_empty(inner->lateral_relids))
		return false;
	if (inner->cheapest_total_path == NULL ||
		inner->cheapest_total_path->param_info != NULL)
		return false;

	/* ---- every clause of the anti join ---- */
	rinfos = generate_join_implied_equalities(root, joinrel->relids,
											  outer->relids, inner, NULL);
	foreach(lc, outer->joininfo)
	{
		RestrictInfo *ri = (RestrictInfo *) lfirst(lc);

		if (bms_is_subset(ri->required_relids, joinrel->relids))
			rinfos = list_append_unique_ptr(rinfos, ri);
	}
	foreach(lc, rinfos)
	{
		RestrictInfo *ri = (RestrictInfo *) lfirst(lc);
		Node	   *clause;
		List	   *vars;
		ListCell   *lv;

		if (!IsA(ri, RestrictInfo) || ri->pseudoconstant)
			return false;
		clause = (Node *) ri->clause;
		if (contain_volatile_functions(clause) || contain_subplans(clause) ||
			contain_agg_clause(clause) || contain_window_function(clause) ||
			expression_returns_set(clause))
			return false;

		/* plain columns of t, and columns of the inner side's rows */
		vars = pull_var_clause(clause, 0);
		foreach(lv, vars)
		{
			Var		   *v = (Var *) lfirst(lv);

			if (!IsA(v, Var) || v->varlevelsup != 0 || v->varattno <= 0)
				return false;
			if (v->varno == outerrelid)
				continue;
			if (!bms_is_member(v->varno, innertables) ||
				!lion_fkjoin_target_has(inner->reltarget, v))
				return false;
		}
		clauses = lappend(clauses, ri);

		/* ... and the keys among them */
		if (IsA(clause, OpExpr) &&
			list_length(((OpExpr *) clause)->args) == 2 &&
			op_strict(((OpExpr *) clause)->opno))
		{
			OpExpr	   *op = (OpExpr *) clause;

			for (i = 0; i < 2; i++)
			{
				Node	   *side = lion_fkjoin_strip((Node *) list_nth(op->args, i));
				Node	   *other = (Node *) list_nth(op->args, 1 - i);
				LionAntiKey *key;

				if (side == NULL || !IsA(side, Var) ||
					((Var *) side)->varno != outerrelid ||
					!lion_anti_inner_expr(root, other, innertables,
										  outerrelid))
					continue;
				key = (LionAntiKey *) palloc0(sizeof(LionAntiKey));
				key->clause = op;
				key->outervar = (Var *) side;
				key->innerexpr = other;
				key->outerleft = (i == 0);
				keys = lappend(keys, key);
				break;
			}
		}
	}
	if (keys == NIL)
		return false;

	out->joinrel = joinrel;
	out->outerrel = outer;
	out->innerrel = inner;
	out->innerpath = inner->cheapest_total_path;
	out->clauses = clauses;
	out->keys = keys;
	return true;
}

/*
 * The B-tree family's equality of a key column of type `keytype` (the
 * family's own input type) with a value of the type `clause` compares it
 * with: the clause's operator, an equality of `idx`'s family, taken apart
 * into its two input types, `keyleft` saying which one is the key's.  What a
 * lookup of the other side's value in the B-tree is made with, as
 * ExecIndexBuildScanKeys() makes a scan key of an index qual.  InvalidOid
 * when the family has none, or the operator is not its equality.
 */
static Oid
lion_probe_lookup_op(IndexOptInfo *idx, OpExpr *clause, bool keyleft)
{
	int			strategy;
	Oid			lefttype;
	Oid			righttype;

	if (!op_in_opfamily(clause->opno, idx->opfamily[0]))
		return InvalidOid;
	get_op_opfamily_properties(clause->opno, idx->opfamily[0], false,
							   &strategy, &lefttype, &righttype);
	if (strategy != BTEqualStrategyNumber)
		return InvalidOid;
	return get_opfamily_member(idx->opfamily[0], idx->opcintype[0],
							   keyleft ? righttype : lefttype,
							   BTEqualStrategyNumber);
}

/* Is node, its relabels peeled, a plain user column of relation relid? */
static Var *
lion_probe_plain_var(Node *node, int relid)
{
	Node	   *n = lion_fkjoin_strip(node);

	if (n == NULL || !IsA(n, Var))
		return NULL;
	if (((Var *) n)->varno != relid || ((Var *) n)->varattno <= 0 ||
		((Var *) n)->varlevelsup != 0)
		return NULL;
	return (Var *) n;
}

/*
 * The NOT EXISTS of the dimension by probe (DESIGN.md §46, "The anti
 * join"): its one clause is `inner key = fk` or `inner key = pk`, a strict
 * equality of the B-tree's family under its collation - so that, the join
 * having made fk and pk equal under that family, the dimension rows some
 * inner row matches are the ones a lookup of the inner keys in the B-tree
 * finds - and the inner key a plain column the inner side's rows carry.
 * Every clause the anti join evaluates is that one: any other, of the inner
 * side and the join, is declined; the inner side's own are its child's.
 */
static bool
lion_probe_anti(PlannerInfo *root, LionProbeShape *ps,
				SpecialJoinInfo *anti, Relids innertables)
{
	RelOptInfo *inner;
	Relids		outerrels = bms_union(ps->factrel->relids, ps->dimrel->relids);
	List	   *rinfos;
	RestrictInfo *ri;
	OpExpr	   *op;
	Var		   *sides[2];
	int			innerrelid;
	int			k;
	ListCell   *lc;

	if (bms_get_singleton_member(innertables, &innerrelid) &&
		bms_equal(anti->syn_righthand, innertables))
		inner = root->simple_rel_array[innerrelid];
	else
		inner = find_join_rel(root, anti->syn_righthand);
	if (inner == NULL || IS_DUMMY_REL(inner))
		return false;
	if (!bms_is_empty(inner->lateral_relids))
		return false;
	if (inner->cheapest_total_path == NULL ||
		inner->cheapest_total_path->param_info != NULL)
		return false;

	/* every clause of the anti join: an equivalence class's, and joininfo's */
	rinfos = generate_join_implied_equalities(root, ps->joinrel->relids,
											  outerrels, inner, NULL);
	foreach(lc, list_concat(list_copy(ps->factrel->joininfo),
							ps->dimrel->joininfo))
	{
		RestrictInfo *r = (RestrictInfo *) lfirst(lc);

		if (bms_is_subset(r->required_relids, ps->joinrel->relids) &&
			bms_overlap(r->required_relids, innertables))
			rinfos = list_append_unique_ptr(rinfos, r);
	}
	if (list_length(rinfos) != 1)
		return false;
	ri = (RestrictInfo *) linitial(rinfos);
	if (!IsA(ri, RestrictInfo) || ri->pseudoconstant ||
		!IsA(ri->clause, OpExpr))
		return false;
	op = (OpExpr *) ri->clause;
	if (list_length(op->args) != 2 || !op_strict(op->opno) || op->opretset)
		return false;
	if (OidIsValid(ps->collation) ? op->inputcollid != ps->collation :
		OidIsValid(op->inputcollid))
		return false;

	/* one side the fk or the pk, the other a column of the inner side */
	for (k = 0; k < 2; k++)
	{
		Node	   *n = lion_fkjoin_strip((Node *) list_nth(op->args, k));

		sides[k] = (n != NULL && IsA(n, Var) && ((Var *) n)->varlevelsup == 0 &&
					((Var *) n)->varattno > 0) ? (Var *) n : NULL;
	}
	if (sides[0] == NULL || sides[1] == NULL)
		return false;
	for (k = 0; k < 2; k++)
	{
		Var		   *mine = sides[k];
		Var		   *theirs = sides[1 - k];
		bool		joinside;

		joinside = (mine->varno == ps->fkvar->varno &&
					mine->varattno == ps->fkvar->varattno) ||
			(mine->varno == ps->pkvar->varno &&
			 mine->varattno == ps->pkvar->varattno);
		if (!joinside || !bms_is_member(theirs->varno, innertables) ||
			!lion_fkjoin_member_ok(root, theirs->varno) ||
			!lion_fkjoin_target_has(inner->reltarget, theirs))
			continue;

		/* the lookup of the inner key: the key on the operator's other side */
		ps->antiop = lion_probe_lookup_op(ps->pkidx, op, k == 0);
		if (!OidIsValid(ps->antiop))
			return false;
		ps->antirel = inner;
		ps->antipath = inner->cheapest_total_path;
		ps->anticlause = ri;
		ps->antivar = theirs;
		return true;
	}
	return false;
}

/* One way round of the dimension by probe: factrelid the fact. */
static bool
lion_probe_try(PlannerInfo *root, RelOptInfo *joinrel, int factrelid,
			   int dimrelid, SpecialJoinInfo *anti, Relids innertables,
			   LionProbeShape *ps)
{
	RelOptInfo *fact = root->simple_rel_array[factrelid];
	RelOptInfo *dim = root->simple_rel_array[dimrelid];
	RangeTblEntry *dimrte;
	List	   *clauses;
	RestrictInfo *rinfo;
	OpExpr	   *op;
	Var		   *fk;
	Var		   *pk;
	bool		pkleft;

	memset(ps, 0, sizeof(LionProbeShape));
	if (!lion_fkjoin_rel_ok(root, fact) || !lion_fkjoin_rel_ok(root, dim) ||
		lion_fkjoin_is_parent(root, dim))
		return false;
	dimrte = root->simple_rte_array[dimrelid];
	if (dimrte->securityQuals != NIL)
		return false;

	/* the one clause that joins them */
	clauses = (factrelid < dimrelid) ?
		lion_fkjoin_pair_clauses(root, fact, dim) :
		lion_fkjoin_pair_clauses(root, dim, fact);
	if (list_length(clauses) != 1)
		return false;
	rinfo = (RestrictInfo *) linitial(clauses);
	if (!IsA(rinfo, RestrictInfo) || rinfo->pseudoconstant ||
		!IsA(rinfo->clause, OpExpr))
		return false;
	op = (OpExpr *) rinfo->clause;
	if (list_length(op->args) != 2 || !op_strict(op->opno) || op->opretset)
		return false;
	if ((fk = lion_probe_plain_var((Node *) linitial(op->args),
								   factrelid)) != NULL &&
		(pk = lion_probe_plain_var((Node *) lsecond(op->args),
								   dimrelid)) != NULL)
		pkleft = false;
	else if ((pk = lion_probe_plain_var((Node *) linitial(op->args),
										dimrelid)) != NULL &&
			 (fk = lion_probe_plain_var((Node *) lsecond(op->args),
										factrelid)) != NULL)
		pkleft = true;
	else
		return false;

	/* ... on a column of the dimension a B-tree makes unique */
	ps->pkidx = lion_column_unique_index(dim, pk->varattno, op->opno,
										 op->inputcollid);
	if (ps->pkidx == NULL)
		return false;
	ps->probeop = lion_probe_lookup_op(ps->pkidx, op, pkleft);
	if (!OidIsValid(ps->probeop))
		return false;
	ps->joinrel = joinrel;
	ps->factrel = fact;
	ps->dimrel = dim;
	ps->joinclause = rinfo;
	ps->fkvar = fk;
	ps->pkvar = pk;
	ps->collation = op->inputcollid;
	if (OidIsValid(ps->collation) &&
		ps->pkidx->indexcollations[0] != ps->collation)
		return false;

	if (anti != NULL && !lion_probe_anti(root, ps, anti, innertables))
		return false;
	return true;
}

/*
 * The dimension by probe (DESIGN.md §46, "What qualifies"): joinrel is the
 * whole FROM of a query over two tables inner-joined on one equality, and
 * at most a NOT EXISTS beside them -
 *
 *	- no special join but that NOT EXISTS, the anti join core makes of one
 *	  (lion_fkjoin_sublink_anti()), whose syntactic outer side is the two
 *	  tables and whose inner side every other table; every other special
 *	  join is inside that inner side;
 *	- each table a plain one or a materialized view - the fact may be a
 *	  partitioned table, which its child reads as core does - with no
 *	  LATERAL reference and no TABLESAMPLE (lion_fkjoin_rel_ok()); the
 *	  dimension with no security barrier quals;
 *	- one clause between them (lion_fkjoin_pair_clauses()), a strict
 *	  equality of plain columns, whose dimension column a unique B-tree of
 *	  the operator's family and collation covers alone
 *	  (lion_column_is_unique()), and which that family can look the fact's
 *	  value up with;
 *	- no PlaceHolderVar, pseudoconstant qual or LATERAL reference anywhere.
 *
 * Each way round is tried, as for the FK-side join.
 */
int
lion_fkjoin_recognize_probe(PlannerInfo *root, RelOptInfo *joinrel,
							LionProbeShape **out)
{
	Relids		tables;
	Relids		outer;
	Relids		innertables = NULL;
	SpecialJoinInfo *anti = NULL;
	LionProbeShape *shapes;
	int			n = 0;
	int			a;
	int			b;
	ListCell   *lc;

	*out = NULL;
	if (joinrel->reloptkind != RELOPT_JOINREL || IS_DUMMY_REL(joinrel))
		return 0;
	if (root->placeholder_list != NIL)
		return 0;
	if (root->hasPseudoConstantQuals || root->hasLateralRTEs)
		return 0;

	tables = bms_difference(joinrel->relids, root->outer_join_rels);
	foreach(lc, root->join_info_list)
	{
		SpecialJoinInfo *sjinfo = (SpecialJoinInfo *) lfirst(lc);
		Relids		lt;
		Relids		rt;

		if (!lion_fkjoin_sublink_anti(root, sjinfo))
			continue;
		lt = bms_difference(sjinfo->syn_lefthand, root->outer_join_rels);
		rt = bms_difference(sjinfo->syn_righthand, root->outer_join_rels);
		if (bms_num_members(lt) != 2 || bms_is_empty(rt) ||
			bms_overlap(lt, rt) || !bms_equal(bms_union(lt, rt), tables))
			continue;
		if (anti != NULL)
			return 0;
		anti = sjinfo;
		innertables = rt;
	}
	foreach(lc, root->join_info_list)
	{
		SpecialJoinInfo *sjinfo = (SpecialJoinInfo *) lfirst(lc);

		if (sjinfo == anti)
			continue;
		if (anti == NULL ||
			!bms_is_subset(sjinfo->syn_lefthand, anti->syn_righthand) ||
			!bms_is_subset(sjinfo->syn_righthand, anti->syn_righthand))
			return 0;
	}
	outer = bms_difference(tables, innertables);
	if (bms_num_members(outer) != 2)
		return 0;
	a = bms_next_member(outer, -1);
	b = bms_next_member(outer, a);
	if (a <= 0 || b <= 0 || a >= root->simple_rel_array_size ||
		b >= root->simple_rel_array_size)
		return 0;
	if (innertables != NULL)
	{
		int			i = -1;

		while ((i = bms_next_member(innertables, i)) >= 0)
		{
			if (!lion_fkjoin_member_ok(root, i))
				return 0;
		}
	}

	shapes = (LionProbeShape *) palloc0(sizeof(LionProbeShape) * 2);
	if (lion_probe_try(root, joinrel, a, b, anti, innertables, &shapes[n]))
		n++;
	if (lion_probe_try(root, joinrel, b, a, anti, innertables, &shapes[n]))
		n++;
	*out = shapes;
	return n;
}
