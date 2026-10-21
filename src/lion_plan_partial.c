/*-------------------------------------------------------------------------
 *
 * lion_plan_partial.c
 *		Partial lion indexes in the count pushdown (DESIGN.md §45): the
 *		partial index a count may read, the restrictions its predicate lets
 *		the count leave out, and the test that every row the count can reach
 *		is one the predicate holds for.
 *
 * A count path is planned once with whole indexes only, as it always was,
 * and then once more for each partial index whose predicate the query's
 * restrictions imply (lion_try_count_path()): inside lion_partial_enter() /
 * lion_partial_leave(), every index lookup of the planner - the drivers'
 * (lion_find_roaring_index()), the clauses' (lion_match_index()) - tries
 * the scope's partial index first, and the restrictions the predicate
 * implies are left out of the WHERE (lion_partial_drops()), as core leaves
 * them out of a partial index scan's indrestrictinfo.  add_path() keeps the
 * cheaper of the plans.
 *
 * Why that is exact only when the count's rows lie inside the predicate,
 * and how lion_partial_anchored() makes sure they do, is DESIGN.md §45.
 * The scope is this file's, as the expression columns' are
 * lion_plan_match.c's: one count path's, entered after the relation is
 * known and left on every way out, errors included.
 *
 * Part of the LionCount custom scan: lion_customscan.h describes the node
 * and declares what its files share.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "lion_customscan.h"

/*
 * The partial indexes of the relations a count reads, whose predicates the
 * query implies: for each relation - the table, or each live leaf partition
 * - its candidates, the smallest first.
 */
struct LionPartialCands
{
	List	   *rels;			/* RelOptInfo *, every relation counted */
	List	   *cands;			/* a List of IndexOptInfo * for each */
	int			max;			/* the most candidates any relation has */
};

/*
 * The scope one attempt plans in: the partial index each relation reads,
 * or NULL where it has none at this rank, the restrictions left out, and the
 * partial indexes the count found it reads (lion_partial_anchored()).
 */
typedef struct LionPartialScope
{
	List	   *rels;			/* RelOptInfo * */
	List	   *idxs;			/* IndexOptInfo *, or NULL, for each */
	List	   *dropped;		/* String: the restrictions left out */
	List	   *used;			/* OidList: the partial indexes read */
} LionPartialScope;

static LionPartialScope *lion_partial_scope = NULL;

/*
 * The live leaf relations under rel (rel itself for a plain table), in the
 * order lion_collect_targets() visits them, or false for one that is no
 * plain relation: such a count is declined there anyway.
 */
static bool
lion_partial_leaves(PlannerInfo *root, RelOptInfo *rel, List **leaves)
{
	RangeTblEntry *rte;
	int			i;

	check_stack_depth();

	if (rel == NULL || rel->relid == 0 ||
		rel->relid >= (Index) root->simple_rel_array_size)
		return false;
	rte = root->simple_rte_array[rel->relid];
	if (rte == NULL || rte->rtekind != RTE_RELATION)
		return false;
	if (rte->relkind != RELKIND_PARTITIONED_TABLE)
	{
		*leaves = lappend(*leaves, rel);
		return true;
	}
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
		if (!lion_partial_leaves(root, child, leaves))
			return false;
	}
	return true;
}

/*
 * The partial lion indexes of one relation whose predicates its restrictions
 * imply - with, for a partition, its partition constraint, which every row
 * of it satisfies - the smallest first.  The proof is core's own
 * (check_index_predicates()), from the restrictions alone: join clauses
 * prove nothing about the rows a count reads.  Hypothetical indexes have no
 * entries to read.
 */
static List *
lion_partial_rel_cands(PlannerInfo *root, RelOptInfo *rel, RelOptInfo *toprel)
{
	Oid			amoid = lion_get_am_oid();
	List	   *clauses = NIL;
	List	   *cands = NIL;
	bool		have = false;
	ListCell   *lc;

	foreach(lc, rel->indexlist)
	{
		IndexOptInfo *idx = (IndexOptInfo *) lfirst(lc);

		if (idx->relam == amoid && !idx->hypothetical && idx->indpred != NIL)
			have = true;
	}
	if (!have)
		return NIL;

	clauses = list_copy(rel->baserestrictinfo);
	if (rel != toprel)
	{
		RangeTblEntry *rte = root->simple_rte_array[rel->relid];
		Relation	relation = table_open(rte->relid, NoLock);

		clauses = list_concat(clauses,
							  lion_leaf_partition_qual(relation, rel->relid));
		table_close(relation, NoLock);
	}

	foreach(lc, rel->indexlist)
	{
		IndexOptInfo *idx = (IndexOptInfo *) lfirst(lc);
		ListCell   *lp;
		int			pos = 0;

		if (idx->relam != amoid || idx->hypothetical || idx->indpred == NIL)
			continue;
		if (!predicate_implied_by(idx->indpred, clauses, false))
			continue;

		/* by size: the fewer rows, the smaller every set the count reads */
		foreach(lp, cands)
		{
			if (((IndexOptInfo *) lfirst(lp))->tuples > idx->tuples)
				break;
			pos++;
		}
		cands = list_insert_nth(cands, pos, idx);
	}
	list_free(clauses);
	return cands;
}

/*
 * The candidates of every relation rel counts, or NULL when none has one
 * (or when the relations are not ones a count reads).
 */
LionPartialCands *
lion_partial_find(PlannerInfo *root, RelOptInfo *rel)
{
	LionPartialCands *pc;
	List	   *leaves = NIL;
	ListCell   *lc;

	if (!lion_partial_leaves(root, rel, &leaves) || leaves == NIL)
		return NULL;

	pc = (LionPartialCands *) palloc0(sizeof(LionPartialCands));
	foreach(lc, leaves)
	{
		RelOptInfo *leaf = (RelOptInfo *) lfirst(lc);
		List	   *cands = lion_partial_rel_cands(root, leaf, rel);

		pc->rels = lappend(pc->rels, leaf);
		pc->cands = lappend(pc->cands, cands);
		pc->max = Max(pc->max, list_length(cands));
	}
	if (pc->max == 0)
		return NULL;
	return pc;
}

int
lion_partial_ncands(const LionPartialCands *pc)
{
	return (pc != NULL) ? pc->max : 0;
}

/*
 * Plan with each relation's rank'th candidate (0 the smallest) until
 * lion_partial_leave() is handed back *saved; false, and nothing entered,
 * when no relation has one at that rank.
 */
bool
lion_partial_enter(const LionPartialCands *pc, int rank, void **saved)
{
	LionPartialScope *s;
	ListCell   *lc;
	bool		any = false;

	if (pc == NULL)
		return false;
	s = (LionPartialScope *) palloc0(sizeof(LionPartialScope));
	s->rels = pc->rels;
	foreach(lc, pc->cands)
	{
		List	   *cands = (List *) lfirst(lc);
		IndexOptInfo *idx = (rank < list_length(cands)) ?
			(IndexOptInfo *) list_nth(cands, rank) : NULL;

		s->idxs = lappend(s->idxs, idx);
		any |= (idx != NULL);
	}
	if (!any)
	{
		pfree(s);
		return false;
	}
	*saved = lion_partial_scope;
	lion_partial_scope = s;
	return true;
}

void
lion_partial_leave(void *saved)
{
	lion_partial_scope = (LionPartialScope *) saved;
}

/* rel's partial index in the scope, or NULL (none, or no scope at all) */
IndexOptInfo *
lion_partial_index_of(RelOptInfo *rel)
{
	LionPartialScope *s = lion_partial_scope;
	ListCell   *l1;
	ListCell   *l2;

	if (s == NULL || rel == NULL)
		return NULL;
	forboth(l1, s->rels, l2, s->idxs)
	{
		if ((RelOptInfo *) lfirst(l1) == rel)
			return (IndexOptInfo *) lfirst(l2);
	}
	return NULL;
}

bool
lion_partial_active(void)
{
	return lion_partial_scope != NULL;
}

/*
 * May restriction `clause`, over toprel's columns, be left out of the
 * count?  When the predicate of the partial index every relation counted
 * reads implies it - core's own test for leaving a restriction out of a
 * partial index scan (check_index_predicates()), which also keeps every
 * clause with a mutable function - mapped onto each partition's columns.
 * Inside the predicate's rows the clause is TRUE, and the count is held to
 * those rows (lion_partial_anchored()).  A clause with a subquery is never
 * left out: the query asked for it per row.
 */
bool
lion_partial_drops(PlannerInfo *root, RelOptInfo *toprel, Node *clause)
{
	LionPartialScope *s = lion_partial_scope;
	ListCell   *l1;
	ListCell   *l2;

	if (s == NULL || s->rels == NIL)
		return false;
	if (contain_mutable_functions(clause) || contain_subplans(clause))
		return false;
	forboth(l1, s->rels, l2, s->idxs)
	{
		RelOptInfo *rel = (RelOptInfo *) lfirst(l1);
		IndexOptInfo *idx = (IndexOptInfo *) lfirst(l2);
		Node	   *c = clause;

		if (idx == NULL)
			return false;
		if (rel != toprel)
			c = adjust_appendrel_attrs_multilevel(root, clause, rel, toprel);
		if (!predicate_implied_by(list_make1(c), idx->indpred, false))
			return false;
	}
	return true;
}

/*
 * A restriction left out, as EXPLAIN prints it ("Implied by Index
 * Predicate"): once, however many times the WHERE is analyzed in the scope.
 */
void
lion_partial_note_drop(char *text)
{
	LionPartialScope *s = lion_partial_scope;
	ListCell   *lc;

	Assert(s != NULL);
	foreach(lc, s->dropped)
		if (strcmp(strVal(lfirst(lc)), text) == 0)
			return;
	s->dropped = lappend(s->dropped, makeString(text));
}

/* The restrictions left out, String nodes, or NIL */
List *
lion_partial_dropped(void)
{
	return (lion_partial_scope != NULL) ? lion_partial_scope->dropped : NIL;
}

/* The partial indexes the count reads (lion_partial_anchored()), or NIL */
List *
lion_partial_used(void)
{
	return (lion_partial_scope != NULL) ? lion_partial_scope->used : NIL;
}

/* A clause of this kind selects only rows its own set holds. */
static bool
lion_partial_anchor_kind(int kind)
{
	return kind == LION_CLAUSE_EQ || kind == LION_CLAUSE_ARRAY ||
		kind == LION_CLAUSE_NULL || kind == LION_CLAUSE_RANGESRC;
}

/* Is clause i one of run r's: the WHERE's (filter 0), or filter r's? */
static bool
lion_partial_in_run(const LionClauseInfo *ci, int r)
{
	return ci->filter == 0 || ci->filter == r;
}

/*
 * Does target t read partial index p for a clause of run r?
 */
static bool
lion_partial_run_reads(const LionCountTarget *t, List *clauseinfos,
					   IndexOptInfo *p, int r)
{
	ListCell   *l1;
	ListCell   *l2;

	forboth(l1, clauseinfos, l2, t->whereidx)
	{
		if ((IndexOptInfo *) lfirst(l2) == p &&
			lion_partial_in_run((LionClauseInfo *) lfirst(l1), r))
			return true;
	}
	return false;
}

/*
 * Is every row run r of target t counts one of partial index p's, by its
 * WHERE alone: a positive clause of the run, outside any OR, read from p -
 * an equality, a list, `IS NULL`, a range taken as a source, whose set holds
 * p's rows only and is ANDed into every count of the run - or an OR each of
 * whose arms the partition keeps has such a leaf?  Not a multi-key clause,
 * whose query may turn out to narrow nothing (DESIGN.md §17), nor anything
 * negated, nor an OR with a complement arm, which counts the rows outside a
 * set (§35).  A clause the partition's bounds leave out (§16) reads no
 * index there and holds nothing.
 */
static bool
lion_partial_run_held(const LionCountTarget *t, List *clauseinfos, List *ors,
					  IndexOptInfo *p, int r)
{
	ListCell   *l1;
	ListCell   *l2;

	forboth(l1, clauseinfos, l2, t->whereidx)
	{
		LionClauseInfo *ci = (LionClauseInfo *) lfirst(l1);

		if ((IndexOptInfo *) lfirst(l2) == p && !ci->inor && !ci->incompl &&
			lion_partial_in_run(ci, r) && lion_partial_anchor_kind(ci->kind))
			return true;
	}

	foreach(l1, ors)
	{
		List	   *one = (List *) lfirst(l1);
		int			first = linitial_int(one);
		int			narms = lsecond_int(one);
		int			at = first;
		bool		ok = true;
		bool		anykept = false;
		int			a;

		if (!lion_partial_in_run((LionClauseInfo *) list_nth(clauseinfos,
															 first), r))
			continue;
		for (a = 0; a < narms && ok; a++)
		{
			int			len = list_nth_int(one, 2 + a);
			bool		armkept = false;
			bool		armheld = false;
			int			j;

			for (j = at; j < at + len; j++)
			{
				LionClauseInfo *cj = (LionClauseInfo *) list_nth(clauseinfos, j);
				IndexOptInfo *ij = (IndexOptInfo *) list_nth(t->whereidx, j);

				if (cj->incompl)
					ok = false;
				if (ij == NULL)
					continue;	/* refuted, or implied, by the bounds */
				armkept = true;
				if (ij == p && lion_partial_anchor_kind(cj->kind))
					armheld = true;
			}
			if (armkept && !armheld)
				ok = false;
			anykept |= armkept;
			at += len;
		}
		if (ok && anykept)
			return true;
	}
	return false;
}

/*
 * Does run r of target t have a source that selects rows by itself, so that
 * a sum over every row counts it as an intersection and not by the driving
 * walk (lion_run_has_plain_source(), lion_count_runs())?  Asked as the
 * executor asks it, of the clauses the run keeps: an equality, a list,
 * `IS NULL` or the rows of an array column's reserved entries (§17), or an
 * OR with no range among its leaves and no complement arm (DESIGN.md §35,
 * "Complements in an OR"), which selects nothing by itself.
 */
static bool
lion_partial_run_plain(const LionCountTarget *t, List *clauseinfos, List *ors,
					   int r)
{
	ListCell   *l1;
	ListCell   *l2;

	forboth(l1, clauseinfos, l2, t->whereidx)
	{
		LionClauseInfo *ci = (LionClauseInfo *) lfirst(l1);

		if (lfirst(l2) != NULL && !ci->inor && lion_partial_in_run(ci, r) &&
			(ci->kind == LION_CLAUSE_EQ || ci->kind == LION_CLAUSE_ARRAY ||
			 ci->kind == LION_CLAUSE_NULL || ci->kind == LION_CLAUSE_RESERVED))
			return true;
	}
	foreach(l1, ors)
	{
		List	   *one = (List *) lfirst(l1);
		int			first = linitial_int(one);
		int			narms = lsecond_int(one);
		int			nleaves = 0;
		bool		range = false;
		bool		kept = false;
		int			a;
		int			j;

		if (!lion_partial_in_run((LionClauseInfo *) list_nth(clauseinfos,
															 first), r) ||
			((LionClauseInfo *) list_nth(clauseinfos, first))->incompl)
			continue;
		for (a = 0; a < narms; a++)
			nleaves += list_nth_int(one, 2 + a);
		for (j = first; j < first + nleaves; j++)
		{
			if (list_nth(t->whereidx, j) == NULL)
				continue;
			kept = true;
			if (((LionClauseInfo *) list_nth(clauseinfos, j))->kind ==
				LION_CLAUSE_RANGESRC)
				range = true;
		}
		if (kept && !range)
			return true;
	}
	return false;
}

/*
 * Is the count held to the rows of the partial indexes it reads (DESIGN.md
 * §45, "Held to the predicate's rows")?  Every set read from partial index p
 * agrees with its clause on the rows p holds, and every restriction left out
 * is TRUE there, so the count is exact exactly when no row it counts lies
 * outside them.  For each relation counted and each of its runs (the
 * WHERE's, and each filtered count's, DESIGN.md §10) that reads p, or
 * leaves a restriction out, that is so when p's entries drive the run - a
 * GROUP BY column, the column a count(DISTINCT) or a sum over the entries
 * walks, the decoded walk's columns, the FK-side join's fact group, each a
 * driving slot of t below `ndrive` - or when its WHERE holds it there
 * (lion_partial_run_held()).  A sum over every row (`allrows`) drives a
 * filtered count's run only when nothing in it selects rows; one that has a
 * source of its own is counted by intersection, and needs its own hold.
 *
 * Records the partial indexes the count reads for the plan, and is false -
 * the attempt declined - when it reads none and leaves nothing out: the
 * plan would be the one made with whole indexes.
 */
bool
lion_partial_anchored(List *targets, List *clauseinfos, List *ors,
					  int ndrive, bool allrows, int nfilter)
{
	LionPartialScope *s = lion_partial_scope;
	List	   *used = NIL;
	ListCell   *lt;

	if (s == NULL)
		return true;

	foreach(lt, targets)
	{
		LionCountTarget *t = (LionCountTarget *) lfirst(lt);
		IndexOptInfo *p = lion_partial_index_of(t->rel);
		bool		drives = false;
		int			d;
		int			r;

		if (p == NULL)
		{
			/* nothing is left out unless every relation has one */
			Assert(s->dropped == NIL);
			continue;
		}
		for (d = 0; d < ndrive && d < LION_MAX_GROUPCOLS; d++)
		{
			if (t->driveconst[d] == NULL && t->driveidx[d] == p)
				drives = true;
		}
		for (r = 0; r <= nfilter; r++)
		{
			bool		reads = drives ||
				lion_partial_run_reads(t, clauseinfos, p, r);

			if (!reads && s->dropped == NIL)
				continue;
			if (!lion_partial_run_held(t, clauseinfos, ors, p, r) &&
				!(drives &&
				  !(allrows && r > 0 &&
					lion_partial_run_plain(t, clauseinfos, ors, r))))
				return false;
			if (reads)
				used = list_append_unique_oid(used, p->indexoid);
		}
	}
	if (used == NIL && s->dropped == NIL)
		return false;
	s->used = used;
	return true;
}
