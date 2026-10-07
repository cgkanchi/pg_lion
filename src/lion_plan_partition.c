/*-------------------------------------------------------------------------
 *
 * lion_plan_partition.c
 *		The relations a LionCount path counts: one table, or the live leaf
 *		partitions of a partitioned one (DESIGN.md §16), with the WHERE clauses
 *		each leaf's partition bounds imply or refute.
 *
 * Part of the LionCount custom scan: lion_customscan.h describes the node
 * and declares what its files share.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "lion_customscan.h"

/*
 * The Var a parent column becomes in one child, or NULL when the child does
 * not have it (a column dropped in that partition).  Partitions may number
 * their columns differently, so every attnum the pushdown carries across a
 * partition boundary goes through here (DESIGN.md §16); the Var itself is
 * what a per-partition group estimate has to be made against, because the
 * statistics live on the child.
 */
Var *
lion_child_var(PlannerInfo *root, Index childrelid, AttrNumber parentattno)
{
	AppendRelInfo *appinfo;
	Var		   *cvar;

	if (parentattno <= 0)
		return NULL;
	if (childrelid == 0 || childrelid >= (Index) root->simple_rel_array_size)
		return NULL;
	if (root->append_rel_array == NULL)
		return NULL;
	appinfo = root->append_rel_array[childrelid];
	if (appinfo == NULL)
		return NULL;
	if ((int) parentattno > list_length(appinfo->translated_vars))
		return NULL;

	cvar = (Var *) list_nth(appinfo->translated_vars, parentattno - 1);
	if (cvar == NULL || !IsA(cvar, Var) || cvar->varattno <= 0)
		return NULL;

	return cvar;
}

/*
 * The same, reduced to the attribute number; 0 when the child lacks it.
 */
static AttrNumber
lion_child_attno(PlannerInfo *root, Index childrelid, AttrNumber parentattno)
{
	Var		   *cvar = lion_child_var(root, childrelid, parentattno);

	return (cvar != NULL) ? cvar->varattno : 0;
}

/*
 * Collect one LionCountTarget per relation the node will count: just rel when
 * it is an ordinary table, or one per live leaf partition when it is a
 * partitioned parent, recursing through sub-partitioned children
 * (DESIGN.md §16).
 *
 * The attribute numbers are rel's own and are translated for every child.
 * The partition set is the planner's already-pruned one: part_rels entries
 * that are non-NULL and in live_parts, minus the ones the planner has since
 * proved empty.
 *
 * Returns false when the pushdown is impossible - a child that is not a plain
 * table (a foreign table, say), a column dropped in some partition, or a leaf
 * without a usable lion index on one of the columns.  An empty *targets
 * means everything was pruned away; the caller leaves that to the planner's
 * own dummy-rel handling.
 *
 * With `imply` (a partitioned table) a leaf leaves out the WHERE clauses its
 * partition bounds imply (lion_leaf_drops()), and needs no index for them:
 * its target's `dropped` says which, and its whereidx holds NULL there.
 */
/*
 * The constraint every row of leaf partition `relation` satisfies, as an
 * implicit-AND list over the leaf's own columns under range table index
 * `relid`, or NIL for a table that is not a partition.  It is the whole of it
 * - RelationGetPartitionQual() adds every ancestor's bound to the leaf's own,
 * so a sub-partition's includes the bound its parent has in the table above -
 * made ready for the planner as get_relation_constraints() makes it for
 * constraint exclusion.
 *
 * Partition constraints never evaluate to NULL (partbounds.c builds them with
 * explicit null tests, and says so where it negates one for a default
 * partition), and a row is in a partition only if its constraint is not false
 * - tuple routing puts it there by its bound, and ExecPartitionCheck() lets
 * nothing else in, nor ATTACH PARTITION without proving or scanning for it.
 * So every row of the leaf makes it TRUE, and a clause it strongly implies is
 * TRUE of every row: exactly what a WHERE clause asks.
 */
List *
lion_leaf_partition_qual(Relation relation, Index relid)
{
	List	   *qual = RelationGetPartitionQual(relation);

	if (qual == NIL)
		return NIL;
	qual = (List *) expression_planner((Expr *) qual);
	if (relid != 1)
		ChangeVarNodes((Node *) qual, 1, (int) relid, 0);
	return qual;
}

/*
 * Does leaf partition `leaf`'s constraint (lion_leaf_partition_qual()) imply
 * `clause`, a WHERE clause over `toprel`'s columns?  The clause is mapped onto
 * the leaf's columns through every level of partitioning between them, which
 * is what the planner does to the restrictions it hands the leaf itself, and
 * the proof is core's own predicate_implied_by(), the one partial indexes and
 * constraint exclusion are decided by.  A clause with a volatile function or
 * a subquery in it is never taken as implied: dropping it would change how
 * often it runs, and the query asked for it per row.
 */
static bool
lion_leaf_implies(PlannerInfo *root, RelOptInfo *leaf, RelOptInfo *toprel,
				  List *partqual, Node *clause)
{
	Node	   *leafclause;

	if (partqual == NIL || leaf == toprel)
		return false;
	if (contain_volatile_functions(clause) || contain_subplans(clause))
		return false;
	leafclause = adjust_appendrel_attrs_multilevel(root, clause, leaf, toprel);
	return predicate_implied_by(list_make1(leafclause), partqual, false);
}

/*
 * The constants a leaf partition's constraint compares column `var` with by
 * an operator (`var = c`, either way round) - the candidates for the one
 * value its bounds may give the column (lion_leaf_bound_value()).  A LIST
 * bound of one value is such a clause, in the leaf's own bound or in an
 * ancestor's; a bound of several is a ScalarArrayOpExpr, which gives the
 * column no one value, and is not looked into.  A few at most: a constraint
 * is a partition's bound and its ancestors'.
 */
#define LION_BOUND_CANDIDATES	16

typedef struct LionBoundCands
{
	Var		   *var;
	List	   *cands;
} LionBoundCands;

static bool
lion_bound_cands_walker(Node *node, LionBoundCands *cx)
{
	if (node == NULL)
		return false;
	if (IsA(node, OpExpr) && list_length(((OpExpr *) node)->args) == 2 &&
		list_length(cx->cands) < LION_BOUND_CANDIDATES)
	{
		OpExpr	   *op = (OpExpr *) node;
		Node	   *l = lion_strip((Node *) linitial(op->args));
		Node	   *r = lion_strip((Node *) lsecond(op->args));
		int			side;

		for (side = 0; side < 2; side++)
		{
			Node	   *v = side ? r : l;
			Node	   *c = side ? l : r;

			if (v != NULL && IsA(v, Var) && c != NULL && IsA(c, Const) &&
				((Var *) v)->varno == cx->var->varno &&
				((Var *) v)->varattno == cx->var->varattno &&
				((Var *) v)->varlevelsup == 0 &&
				!((Const *) c)->constisnull &&
				((Const *) c)->consttype == cx->var->vartype)
				cx->cands = lappend(cx->cands, c);
		}
		return false;
	}
	return expression_tree_walker(node, lion_bound_cands_walker, (void *) cx);
}

/*
 * The one value leaf partition `partqual`'s bounds give column `drive->var`,
 * as a Const of the column's type - a NULL one for a partition of the NULLs
 * alone - or NULL when they give it none (DESIGN.md §27, "Grouped by a fact
 * column").  Every row of such a partition is then one group of a GROUP BY of
 * the column, which the FK-side join counts as its partition's count, with no
 * index on the column at all.
 *
 * The proof is the one "Clauses the partition bounds imply" makes (§16): the
 * constraint is TRUE for every row the partition holds, so when it strongly
 * implies `var = c` - under the GROUPING's own equality and collation, which
 * is what makes the rows one group and not merely rows of one partition - every
 * row's value is equal to c in the sense the Agg above groups by.  And since
 * c is what the node prints for them, equal has to mean identical, as it does
 * for a key an index stored (§10's value gate, lion_index_can_emit_value()):
 * the grouping equality has to be the type's own, and to imply an identical
 * representation under that collation (lion_type_equalimage()).  numeric's
 * 1.0 and 1.00 are one partition value and print differently; a text bound
 * under a case-insensitive collation holds spellings its rows need not have.
 */
static Const *
lion_leaf_bound_value(List *partqual, const LionDriveInfo *drive)
{
	Var		   *var = drive->var;
	TypeCacheEntry *typentry;
	LionBoundCands cx;
	NullTest   *nt;
	ListCell   *lc;

	if (partqual == NIL || var == NULL || !OidIsValid(drive->eqop))
		return NULL;

	typentry = lookup_type_cache(var->vartype,
								 TYPECACHE_EQ_OPR | TYPECACHE_BTREE_OPFAMILY);
	if (!OidIsValid(typentry->eq_opr) || typentry->eq_opr != drive->eqop ||
		!OidIsValid(typentry->btree_opintype) ||
		!lion_type_equalimage(typentry->btree_opintype, drive->collation))
		return NULL;

	cx.var = var;
	cx.cands = NIL;
	(void) lion_bound_cands_walker((Node *) partqual, &cx);
	foreach(lc, cx.cands)
	{
		Const	   *c = (Const *) lfirst(lc);
		OpExpr	   *eq;

		eq = (OpExpr *) make_opclause(drive->eqop, BOOLOID, false,
									  (Expr *) copyObject(var),
									  (Expr *) copyObject(c),
									  InvalidOid, drive->collation);
		set_opfuncid(eq);
		if (predicate_implied_by(list_make1(eq), partqual, false))
			return (Const *) copyObject(c);
	}

	/* ... or a partition of the NULLs alone: its one group is the NULL one */
	nt = makeNode(NullTest);
	nt->arg = (Expr *) copyObject(var);
	nt->nulltesttype = IS_NULL;
	nt->argisrow = false;
	nt->location = -1;
	if (predicate_implied_by(list_make1(nt), partqual, false))
		return makeNullConst(var->vartype, var->vartypmod, var->varcollid);
	return NULL;
}

/*
 * ... and does it REFUTE `clauses`, an implicit AND over toprel's columns - an
 * arm of an OR restriction - so that no row of the partition makes them TRUE?
 * Every row makes the constraint TRUE, so what core's predicate_refuted_by()
 * calls weak refutation - the clauses TRUE, the predicate FALSE or NULL - is
 * exactly that, and an arm that is never TRUE adds no row to the OR's.  A
 * volatile function or a subquery keeps the arm, as it keeps a clause from
 * being implied.
 */
static bool
lion_leaf_refutes(PlannerInfo *root, RelOptInfo *leaf, RelOptInfo *toprel,
				  List *partqual, List *clauses)
{
	Node	   *leafclauses;

	if (partqual == NIL || leaf == toprel || clauses == NIL)
		return false;
	if (contain_volatile_functions((Node *) clauses) ||
		contain_subplans((Node *) clauses))
		return false;
	leafclauses = adjust_appendrel_attrs_multilevel(root, (Node *) clauses,
													leaf, toprel);
	return predicate_refuted_by((List *) leafclauses, partqual, true);
}

/*
 * A WHERE clause of range table entry rti as EXPLAIN prints a qual on it:
 * for a clause the plan does not carry at all (LION_PRIV_IMPLIED), and so
 * deparsed when the plan is made, against the relation alone.
 */
char *
lion_deparse_rel_clause(PlannerInfo *root, Index rti, Node *clause)
{
	RangeTblEntry *rte = root->simple_rte_array[rti];
	Node	   *copy = copyObject(clause);

	if (rti != 1)
		ChangeVarNodes(copy, (int) rti, 1, 0);
	return deparse_expression(copy,
							  deparse_context_for(get_rel_name(rte->relid),
												  rte->relid),
							  false, false);
}

/*
 * Is `clause`, over partitioned table toprel's columns, implied by the bounds
 * of every live leaf partition under `rel` (toprel itself, or one of its
 * sub-partitioned descendants)?  The leaves are the ones lion_collect_targets()
 * walks.  A clause no posting set can answer is no reason to decline a query
 * whose partitions all imply it: every row they hold satisfies it, and it is
 * left out of the count altogether (DESIGN.md §16, "Clauses the partition
 * bounds imply").
 */
bool
lion_implied_everywhere(PlannerInfo *root, RelOptInfo *toprel, RelOptInfo *rel,
						Node *clause)
{
	RangeTblEntry *rte;
	Relation	relation;
	List	   *partqual;

	check_stack_depth();

	if (rel == NULL || rel->relid == 0 ||
		rel->relid >= (Index) root->simple_rel_array_size)
		return false;
	rte = root->simple_rte_array[rel->relid];
	if (rte == NULL || rte->rtekind != RTE_RELATION)
		return false;

	if (rte->relkind == RELKIND_PARTITIONED_TABLE)
	{
		int			i;

		if (IS_DUMMY_REL(rel))
			return true;
		if (!IS_PARTITIONED_REL(rel))
			return false;
		for (i = 0; i < rel->nparts; i++)
		{
			RelOptInfo *child = rel->part_rels[i];

			if (child == NULL || !bms_is_member(i, rel->live_parts) ||
				IS_DUMMY_REL(child))
				continue;
			if (!lion_implied_everywhere(root, toprel, child, clause))
				return false;
		}
		return true;
	}

	relation = table_open(rte->relid, NoLock);
	partqual = lion_leaf_partition_qual(relation, rel->relid);
	table_close(relation, NoLock);

	return lion_leaf_implies(root, rel, toprel, partqual, clause);
}

/*
 * Is there still a clause that selects rows and carries the §9 interlock among
 * the ones a partition keeps (drop[] says which restrictions it leaves out):
 * a positive clause other than a range collected into memory, or an OR
 * without one among its leaves (lion_leaf_drops())?
 */
static bool
lion_leaf_selects(List *clauseinfos, const bool *drop, const bool *orrange)
{
	ListCell   *lc;

	foreach(lc, clauseinfos)
	{
		LionClauseInfo *ci = (LionClauseInfo *) lfirst(lc);
		int			r = ci->rinfono;

		if ((r >= 0 && drop[r]) || !LION_CLAUSE_IS_POSITIVE(ci->kind))
			continue;
		if (ci->inor ? (r < 0 || !orrange[r]) :
			ci->kind != LION_CLAUSE_RANGESRC)
			return true;
	}
	return false;
}

/*
 * Are all of `rinfos` (RestrictInfos over toprel) implied by the bounds of
 * every partition counted (lion_implied_everywhere())?
 */
bool
lion_rinfos_implied_everywhere(PlannerInfo *root, RelOptInfo *toprel,
							   List *rinfos)
{
	ListCell   *lc;

	foreach(lc, rinfos)
	{
		if (!lion_implied_everywhere(root, toprel, toprel,
									 (Node *) ((RestrictInfo *) lfirst(lc))->clause))
			return false;
	}
	return true;
}

/*
 * Is there a WHERE clause that selects rows and carries the §9 interlock -
 * as lion_leaf_selects() asks it of one partition - that not every partition
 * counted leaves out?  `clauses` is each restriction's clause by position.
 */
bool
lion_pinned_not_implied(PlannerInfo *root, RelOptInfo *toprel,
						List *clauseinfos, List *clauses)
{
	ListCell   *lc;

	foreach(lc, clauseinfos)
	{
		LionClauseInfo *ci = (LionClauseInfo *) lfirst(lc);
		ListCell   *l2;
		bool		pinned = true;

		if (!LION_CLAUSE_IS_POSITIVE(ci->kind) || ci->rinfono < 0)
			continue;
		if (ci->inor)
		{
			/* an OR carries the pin when no leaf of it is a range */
			foreach(l2, clauseinfos)
			{
				LionClauseInfo *other = (LionClauseInfo *) lfirst(l2);

				if (other->rinfono == ci->rinfono &&
					other->kind == LION_CLAUSE_RANGESRC)
					pinned = false;
			}
		}
		else if (ci->kind == LION_CLAUSE_RANGESRC)
			pinned = false;
		if (pinned &&
			!lion_implied_everywhere(root, toprel, toprel,
									 (Node *) list_nth(clauses, ci->rinfono)))
			return true;
	}
	return false;
}

/*
 * Which WHERE clauses one leaf partition leaves out (DESIGN.md §16, "Clauses
 * the partition bounds imply"), given the index idxs[i] it has for each clause
 * i, NULL where it has none.  A restriction the partition's bounds imply is
 * TRUE of every row the partition holds, so it selects nothing there and
 * needs no index: it is left out of the partition's count - all of it, every
 * leaf of an OR together, since the proof is about the whole restriction.
 *
 *	- A restriction the partition has no index for HAS to be left out, and
 *	  one its bounds do not imply then declines the query, as it always did.
 *	- One it has indexes for is left out too, which saves its lookups and the
 *	  AND with a set that holds every row - but not where the count would be
 *	  left with nothing that selects rows (below).
 *	- Never left out: a range that bounds the walk driving the count (§28),
 *	  which walks the same entries with it or without it and whose index is
 *	  the driver's, and a pinned column whose value the target list prints,
 *	  which comes out of the index's stored key (§10's value gate).
 *
 * A count that nothing but the WHERE clauses drives - no GROUP BY, no sum
 * over a column's entries, no fk key - needs one of them to select rows, and
 * one that carries the §9 interlock: a positive clause, or an OR of them,
 * other than a range collected into memory (§32).  The planner asked the same
 * of the query as a whole (pinnedsrc); here it is asked of what the partition
 * keeps.  A partition that has to leave out every such clause declines the
 * query, as it did before.
 *
 * An OR restriction its bounds do not imply may still be one they narrow
 * (DESIGN.md §16, "OR arms the partition bounds refute"): an arm they refute
 * is never TRUE of a row the partition holds and adds nothing to the union,
 * so it is left out, index or none; and in an arm they do not refute, a leaf
 * they imply is TRUE of every row and is left out of the arm's AND - a range
 * excepted, whose bounds make one source together (§32).  An arm all of whose
 * leaves go that way is TRUE of every row itself, and so is the OR, which is
 * then left out whole.  The executor tells the three apart by what is left
 * of each arm (lion_locate_or()).  And an OR whose every arm is refuted is
 * never TRUE in the partition at all: *emptyp says so, and the partition has
 * nothing to count.
 *
 * On success *droppedp is NULL when nothing is left out, and otherwise one
 * flag per clause; false declines the query.
 */
static bool
lion_leaf_drops(PlannerInfo *root, RelOptInfo *leaf, List *partqual,
				const LionImply *imply, List *clauseinfos,
				IndexOptInfo **idxs, bool **droppedp, int *ndroppedp,
				bool *emptyp)
{
	int			nr = list_length(imply->clauses);
	int			nclause = list_length(clauseinfos);
	int8	   *implied;		/* -1 not asked yet, 0 no, 1 yes */
	bool	   *present;		/* some clause came of the restriction */
	bool	   *need;			/* ... and this partition has no index for it */
	bool	   *keep;			/* ... and it may not be left out */
	bool	   *orrange;		/* an OR with a range leaf it keeps: carries no
								 * pin */
	bool	   *drop;
	bool	   *armdrop;		/* an OR's leaf, left out with its arm or alone */
	bool	   *dropped;
	int			ndropped = 0;
	int			i;
	int			r;
	ListCell   *lc;

	*droppedp = NULL;
	*ndroppedp = 0;
	*emptyp = false;
	if (partqual == NIL || nr == 0)
		return true;

	implied = (int8 *) palloc(sizeof(int8) * nr);
	memset(implied, -1, sizeof(int8) * nr);
	present = (bool *) palloc0(sizeof(bool) * nr);
	need = (bool *) palloc0(sizeof(bool) * nr);
	keep = (bool *) palloc0(sizeof(bool) * nr);
	orrange = (bool *) palloc0(sizeof(bool) * nr);
	drop = (bool *) palloc0(sizeof(bool) * nr);
	armdrop = (bool *) palloc0(sizeof(bool) * Max(nclause, 1));

#define LION_IMPLIED(r) \
	(implied[r] < 0 ? \
	 (implied[r] = lion_leaf_implies(root, leaf, imply->toprel, partqual, \
									 (Node *) list_nth(imply->clauses, r)) ? 1 : 0) : \
	 implied[r])

	/* The arms of each OR the bounds do not imply as a whole. */
	foreach(lc, imply->ors)
	{
		List	   *one = (List *) lfirst(lc);
		int			first = linitial_int(one);
		int			narms = lsecond_int(one);
		int			at = first;
		int			nkept = 0;
		bool		whole = false;
		int			a;
		int			j;

		r = ((LionClauseInfo *) list_nth(clauseinfos, first))->rinfono;
		if (r < 0 || r >= nr || LION_IMPLIED(r))
			continue;
		for (a = 0; a < narms; a++)
		{
			int			len = list_nth_int(one, 2 + a);
			List	   *arm = NIL;
			int			nleft = 0;

			for (j = at; j < at + len; j++)
				arm = lappend(arm, list_nth(imply->leafclauses, j));
			if (lion_leaf_refutes(root, leaf, imply->toprel, partqual, arm))
			{
				for (j = at; j < at + len; j++)
					armdrop[j] = true;
			}
			else
			{
				nkept++;
				for (j = at; j < at + len; j++)
				{
					LionClauseInfo *ci = (LionClauseInfo *) list_nth(clauseinfos, j);

					if (ci->kind != LION_CLAUSE_RANGESRC &&
						lion_leaf_implies(root, leaf, imply->toprel, partqual,
										  (Node *) list_nth(imply->leafclauses, j)))
						armdrop[j] = true;
					else
						nleft++;
				}
				if (nleft == 0)
					whole = true;
			}
			list_free(arm);
			at += len;
		}

		if (nkept == 0)
		{
			/* never TRUE here: the partition has no row to count */
			*emptyp = true;
			return true;
		}
		if (whole)
		{
			/* an arm TRUE of every row makes the OR so: it goes whole */
			implied[r] = 1;
			for (j = first; j < at; j++)
				armdrop[j] = false;
		}
	}

	i = 0;
	foreach(lc, clauseinfos)
	{
		LionClauseInfo *ci = (LionClauseInfo *) lfirst(lc);

		r = ci->rinfono;
		if (r >= 0 && r < nr)
		{
			present[r] = true;
			if (idxs[i] == NULL && !armdrop[i])
				need[r] = true;
			if (ci->kind == LION_CLAUSE_RANGE || (ci->valueout && !ci->inor))
				keep[r] = true;
			if (ci->inor && ci->kind == LION_CLAUSE_RANGESRC && !armdrop[i])
				orrange[r] = true;
		}
		i++;
	}

	/* What the partition cannot answer, it must be able to leave out. */
	for (r = 0; r < nr; r++)
	{
		if (!need[r])
			continue;
		if (keep[r] || !LION_IMPLIED(r))
			return false;
		drop[r] = true;
	}
	if (!imply->driven && !lion_leaf_selects(clauseinfos, drop, orrange))
		return false;

	/* ... and what it can, it leaves out where something else selects rows */
	for (r = 0; r < nr; r++)
	{
		if (!present[r] || need[r] || keep[r] || !LION_IMPLIED(r))
			continue;
		drop[r] = true;
		if (!imply->driven && !lion_leaf_selects(clauseinfos, drop, orrange))
			drop[r] = false;
	}
#undef LION_IMPLIED

	dropped = (bool *) palloc0(sizeof(bool) * Max(nclause, 1));
	i = 0;
	foreach(lc, clauseinfos)
	{
		LionClauseInfo *ci = (LionClauseInfo *) lfirst(lc);

		if ((ci->rinfono >= 0 && ci->rinfono < nr && drop[ci->rinfono]) ||
			armdrop[i])
		{
			dropped[i] = true;
			ndropped++;
		}
		i++;
	}
	if (ndropped == 0)
	{
		pfree(dropped);
		return true;
	}
	*droppedp = dropped;
	*ndroppedp = ndropped;
	return true;
}

bool
lion_collect_targets(PlannerInfo *root, RelOptInfo *rel,
					const LionDriveInfo *drive, int ndrive, List *whereattnos,
					List *clauseinfos, const LionImply *imply, List **targets)
{
	RangeTblEntry *rte;
	LionCountTarget *t;
	List	   *partqual = NIL;
	int			nclause = list_length(clauseinfos);
	IndexOptInfo **idxs;
	AttrNumber *cols;
	ListCell   *l1;
	ListCell   *l2;
	int			d;
	int			i;

	/* Sub-partitioning nests, exactly as expand_partitioned_rtentry() does. */
	check_stack_depth();

	if (rel == NULL || rel->relid == 0 ||
		rel->relid >= (Index) root->simple_rel_array_size)
		return false;
	rte = root->simple_rte_array[rel->relid];
	if (rte == NULL || rte->rtekind != RTE_RELATION)
		return false;
	if (rte->securityQuals != NIL || rte->tablesample != NULL)
		return false;

	if (rte->relkind == RELKIND_PARTITIONED_TABLE)
	{
		/* Pruned down to nothing: no targets, but no reason to bail either. */
		if (IS_DUMMY_REL(rel))
			return true;
		if (!IS_PARTITIONED_REL(rel))
			return false;

		for (i = 0; i < rel->nparts; i++)
		{
			RelOptInfo *child = rel->part_rels[i];
			LionDriveInfo cdrive[LION_MAX_GROUPCOLS];
			List	   *cattnos = NIL;

			if (child == NULL || !bms_is_member(i, rel->live_parts))
				continue;		/* pruned at plan time */
			if (IS_DUMMY_REL(child))
				continue;		/* provably empty: it counts nothing */

			for (d = 0; d < ndrive; d++)
			{
				cdrive[d] = drive[d];
				if (drive[d].attno == 0)
					continue;
				cdrive[d].var = lion_child_var(root, child->relid,
											  drive[d].attno);
				if (cdrive[d].var == NULL)
					return false;
				cdrive[d].attno = cdrive[d].var->varattno;
			}
			foreach(l1, whereattnos)
			{
				AttrNumber	ca = lion_child_attno(root, child->relid,
												 (AttrNumber) lfirst_int(l1));

				if (ca == 0)
					return false;
				cattnos = lappend_int(cattnos, (int) ca);
			}

			if (!lion_collect_targets(root, child, cdrive, ndrive, cattnos,
									 clauseinfos, imply, targets))
				return false;
		}
		return true;
	}

	/*
	 * A leaf.  Anything whose rows do not live in a local heap this backend
	 * can read - a foreign table above all - is out.  A materialized view
	 * cannot be a partition; it is accepted here because the single-table
	 * path goes through this function too.
	 */
	if (rte->relkind != RELKIND_RELATION && rte->relkind != RELKIND_MATVIEW)
		return false;
	if (rel->indexlist == NIL)
		return false;

	/*
	 * Nor a table of another table AM (lion_table_am_supported()): the count
	 * reads the heap's visibility map.  ambuild refuses to put a lion index
	 * there, so this only declines what got past it; partitions may each have
	 * their own table AM, and one such leaf declines the whole parent.
	 */
	{
		Relation	relation = table_open(rte->relid, NoLock);
		bool		supported = lion_table_am_supported(relation);
		bool		bound = false;

		for (d = 0; d < ndrive; d++)
			bound |= (drive[d].attno != 0 && drive[d].bound);

		/*
		 * ... and the bounds that may imply WHERE clauses (below), or give a
		 * driving column its one value
		 */
		if (supported && (imply != NULL || bound))
			partqual = lion_leaf_partition_qual(relation, rel->relid);
		table_close(relation, NoLock);
		if (!supported)
			return false;
	}

	t = (LionCountTarget *) palloc0(sizeof(LionCountTarget));
	t->rel = rel;
	t->heapoid = rte->relid;

	/*
	 * The driving indexes - a GROUP BY column's (two of them for the nested
	 * loop of DESIGN.md §20), or the one DESIGN.md §14 sums over - must be
	 * scalar ones: their entries have to be the column's values, one per row.
	 */
	for (d = 0; d < ndrive; d++)
	{
		t->drivevar[d] = drive[d].var;
		if (drive[d].attno == 0)
			continue;

		/*
		 * A column the leaf's bounds give one value to groups nothing there:
		 * the leaf is one group, of that value, and needs no index on it
		 * (DESIGN.md §27, "Grouped by a fact column").
		 */
		if (drive[d].bound)
		{
			t->driveconst[d] = lion_leaf_bound_value(partqual, &drive[d]);
			if (t->driveconst[d] != NULL)
				continue;
		}

		t->drivecol[d] = 1;
		t->driveidx[d] = lion_find_roaring_index(rel, drive[d].attno, false,
											   &t->drivecol[d]);
		if (t->driveidx[d] == NULL)
			return false;
		/* Grouping under one collation, index built under another: no. */
		if (OidIsValid(drive[d].collation) &&
			t->driveidx[d]->indexcollations[t->drivecol[d] - 1] !=
			drive[d].collation)
			return false;

		/*
		 * Grouping asks for the groups of ONE equality relation, and the
		 * index's entries are the classes of its own opclass equality.  A
		 * matching collation does not make those the same relation: an
		 * opclass may define a coarser equality on the same type (a text
		 * opclass over lower(), say), and then its entries are already
		 * merged groups that no aggregation above the node can take
		 * apart.  So strategy 1 of this index's opfamily, on its own key
		 * type, has to be the very operator the planner chose for the
		 * grouping column (the 2026-09-20 review, finding 3).  This also
		 * covers the count(col) cases of DESIGN.md §14 that read the group
		 * column's entries (a real group is count(*), the NULL group is 0),
		 * because they are only reached through a grouping index.
		 */
		if (OidIsValid(drive[d].eqop) &&
			lion_index_equality_op(t->driveidx[d], t->drivecol[d]) !=
			drive[d].eqop)
			return false;

		/*
		 * Printing the group key means printing a key this index stored, so
		 * it has to be a representation the rows really have (finding 4).
		 */
		if (drive[d].valueout &&
			!lion_index_can_emit_value(t->driveidx[d], t->drivecol[d]))
			return false;
	}

	idxs = (IndexOptInfo **) palloc0(sizeof(IndexOptInfo *) * Max(nclause, 1));
	cols = (AttrNumber *) palloc0(sizeof(AttrNumber) * Max(nclause, 1));
	i = 0;
	forboth(l1, whereattnos, l2, clauseinfos)
	{
		LionClauseInfo *ci = (LionClauseInfo *) lfirst(l2);
		AttrNumber	col = 1;
		IndexOptInfo *idx = lion_match_index(rel, (AttrNumber) lfirst_int(l1),
											ci->kind, ci->opno, ci->cmptype,
											ci->strategy, ci->extractquery,
											ci->collation, ci->positions,
											&col);

		/*
		 * Same rule for a pinned column whose value the output prints: an
		 * index that cannot print it is no index for the clause.
		 */
		if (idx != NULL && ci->valueout &&
			!lion_index_can_emit_value(idx, col))
			idx = NULL;

		idxs[i] = idx;
		cols[i] = (idx != NULL) ? col : 0;
		i++;
	}

	/*
	 * A clause this partition's bounds imply needs no index here, and is left
	 * out of its count (DESIGN.md §16, "Clauses the partition bounds
	 * imply"); every other clause needs one, as it always did.
	 */
	if (imply != NULL)
	{
		bool		empty;

		if (!lion_leaf_drops(root, rel, partqual, imply, clauseinfos, idxs,
							 &t->dropped, &t->ndropped, &empty))
			return false;
		if (empty)
			return true;		/* the WHERE clauses select nothing here */
	}
	for (i = 0; i < nclause; i++)
	{
		bool		dropped = (t->dropped != NULL && t->dropped[i]);

		if (idxs[i] == NULL && !dropped)
			return false;
		t->whereidx = lappend(t->whereidx, dropped ? NULL : idxs[i]);
		t->wherecol = lappend_int(t->wherecol, dropped ? 0 : (int) cols[i]);
	}

	*targets = lappend(*targets, t);
	return true;
}

/*
 * The columns of rel declared NOT NULL: RelOptInfo.notnullattnums, which 17
 * added.  On 16 it is read from the relation the same way 17's
 * get_relation_info() fills it in, including leaving it empty for an
 * inheritance parent that is not partitioned, whose children may disagree.
 */
Bitmapset *
lion_notnullattnums(PlannerInfo *root, RelOptInfo *rel)
{
#if PG_VERSION_NUM >= 170000
	return rel->notnullattnums;
#else
	RangeTblEntry *rte;
	Relation	relation;
	Bitmapset  *result = NULL;

	if (rel->reloptkind != RELOPT_BASEREL &&
		rel->reloptkind != RELOPT_OTHER_MEMBER_REL)
		return NULL;
	rte = planner_rt_fetch(rel->relid, root);
	if (rte->rtekind != RTE_RELATION)
		return NULL;

	relation = table_open(rte->relid, NoLock);
	if (!rte->inh || relation->rd_rel->relkind == RELKIND_PARTITIONED_TABLE)
	{
		for (int i = 0; i < relation->rd_att->natts; i++)
		{
			Form_pg_attribute attr = TupleDescAttr(relation->rd_att, i);

			if (attr->attnotnull && !attr->attisdropped)
				result = bms_add_member(result, attr->attnum);
		}
	}
	table_close(relation, NoLock);

	return result;
#endif
}
