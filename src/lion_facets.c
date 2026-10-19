/*-------------------------------------------------------------------------
 *
 * lion_facets.c
 *		lion_index_facets(): the rows under each key of one lion index
 *		column, counted exactly, under an optional filter answered from lion
 *		indexes alone - the facet counts of a search page (DESIGN.md §7,
 *		"Facet counts").
 *
 * For a multi-key column (DESIGN.md §17) a key is an extracted element - a
 * tag of a text[], a lexeme of a tsvector, a path key of jsonb_contains_ops -
 * and its count is the number of rows whose value holds it:
 *
 *		SELECT e, count(DISTINCT t.ctid) FROM t, unnest(t.col) e
 *		 WHERE <filter> AND e IS NOT NULL GROUP BY e
 *		 ORDER BY 2 DESC, e LIMIT k
 *
 * and for a scalar column it is `SELECT col, count(*) FROM t WHERE <filter>
 * GROUP BY col ORDER BY 2 DESC, col LIMIT k`, the NULL group included.  The
 * counts are those of the rows the caller's snapshot sees, made by the count
 * engine against the visibility map and the heap (DESIGN.md §9), exactly as
 * the count pushdown makes them.
 *
 * THE FILTER is a WHERE clause over the table, given as text, and every part
 * of it has to be a posting set: an AND and OR of `col = value`, `col = ANY
 * (array)` and `col IS NULL` on a column some lion index has as a scalar key
 * column, and of the multi-key operators (`@>`, `&&`, `@@`, `?`, ...) on one
 * a lion index has as a multi-key column, each answered exactly by its keys.
 * Anything else - a range, `<>`, NOT, a tsquery with a phrase or a negation,
 * a clause on a column with no lion index - is an ERROR: the filter is never
 * evaluated against the rows.  The values are evaluated once, as constants, and may not refer to the table.  The filter's sets are
 * collected ONCE into a private copy (lion_sources_collect()), which every
 * key's count is ANDed with, as the GROUP BY pushdown does with its WHERE.
 *
 * THE TOP k.  Every entry records `ntids`, the TIDs its posting set holds,
 * and every row the snapshot sees under a key has its TID there (DESIGN.md
 * §36, "The bound"), so ntids bounds the key's count under any filter.  With
 * a k, one walk of the column's entry headers keeps the candidates with the
 * largest bounds, as lion_topk_run() does, and they are counted largest
 * bound first until the k-th count is LARGER than the next bound: no key not
 * yet counted can then reach it, or tie with it - ties are broken by the key,
 * so every key tied with the k-th must have been counted.  When the
 * candidates run out first, the column is walked again and every entry
 * counted whose bound is not below the k-th count so far.
 *
 * A SELECTIVE FILTER keeps fewer rows than counting the keys would cost:
 * then the rows are fetched from the heap by their TIDs in the filter's copy,
 * under the snapshot, and their keys extracted and counted in a hash table -
 * the same rows, so the same counts.  Only for a plain column of a
 * non-partial index, whose rows' values are the index's keys.
 *
 * Nothing here is reached by the planner: the function is called by name,
 * and goes through the reader contract of every such function
 * (lion_count_open_indexes(), lion_reader_vet()).
 *
 * Part of the count engine: lion_count.h is its interface, and
 * lion_count_int.h declares what its files share.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "lion_count_int.h"

#include "access/skey.h"
#include "executor/executor.h"
#include "parser/parse_clause.h"
#include "parser/parse_collate.h"
#include "parser/parse_node.h"
#include "parser/parse_relation.h"
#include "parser/parser.h"
#include "rewrite/rewriteManip.h"
#include "utils/ruleutils.h"
#include "utils/tuplestore.h"
#include "utils/varlena.h"

PG_FUNCTION_INFO_V1(lion_index_facets);

/* At most this many candidates by default, as the top k of §36 takes. */
#define LION_FACET_CAND_MAX		16384

/*
 * A k past this many keys is counted as every key - the walk keeps nothing it
 * does not return then anyway - and the output cut at k.
 */
#define LION_FACET_K_MAX		1000000

/* What a part of the filter selects. */
typedef enum LionFacetSel
{
	LION_FSEL_NONE,				/* no row */
	LION_FSEL_ALL,				/* every row: no restriction */
	LION_FSEL_TREE				/* the rows of a tree over located sets */
} LionFacetSel;

/* The filter, being taken apart into located sets. */
typedef struct LionFacetFilter
{
	Relation	heap;
	Snapshot	snapshot;
	List	   *indexes;		/* the lion indexes of heap, opened */
	List	   *vetted;			/* OIDs of the ones lion_reader_vet() passed */
	int			nsets;
	int			maxsets;
	LionPostingSet *sets;
} LionFacetFilter;

/*
 * A key counted, or a candidate to count.  A key that is not NULL nor of a
 * by-value type is its own copy in keycxt (lion_facets_next_entry(),
 * lion_facets_heap()), owned by the one array that holds the key - the
 * candidates, a batch, the result - and freed by whoever drops it
 * (lion_facets_key_free()), so that keycxt holds the keys kept, not every
 * key walked.
 */
typedef struct LionFacetKey
{
	uint64		ntids;			/* the entry's bound */
	int64		count;			/* the rows counted */
	Datum		key;
	bool		isnull;			/* the NULL group of a scalar column */
} LionFacetKey;

/* One run of the function. */
typedef struct LionFacetRun
{
	Relation	heap;
	Relation	index;
	AttrNumber	attno;
	LionState  *state;
	Snapshot	snapshot;
	int64		k;				/* the top k counted; -1: every key */
	int64		limit;			/* the keys returned; -1: every one */
	bool		hasfilter;
	bool		hascopy;		/* the filter is collected into copy */
	LionPostingSet copy;
	LionCountSource filtersrc;	/* the filter as a source, when not */
	LionVisCache *cache;
	LionCountStats stats;
	double		share;			/* of the table's rows the filter keeps */
	int64		fetched;		/* rows read from the heap */
	bool		heapread;		/* the keys were read from the filter's rows */
	int64		walked;			/* entry headers read */
	int64		counted;		/* keys counted */
	bool		whole;			/* the candidates did not settle the top k */
	int			batch;			/* keys counted together at most */
	PGAlignedBlock *images;		/* ... and their cursors' page images */
	MemoryContext percount;		/* reset around every count */
	MemoryContext keycxt;		/* the keys kept */
	EState	   *estate;			/* the key column's value of a row, made by
								 * lion_facets_row_value(); NULL until then */
	IndexInfo  *indexinfo;

	/* the keys counted, and a min-heap of the k largest counts */
	LionFacetKey *res;
	int			nres;
	int			maxres;
	int64	   *heap_counts;
	int			nheap;
} LionFacetRun;

/* ---------------------------------------------------------------------
 * The filter
 * --------------------------------------------------------------------- */

/*
 * Parse the filter as the WHERE clause of a query over the table alone, and
 * transform it as such a query's would be.  It is parsed by itself, after
 * "SELECT WHERE", and the statement it makes is refused unless the filter is
 * all of its WHERE clause: nothing else of a SELECT may come out of it.
 */
static Node *
lion_facets_parse(Relation heap, const char *filter)
{
	const char *prefix = "SELECT WHERE ";
	char	   *sql = psprintf("%s%s", prefix, filter);
	List	   *raw;
	RawStmt    *rs;
	SelectStmt *sel;
	ParseState *pstate;
	ParseNamespaceItem *nsitem;
	Node	   *qual;

	raw = raw_parser(sql, RAW_PARSE_DEFAULT);
	if (list_length(raw) != 1)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("a facet filter must be one WHERE condition")));
	rs = linitial_node(RawStmt, raw);
	if (!IsA(rs->stmt, SelectStmt))
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("a facet filter must be one WHERE condition")));
	sel = (SelectStmt *) rs->stmt;
	if (sel->op != SETOP_NONE || sel->whereClause == NULL ||
		sel->targetList != NIL || sel->fromClause != NIL ||
		sel->distinctClause != NIL || sel->intoClause != NULL ||
		sel->groupClause != NIL || sel->havingClause != NULL ||
		sel->windowClause != NIL || sel->valuesLists != NIL ||
		sel->sortClause != NIL || sel->limitOffset != NULL ||
		sel->limitCount != NULL || sel->lockingClause != NIL ||
		sel->withClause != NULL)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("a facet filter must be one WHERE condition")));

	/*
	 * No source text: an error position would count the characters of the
	 * statement made around the filter, not of the filter.
	 */
	pstate = make_parsestate(NULL);
	nsitem = addRangeTableEntryForRelation(pstate, heap, AccessShareLock,
										   NULL, false, true);
	addNSItemToQuery(pstate, nsitem, false, true, true);
	qual = transformWhereClause(pstate, sel->whereClause, EXPR_KIND_WHERE,
								"WHERE");
	assign_expr_collations(pstate, qual);
	if (pstate->p_hasSubLinks || checkExprHasSubLink(qual))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("a facet filter cannot contain a subquery")));
	free_parsestate(pstate);

	return qual;
}

/*
 * SELECT on every column the filter reads, as the query it stands for needs
 * it, unless the caller has SELECT on the whole table; and EXECUTE on every
 * function it calls - its operators, and what computes its values - as the
 * executor would ask of that query.
 */
static void
lion_facets_check_filter(Relation heap, Node *qual)
{
	Oid			relid = RelationGetRelid(heap);
	Bitmapset  *cols = NULL;
	int			m = -1;

	(void) lion_check_functions_walker(qual, NULL);

	if (pg_class_aclcheck(relid, GetUserId(), ACL_SELECT) == ACLCHECK_OK)
		return;
	pull_varattnos(qual, 1, &cols);
	while ((m = bms_next_member(cols, m)) >= 0)
	{
		AttrNumber	attnum = m + FirstLowInvalidHeapAttributeNumber;

		if (attnum == InvalidAttrNumber ||
			pg_attribute_aclcheck(relid, attnum, GetUserId(),
								  ACL_SELECT) != ACLCHECK_OK)
			aclcheck_error(ACLCHECK_NO_PRIV,
						   get_relkind_objtype(heap->rd_rel->relkind),
						   RelationGetRelationName(heap));
	}
}

pg_noreturn static void
lion_facets_unanswerable(LionFacetFilter *ff, Node *clause, const char *why)
{
	List	   *dpcontext;
	char	   *text;

	dpcontext = deparse_context_for(RelationGetRelationName(ff->heap),
									RelationGetRelid(ff->heap));
	text = deparse_expression(clause, dpcontext, false, false);
	ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("facet filter condition %s cannot be answered from lion indexes",
					text),
			 errdetail("%s", why),
			 errhint("A facet filter is an AND and OR of equalities, IN lists and IS NULL on columns with a lion index, and of the operators of a multi-key lion index (@>, &&, @@, ?, ...) that its keys answer exactly.")));
}

/* The table's own column, under any binary relabelling, or NULL. */
static Var *
lion_facets_column(Node *node)
{
	while (node != NULL && IsA(node, RelabelType))
		node = (Node *) ((RelabelType *) node)->arg;
	if (node != NULL && IsA(node, Var))
	{
		Var		   *var = (Var *) node;

		if (var->varno == 1 && var->varlevelsup == 0 && var->varattno > 0)
			return var;
	}
	return NULL;
}

/*
 * The value side of a clause, computed once.  It may not read the table, and
 * a volatile one could not stand for the value each row of the query would
 * compute.  The functions are checked for EXECUTE as the executor checks them
 * (ExecInitExpr()).
 */
static Datum
lion_facets_value(LionFacetFilter *ff, Node *clause, Node *expr, bool *isnull)
{
	EState	   *estate;
	ExprState  *es;
	MemoryContext oldcxt;
	Datum		value;
	int16		typlen;
	bool		typbyval;

	if (contain_var_clause(expr))
		lion_facets_unanswerable(ff, clause,
								 "Both sides of the operator refer to the table.");
	if (contain_volatile_functions(expr))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("a facet filter cannot compare with a volatile expression")));

	if (IsA(expr, Const))
	{
		*isnull = ((Const *) expr)->constisnull;
		return ((Const *) expr)->constvalue;
	}

	get_typlenbyval(exprType(expr), &typlen, &typbyval);
	estate = CreateExecutorState();
	oldcxt = MemoryContextSwitchTo(estate->es_query_cxt);
	es = ExecInitExpr((Expr *) expr, NULL);
	value = ExecEvalExprSwitchContext(es, GetPerTupleExprContext(estate),
									  isnull);
	MemoryContextSwitchTo(oldcxt);
	if (!*isnull)
		value = datumCopy(value, typbyval, typlen);
	FreeExecutorState(estate);
	return value;
}

/* Are the index's collation and the clause's the same equality? */
static bool
lion_facets_collation_ok(Oid idxcoll, Oid exprcoll)
{
	if (!OidIsValid(idxcoll) || idxcoll == exprcoll)
		return true;
	if (!OidIsValid(exprcoll))
		return false;
	return get_collation_isdeterministic(idxcoll) &&
		get_collation_isdeterministic(exprcoll);
}

/*
 * The lion index, and its key column, that answers `var op value` - op is
 * InvalidOid for `var IS NULL` - exactly; NULL when none does.  A partial
 * index holds only the rows of its predicate, and is never taken.  Each
 * index taken is vetted once (lion_reader_vet()) before it is read.
 */
static Relation
lion_facets_find_index(LionFacetFilter *ff, Var *var, Oid opno, Oid inputcollid,
					   AttrNumber *idxcol, StrategyNumber *strategy,
					   Oid *subtype)
{
	ListCell   *lc;

	foreach(lc, ff->indexes)
	{
		Relation	index = (Relation) lfirst(lc);
		int			c;

		if (!heap_attisnull(index->rd_indextuple, Anum_pg_index_indpred, NULL))
			continue;
		for (c = 0; c < IndexRelationGetNumberOfKeyAttributes(index); c++)
		{
			LionState  *state;
			int			strat;
			Oid			lefttype;
			Oid			righttype;

			if (index->rd_index->indkey.values[c] != var->varattno)
				continue;
			state = lion_index_column_state(index, c + 1);
			if (!OidIsValid(opno))
			{
				strat = 0;
				righttype = InvalidOid;
			}
			else
			{
				if (!op_in_opfamily(opno, index->rd_opfamily[c]))
					continue;
				get_op_opfamily_properties(opno, index->rd_opfamily[c], false,
										   &strat, &lefttype, &righttype);
				if (state->multikey ? !LION_STRAT_IS_MULTI_SEARCH(strat) :
					strat != LION_STRAT_EQUAL)
					continue;
				if (!lion_facets_collation_ok(index->rd_indcollation[c],
											  inputcollid))
					continue;
			}

			if (!list_member_oid(ff->vetted, RelationGetRelid(index)))
			{
				lion_reader_vet(ff->heap, index, ff->snapshot, "count");
				ff->vetted = lappend_oid(ff->vetted, RelationGetRelid(index));
			}
			*idxcol = c + 1;
			*strategy = (StrategyNumber) strat;
			*subtype = righttype;
			return index;
		}
	}
	return NULL;
}

static void
lion_facets_shift(LionKeyNode *node, int delta)
{
	int			i;

	if (node == NULL)
		return;
	if (node->kind == LION_KN_KEY)
		node->keyno += delta;
	for (i = 0; i < node->nargs; i++)
		lion_facets_shift(node->args[i], delta);
}

/*
 * One clause the index answers: its sets located as a scan locates them
 * (lion_scankey_sets()), appended to the filter's, and the tree over them.
 */
static LionFacetSel
lion_facets_locate(LionFacetFilter *ff, Node *clause, Relation index,
				   ScanKey skey, LionKeyNode **tree)
{
	LionPostingSet *sets;
	LionKeyNode *node;
	int			nsets;
	bool		nomatch;
	int			i;

	if (!lion_scankey_sets(index, skey, &nsets, &sets, &node, &nomatch))
	{
		for (i = 0; i < nsets; i++)
			lion_posting_set_release(&sets[i]);
		lion_facets_unanswerable(ff, clause,
								 "The index's keys do not answer this query exactly (a NOT, a phrase, a weight, `<@`, an empty containment).");
	}
	if (nomatch || node == NULL)
	{
		for (i = 0; i < nsets; i++)
			lion_posting_set_release(&sets[i]);
		return LION_FSEL_NONE;
	}

	if (ff->nsets + nsets > ff->maxsets)
	{
		ff->maxsets = Max(ff->maxsets * 2, ff->nsets + nsets + 8);
		ff->sets = (ff->sets == NULL) ?
			(LionPostingSet *) palloc(sizeof(LionPostingSet) * ff->maxsets) :
			(LionPostingSet *) repalloc(ff->sets,
										sizeof(LionPostingSet) * ff->maxsets);
	}
	for (i = 0; i < nsets; i++)
		ff->sets[ff->nsets + i] = sets[i];
	lion_facets_shift(node, ff->nsets);
	ff->nsets += nsets;
	*tree = node;
	return LION_FSEL_TREE;
}

/* `var op value` or `var op ANY (array)` */
static LionFacetSel
lion_facets_opclause(LionFacetFilter *ff, Node *clause, Node *left,
					 Node *right, Oid opno, Oid inputcollid, bool isarray,
					 LionKeyNode **tree)
{
	Var		   *var = lion_facets_column(left);
	Relation	index;
	AttrNumber	idxcol;
	StrategyNumber strategy;
	Oid			subtype;
	ScanKeyData skey;
	Datum		value;
	bool		isnull;

	if (var == NULL && !isarray)
	{
		var = lion_facets_column(right);
		if (var != NULL)
		{
			Node	   *t = left;

			opno = get_commutator(opno);
			if (!OidIsValid(opno))
				lion_facets_unanswerable(ff, clause,
										 "The operator has no commutator to put the column on its left.");
			left = right;
			right = t;
		}
	}
	if (var == NULL)
		lion_facets_unanswerable(ff, clause,
								 "The operator does not compare a column of the table with a value.");

	index = lion_facets_find_index(ff, var, opno, inputcollid, &idxcol,
								   &strategy, &subtype);
	if (index == NULL)
		lion_facets_unanswerable(ff, clause,
								 "No lion index of the table answers this operator on this column; a partial index is never used.");

	value = lion_facets_value(ff, clause, right, &isnull);
	ScanKeyEntryInitialize(&skey,
						   (isnull ? SK_ISNULL : 0) |
						   (isarray ? SK_SEARCHARRAY : 0),
						   idxcol, strategy, subtype, inputcollid,
						   get_opcode(opno), value);
	return lion_facets_locate(ff, clause, index, &skey, tree);
}

/*
 * What a part of the filter selects: NONE, ALL, or the rows of the tree over
 * ff->sets that *tree is set to.
 */
static LionFacetSel
lion_facets_filter_tree(LionFacetFilter *ff, Node *node, LionKeyNode **tree)
{
	check_stack_depth();
	CHECK_FOR_INTERRUPTS();

	*tree = NULL;
	if (node == NULL)
		return LION_FSEL_ALL;

	switch (nodeTag(node))
	{
		case T_Const:
			{
				Const	   *c = (Const *) node;

				if (c->consttype != BOOLOID)
					break;
				return (!c->constisnull && DatumGetBool(c->constvalue)) ?
					LION_FSEL_ALL : LION_FSEL_NONE;
			}
		case T_BoolExpr:
			{
				BoolExpr   *b = (BoolExpr *) node;
				LionKeyNode **args;
				int			nargs = 0;
				ListCell   *lc;
				bool		isand = (b->boolop == AND_EXPR);

				if (b->boolop == NOT_EXPR)
					lion_facets_unanswerable(ff, node,
											 "NOT has no posting set.");
				args = (LionKeyNode **) palloc(sizeof(LionKeyNode *) *
											   list_length(b->args));
				foreach(lc, b->args)
				{
					LionKeyNode *sub;
					LionFacetSel sel;

					sel = lion_facets_filter_tree(ff, (Node *) lfirst(lc), &sub);
					if (sel == LION_FSEL_TREE)
						args[nargs++] = sub;
					else if (sel == LION_FSEL_NONE && isand)
						return LION_FSEL_NONE;
					else if (sel == LION_FSEL_ALL && !isand)
						return LION_FSEL_ALL;
				}
				if (nargs == 0)
					return isand ? LION_FSEL_ALL : LION_FSEL_NONE;
				*tree = (nargs == 1) ? args[0] :
					lion_keynode_op(isand ? LION_KN_AND : LION_KN_OR, args,
									nargs);
				return LION_FSEL_TREE;
			}
		case T_OpExpr:
			{
				OpExpr	   *op = (OpExpr *) node;

				if (list_length(op->args) != 2)
					break;
				return lion_facets_opclause(ff, node, linitial(op->args),
											lsecond(op->args), op->opno,
											op->inputcollid, false, tree);
			}
		case T_ScalarArrayOpExpr:
			{
				ScalarArrayOpExpr *sa = (ScalarArrayOpExpr *) node;

				if (!sa->useOr)
					lion_facets_unanswerable(ff, node,
											 "op ALL (array) has no posting set.");
				return lion_facets_opclause(ff, node, linitial(sa->args),
											lsecond(sa->args), sa->opno,
											sa->inputcollid, true, tree);
			}
		case T_NullTest:
			{
				NullTest   *nt = (NullTest *) node;
				Var		   *var = lion_facets_column((Node *) nt->arg);
				Relation	index;
				AttrNumber	idxcol;
				StrategyNumber strategy;
				Oid			subtype;
				ScanKeyData skey;

				if (nt->nulltesttype != IS_NULL || nt->argisrow || var == NULL)
					lion_facets_unanswerable(ff, node,
											 "Only `column IS NULL` is a posting set.");
				index = lion_facets_find_index(ff, var, InvalidOid, InvalidOid,
											   &idxcol, &strategy, &subtype);
				if (index == NULL)
					lion_facets_unanswerable(ff, node,
											 "No lion index of the table has this column; a partial index is never used.");
				ScanKeyEntryInitialize(&skey, SK_ISNULL | SK_SEARCHNULL, idxcol,
									   InvalidStrategy, InvalidOid, InvalidOid,
									   InvalidOid, (Datum) 0);
				return lion_facets_locate(ff, node, index, &skey, tree);
			}
		default:
			break;
	}
	lion_facets_unanswerable(ff, node, "It is not an operator, IS NULL, AND or OR.");
}

/*
 * The filter of a run, taken apart into posting sets and collected once:
 * false when it selects no row at all.
 */
static bool
lion_facets_filter(LionFacetRun *run, const char *filter, LionFacetFilter *ff)
{
	Node	   *qual;
	List	   *oids;
	ListCell   *lc;
	LionKeyNode *tree;
	LionFacetSel sel;

	memset(ff, 0, sizeof(*ff));
	ff->heap = run->heap;
	ff->snapshot = run->snapshot;

	qual = lion_facets_parse(run->heap, filter);
	lion_facets_check_filter(run->heap, qual);
	qual = eval_const_expressions(NULL, qual);

	/* the table's lion indexes, in OID order, the planner's */
	oids = RelationGetIndexList(run->heap);
	foreach(lc, oids)
	{
		Relation	index = index_open(lfirst_oid(lc), AccessShareLock);

		if (index->rd_rel->relam == lion_get_am_oid() &&
			index->rd_index->indisvalid)
			ff->indexes = lappend(ff->indexes, index);
		else
			index_close(index, AccessShareLock);
	}

	sel = lion_facets_filter_tree(ff, qual, &tree);
	if (sel == LION_FSEL_NONE)
		return false;
	if (sel == LION_FSEL_ALL)
		return true;

	run->hasfilter = true;
	memset(&run->filtersrc, 0, sizeof(LionCountSource));
	run->filtersrc.nsets = ff->nsets;
	run->filtersrc.sets = ff->sets;
	run->filtersrc.tree = tree;

	if (!lion_sets_satisfiable(ff->nsets, ff->sets, tree))
		return false;

	/*
	 * Collected once, as the GROUP BY pushdown collects its WHERE (DESIGN.md
	 * §10, "The WHERE sets, collected once"), and counted ANDed with each
	 * key's own located set, which carries the §9 interlock: the copy was
	 * made after the snapshot was taken, so it holds every row the snapshot
	 * sees, and a dead TID it still lists goes to the map or the heap with
	 * the key's own set's (lion_group_count()).  Not on a standby, where it
	 * would carry every index's TIDs under one index's WAL mode
	 * (lion_where_collect()): the sets are counted as they are there.
	 */
	if (!RecoveryInProgress())
	{
		bool		spilled;

		if (lion_sources_collect(run->heap, run->snapshot, 1, &run->filtersrc,
								 get_hash_memory_limit(), true, &run->copy,
								 &spilled, &run->stats))
		{
			int			i;

			if (!run->copy.found)
				return false;	/* the filter's sets have no row in common */
			run->hascopy = true;
			for (i = 0; i < ff->nsets; i++)
				lion_posting_set_release(&ff->sets[i]);
			ff->nsets = 0;
		}
	}
	return true;
}

static void
lion_facets_filter_end(LionFacetFilter *ff)
{
	ListCell   *lc;
	int			i;

	for (i = 0; i < ff->nsets; i++)
		lion_posting_set_release(&ff->sets[i]);
	ff->nsets = 0;
	foreach(lc, ff->indexes)
		index_close((Relation) lfirst(lc), AccessShareLock);
	ff->indexes = NIL;
}

/* ---------------------------------------------------------------------
 * Counting the keys
 * --------------------------------------------------------------------- */

/*
 * Two keys in the column's order: its btree comparison, under the index's
 * collation, where the type has one, and the stored bytes otherwise.  The
 * NULL group last, as ORDER BY col puts it.
 */
static int
lion_facets_keycmp(LionState *state, const LionFacetKey *a,
				   const LionFacetKey *b)
{
	if (a->isnull || b->isnull)
		return (a->isnull ? 1 : 0) - (b->isnull ? 1 : 0);
	if (state->ordered)
		return DatumGetInt32(FunctionCall2Coll(&state->cmpproc,
											   state->collation,
											   a->key, b->key));
	if (state->typbyval)
		return (a->key < b->key) ? -1 : (a->key > b->key) ? 1 : 0;
	if (state->typlen > 0)
		return memcmp(DatumGetPointer(a->key), DatumGetPointer(b->key),
					  state->typlen);
	if (state->typlen == -1)
	{
		const struct varlena *x = (const struct varlena *) DatumGetPointer(a->key);
		const struct varlena *y = (const struct varlena *) DatumGetPointer(b->key);
		Size		lx = VARSIZE_ANY_EXHDR(x);
		Size		ly = VARSIZE_ANY_EXHDR(y);
		int			r = memcmp(VARDATA_ANY(x), VARDATA_ANY(y), Min(lx, ly));

		return (r != 0) ? r : (lx < ly) ? -1 : (lx > ly) ? 1 : 0;
	}
	return strcmp(DatumGetCString(a->key), DatumGetCString(b->key));
}

/* Free a key's copy in keycxt, when it has one. */
static void
lion_facets_key_free(LionFacetRun *run, LionFacetKey *k)
{
	if (!k->isnull && !run->state->typbyval)
		pfree(DatumGetPointer(k->key));
}

static LionState *lion_facets_sort_state;

/* larger counts first, then the keys in the column's order */
static int
lion_facets_res_cmp(const void *a, const void *b)
{
	const LionFacetKey *x = (const LionFacetKey *) a;
	const LionFacetKey *y = (const LionFacetKey *) b;

	if (x->count != y->count)
		return (x->count > y->count) ? -1 : 1;
	return lion_facets_keycmp(lion_facets_sort_state, x, y);
}

/* larger bounds first */
static int
lion_facets_cand_cmp(const void *a, const void *b)
{
	uint64		x = ((const LionFacetKey *) a)->ntids;
	uint64		y = ((const LionFacetKey *) b)->ntids;

	return (x > y) ? -1 : (x < y) ? 1 : 0;
}

static void
lion_facets_sort(LionFacetRun *run)
{
	lion_facets_sort_state = run->state;
	qsort(run->res, run->nres, sizeof(LionFacetKey), lion_facets_res_cmp);
	lion_facets_sort_state = NULL;
}

/* The k-th largest count so far, in a min-heap of k (lion_topk_push()). */
static void
lion_facets_heap_push(LionFacetRun *run, int64 v)
{
	int64	   *h = run->heap_counts;
	int			i;

	if (run->nheap >= run->k)
	{
		if (v <= h[0])
			return;
		h[0] = v;
		i = 0;
		for (;;)
		{
			int			l = 2 * i + 1;
			int			m = i;
			int64		t;

			if (l < run->nheap && h[l] < h[m])
				m = l;
			if (l + 1 < run->nheap && h[l + 1] < h[m])
				m = l + 1;
			if (m == i)
				return;
			t = h[i];
			h[i] = h[m];
			h[m] = t;
			i = m;
		}
	}
	i = run->nheap++;
	h[i] = v;
	while (i > 0 && h[(i - 1) / 2] > h[i])
	{
		int64		t = h[i];

		h[i] = h[(i - 1) / 2];
		h[(i - 1) / 2] = t;
		i = (i - 1) / 2;
	}
}

/* Has the top k got k counts, all of them larger than bound? */
static bool
lion_facets_settled(LionFacetRun *run, uint64 bound)
{
	return run->k >= 0 && run->nheap >= run->k &&
		(uint64) run->heap_counts[0] > bound;
}

/*
 * The rows of one key that the snapshot sees, under the filter: its entry
 * looked up again by the key - VACUUM may have deleted it since its header
 * was read, which it does only to an entry holding no row the snapshot sees -
 * and counted with the filter's copy, as one group of a GROUP BY is.
 */
static int64
lion_facets_count(LionFacetRun *run, const LionFacetKey *k)
{
	LionPostingSet ps;
	LionCountSource src[2];
	MemoryContext oldcxt;
	bool		found;
	int64		count = 0;
	int			n = 1;

	CHECK_FOR_INTERRUPTS();
	MemoryContextReset(run->percount);
	oldcxt = MemoryContextSwitchTo(run->percount);

	if (k->isnull)
		found = lion_posting_set_lookup_null_col(run->index, run->attno, &ps);
	else
		found = lion_posting_set_lookup_col(run->index, run->attno, k->key,
											InvalidOid, &ps);
	if (found)
	{
		memset(src, 0, sizeof(src));
		src[0].nsets = 1;
		src[0].sets = &ps;
		if (run->hascopy)
		{
			src[1].nsets = 1;
			src[1].sets = &run->copy;
			n = 2;
		}
		else if (run->hasfilter)
		{
			src[1] = run->filtersrc;
			n = 2;
		}
		count = lion_count_sources_cached(run->heap, run->snapshot, n, src,
										  &run->stats, run->cache, false);
	}
	lion_posting_set_release(&ps);
	MemoryContextSwitchTo(oldcxt);
	return count;
}

/*
 * Keep a key counted, and keep no more of them than the top k needs.  The
 * key's copy passes to the result, or is freed: the caller holds it no more.
 */
static void
lion_facets_keep(LionFacetRun *run, LionFacetKey *k, int64 count)
{
	if (count <= 0)
	{
		/* a group exists only with a visible row */
		lion_facets_key_free(run, k);
		return;
	}
	if (run->nres >= run->maxres)
	{
		/*
		 * Past twice k, the keys after the k-th in the final order cannot be
		 * in it: cut back to k, and free the rest.  The heap of the k largest
		 * counts holds counts, not places in res, and is not touched.
		 */
		if (run->k >= 0 && run->nres >= 2 * run->k)
		{
			int			i;

			lion_facets_sort(run);
			for (i = (int) run->k; i < run->nres; i++)
				lion_facets_key_free(run, &run->res[i]);
			run->nres = (int) run->k;
		}
		else
		{
			run->maxres = run->maxres * 2;
			run->res = (LionFacetKey *)
				repalloc_huge(run->res, sizeof(LionFacetKey) * run->maxres);
		}
	}
	run->res[run->nres] = *k;
	run->res[run->nres].count = count;
	run->nres++;
	if (run->k >= 0)
		lion_facets_heap_push(run, count);
}

/* ---------------------------------------------------------------------
 * The rows of a set, read from the heap
 * --------------------------------------------------------------------- */

/* What lion_facets_rows() hands each row it fetches to. */
typedef void (*LionFacetRowFn) (LionFacetRun *run, TupleTableSlot *slot,
								void *arg);

/*
 * Every row of stream's TIDs that the snapshot sees, fetched as an index scan
 * fetches it (table_index_fetch_tuple(): the HOT chain followed, the snapshot
 * applied, the predicate lock taken) and handed to fn in a context of its
 * own, which is reset around each: not percount, which a caller's counts may
 * still be in (lion_facets_count_keep()).
 */
static void
lion_facets_rows(LionFacetRun *run, LionSetStream *stream, LionFacetRowFn fn,
				 void *arg)
{
#if PG_VERSION_NUM < 200000
	IndexFetchTableData *fetch;
#endif
	TupleTableSlot *slot;
	const LionContainer *c;
	uint16	   *los;
	MemoryContext rowcxt;
	MemoryContext oldcxt;

	los = (uint16 *) palloc(sizeof(uint16) * LION_CONTAINER_RANGE);
	rowcxt = AllocSetContextCreate(CurrentMemoryContext,
								   "lion index facet row",
								   ALLOCSET_DEFAULT_SIZES);
	/*
	 * The heap side of an index scan, as lo_fetch_walk() has it: 19 gave
	 * table_index_fetch_begin() scan options, and 20 moved the HOT chain
	 * search into the table AM (table_fetch_tid(), then the version it found).
	 */
#if PG_VERSION_NUM >= 200000
#elif PG_VERSION_NUM >= 190000
	fetch = table_index_fetch_begin(run->heap, SO_NONE);
#else
	fetch = table_index_fetch_begin(run->heap);
#endif
	slot = table_slot_create(run->heap, NULL);

	while ((c = lion_stream_next(stream)) != NULL)
	{
		uint32		n = lion_container_to_array(c, los);
		uint32		i;

		CHECK_FOR_INTERRUPTS();
		for (i = 0; i < n; i++)
		{
			ItemPointerData tid;
			bool		all_dead = false;

			lion_code_to_tid(lion_make_code(c->ckey, los[i]), &tid);
#if PG_VERSION_NUM >= 200000
			if (!table_fetch_tid(run->heap, &tid, run->snapshot, &all_dead) ||
				!table_tuple_fetch_row_version(run->heap, &tid, run->snapshot,
												slot))
				continue;
#else
			{
				bool		call_again = false;

				if (!table_index_fetch_tuple(fetch, &tid, run->snapshot, slot,
											 &call_again, &all_dead))
					continue;
			}
#endif
			run->fetched++;

			MemoryContextReset(rowcxt);
			oldcxt = MemoryContextSwitchTo(rowcxt);
			fn(run, slot, arg);
			MemoryContextSwitchTo(oldcxt);
		}
	}

	ExecDropSingleTupleTableSlot(slot);
#if PG_VERSION_NUM < 200000
	table_index_fetch_end(fetch);
#endif
	pfree(los);
	MemoryContextDelete(rowcxt);
}

/* The keys of the rows, as a hash table under the column's own equality. */
typedef struct LionFacetHashEnt
{
	Datum		key;
	int64		count;
} LionFacetHashEnt;

static LionState *lion_facets_hash_state;

static uint32
lion_facets_hash(const void *key, Size keysize)
{
	return lion_hash_key(lion_facets_hash_state, *(const Datum *) key);
}

static int
lion_facets_match(const void *key1, const void *key2, Size keysize)
{
	return lion_keys_equal(lion_facets_hash_state, *(const Datum *) key1,
						   *(const Datum *) key2) ? 0 : 1;
}

static HTAB *
lion_facets_htab(LionFacetRun *run)
{
	HASHCTL		ctl;

	memset(&ctl, 0, sizeof(ctl));
	ctl.keysize = sizeof(Datum);
	ctl.entrysize = sizeof(LionFacetHashEnt);
	ctl.hash = lion_facets_hash;
	ctl.match = lion_facets_match;
	ctl.hcxt = run->keycxt;
	lion_facets_hash_state = run->state;
	return hash_create("lion index facets", 1024, &ctl,
					   HASH_ELEM | HASH_FUNCTION | HASH_COMPARE | HASH_CONTEXT);
}

/* One row under key: its entry in htab, made with a copy of the key if new. */
static void
lion_facets_heap_add(LionFacetRun *run, HTAB *htab, Datum key)
{
	LionFacetHashEnt *ent;
	bool		found;

	lion_facets_hash_state = run->state;
	ent = (LionFacetHashEnt *) hash_search(htab, &key, HASH_ENTER, &found);
	if (!found)
	{
		MemoryContext oldcxt = MemoryContextSwitchTo(run->keycxt);

		ent->key = datumCopy(key, run->state->typbyval, run->state->typlen);
		ent->count = 0;
		MemoryContextSwitchTo(oldcxt);
	}
	ent->count++;
}

/*
 * Every key of htab with its count, passed to lion_facets_keep() with bound
 * as its bound, and htab destroyed.  Each entry's key passes to
 * lion_facets_keep(), which may free it; the table is only walked and
 * destroyed after that, never searched.
 */
static void
lion_facets_keep_htab(LionFacetRun *run, HTAB *htab, uint64 bound)
{
	HASH_SEQ_STATUS seq;
	LionFacetHashEnt *ent;

	hash_seq_init(&seq, htab);
	while ((ent = (LionFacetHashEnt *) hash_seq_search(&seq)) != NULL)
	{
		LionFacetKey k;

		k.ntids = bound;
		k.count = ent->count;
		k.key = ent->key;
		k.isnull = false;
		lion_facets_keep(run, &k, ent->count);
	}
	hash_destroy(htab);
	lion_facets_hash_state = NULL;
}

/* ---------------------------------------------------------------------
 * Hashed keys
 * --------------------------------------------------------------------- */

/*
 * A key of LION_LEXEME_KEY_MAX bytes of a tsvector column, or of a text or
 * bytea array one (lion_key_is_ambiguous()), may be the hashed key of longer
 * lexemes or elements as well as one spelled like it (DESIGN.md §17, "Long
 * lexemes"), and its rows are those of all of them.  ts_stat() and `GROUP BY
 * e` show each with its own rows, and so does this: the key's rows that the
 * filter keeps and the snapshot sees are read from the heap, and each is
 * counted under every value of its own that is filed under the key
 * (lion_ambiguous_values()) - the rows the key's count counts, so the counts
 * of its values are exact and none is larger than the key's bound.  A row's
 * value is the index column's, an expression's too (FormIndexDatum()).
 */
static Datum
lion_facets_row_value(LionFacetRun *run, TupleTableSlot *slot, bool *isnull)
{
	Datum		values[INDEX_MAX_KEYS];
	bool		isnulls[INDEX_MAX_KEYS];
	ExprContext *econtext;

	if (run->estate == NULL)
	{
		MemoryContext oldcxt = MemoryContextSwitchTo(run->keycxt);

		run->indexinfo = BuildIndexInfo(run->index);
		run->estate = CreateExecutorState();
		MemoryContextSwitchTo(oldcxt);
	}
	econtext = GetPerTupleExprContext(run->estate);
	ResetExprContext(econtext);
	econtext->ecxt_scantuple = slot;
	FormIndexDatum(run->indexinfo, slot, run->estate, values, isnulls);
	*isnull = isnulls[run->attno - 1];
	return values[run->attno - 1];
}

typedef struct LionFacetResolve
{
	HTAB	   *htab;
	Datum		key;
} LionFacetResolve;

static void
lion_facets_resolve_row(LionFacetRun *run, TupleTableSlot *slot, void *arg)
{
	LionFacetResolve *r = (LionFacetResolve *) arg;
	Datum	   *values;
	bool		isnull;
	Datum		value = lion_facets_row_value(run, slot, &isnull);
	int			n;
	int			i;

	if (isnull)
		return;
	n = lion_ambiguous_values(run->state, value, r->key, &values);
	for (i = 0; i < n; i++)
		lion_facets_heap_add(run, r->htab, values[i]);
}

/* A copy of tree with every key number moved up by off. */
static LionKeyNode *
lion_facets_tree_shift(const LionKeyNode *tree, int off)
{
	LionKeyNode *n = (LionKeyNode *) palloc(sizeof(LionKeyNode));
	int			i;

	*n = *tree;
	if (tree->kind == LION_KN_KEY)
		n->keyno = tree->keyno + off;
	if (tree->nargs > 0)
	{
		n->args = (LionKeyNode **) palloc(sizeof(LionKeyNode *) * tree->nargs);
		for (i = 0; i < tree->nargs; i++)
			n->args[i] = lion_facets_tree_shift(tree->args[i], off);
	}
	return n;
}

/*
 * The values a hashed key k stands for, each kept with its own count; k's
 * copy is freed.
 */
static void
lion_facets_resolve(LionFacetRun *run, LionFacetKey *k)
{
	MemoryContext setcxt;
	MemoryContext oldcxt;
	LionFacetResolve r;
	LionPostingSet *sets;
	LionKeyNode *tree = NULL;
	int			nsets = 1;
	LionSetStream *stream;

	setcxt = AllocSetContextCreate(CurrentMemoryContext,
								   "lion index facet key rows",
								   ALLOCSET_DEFAULT_SIZES);
	oldcxt = MemoryContextSwitchTo(setcxt);
	r.htab = lion_facets_htab(run);
	r.key = k->key;

	sets = (LionPostingSet *) palloc0(sizeof(LionPostingSet) *
									  (1 + Max(run->filtersrc.nsets, 1)));
	if (lion_posting_set_lookup_col(run->index, run->attno, k->key,
									InvalidOid, &sets[0]))
	{
		LionKeyNode *args[2];

		if (run->hascopy)
		{
			sets[1] = run->copy;
			nsets = 2;
			args[0] = lion_keynode_leaf(0);
			args[1] = lion_keynode_leaf(1);
			tree = lion_keynode_op(LION_KN_AND, args, 2);
		}
		else if (run->hasfilter)
		{
			LionKeyNode *ftree = lion_source_tree(&run->filtersrc);

			if (ftree == NULL)
				elog(ERROR, "lion index facets: a filter with no sets");
			memcpy(&sets[1], run->filtersrc.sets,
				   sizeof(LionPostingSet) * run->filtersrc.nsets);
			nsets = 1 + run->filtersrc.nsets;
			args[0] = lion_keynode_leaf(0);
			args[1] = lion_facets_tree_shift(ftree, 1);
			tree = lion_keynode_op(LION_KN_AND, args, 2);
		}
		stream = lion_stream_begin(nsets, sets, tree, false);
		lion_facets_rows(run, stream, lion_facets_resolve_row, &r);
		lion_stream_end(stream);
	}
	lion_posting_set_release(&sets[0]);
	MemoryContextSwitchTo(oldcxt);

	lion_facets_keep_htab(run, r.htab, k->ntids);
	lion_facets_key_free(run, k);
	MemoryContextDelete(setcxt);
}

/*
 * Keep a key counted from the index, as lion_facets_keep() does - or, for a
 * hashed key with rows, the values it stands for.
 */
static void
lion_facets_keep_key(LionFacetRun *run, LionFacetKey *k, int64 count)
{
	if (count > 0 && !k->isnull && lion_key_is_ambiguous(run->state, k->key))
		lion_facets_resolve(run, k);
	else
		lion_facets_keep(run, k, count);
}

/*
 * Count n keys and keep them, each passed to lion_facets_keep() or freed.
 * Against a collected filter they are counted together, a batch at a time
 * (lion_count_groups_copy(); DESIGN.md §10, "The groups of a walk, counted
 * together"): each key's set located afresh, with the pin that carries its
 * count's §9 interlock, and the copy read once for all of them.  A key whose
 * entry VACUUM has deleted since its header was read has no row the snapshot
 * sees.
 */
static void
lion_facets_count_keep(LionFacetRun *run, LionFacetKey *keys, int n)
{
	LionPostingSet *sets;
	int64	   *counts;
	int		   *which;
	MemoryContext oldcxt;
	int			nfound = 0;
	int			i;

	run->counted += n;
	if (!run->hascopy || n == 1)
	{
		for (i = 0; i < n; i++)
			lion_facets_keep_key(run, &keys[i],
								 lion_facets_count(run, &keys[i]));
		return;
	}

	CHECK_FOR_INTERRUPTS();
	MemoryContextReset(run->percount);
	oldcxt = MemoryContextSwitchTo(run->percount);
	sets = (LionPostingSet *) palloc(sizeof(LionPostingSet) * n);
	counts = (int64 *) palloc(sizeof(int64) * n);
	which = (int *) palloc(sizeof(int) * n);
	for (i = 0; i < n; i++)
	{
		bool		found;

		if (keys[i].isnull)
			found = lion_posting_set_lookup_null_col(run->index, run->attno,
													 &sets[nfound]);
		else
			found = lion_posting_set_lookup_col(run->index, run->attno,
												keys[i].key, InvalidOid,
												&sets[nfound]);
		if (found)
			which[nfound++] = i;
		else
		{
			lion_posting_set_release(&sets[nfound]);
			lion_facets_key_free(run, &keys[i]);
		}
	}
	if (nfound > 0)
		lion_count_groups_copy(run->heap, run->snapshot, nfound, sets,
							   &run->copy, counts, &run->stats, run->cache,
							   false, run->images);
	for (i = 0; i < nfound; i++)
		lion_posting_set_release(&sets[i]);
	MemoryContextSwitchTo(oldcxt);

	for (i = 0; i < nfound; i++)
		lion_facets_keep_key(run, &keys[which[i]], counts[i]);
}

/*
 * The next entry of the walk whose rows are a key's: not the reserved EMPTY
 * entry, nor - for a multi-key column, whose NULL rows unnest to nothing -
 * the NULL one, nor the length key of a column that stores positions, nor
 * an entry with no TID.  Its key is copied into keycxt.
 */
static bool
lion_facets_next_entry(LionFacetRun *run, LionEntryScan *es,
					   LionFacetKey *k)
{
	for (;;)
	{
		LionEntryTuple *entry;
		Size		itemlen;
		MemoryContext oldcxt;

		entry = lion_entry_scan_next_copy(es, &itemlen);
		if (entry == NULL)
			return false;
		run->walked++;
		if (LionEntryIsEmptyKey(entry) || lion_entry_ntids(entry) == 0 ||
			(LionEntryIsNullKey(entry) && run->state->multikey))
			continue;
		k->isnull = LionEntryIsNullKey(entry);
		k->key = k->isnull ? (Datum) 0 : lion_entry_key(es->state, entry);
		if (!k->isnull && lion_is_length_key(run->state, k->key))
			continue;
		k->ntids = lion_entry_ntids(entry);
		k->count = 0;
		oldcxt = MemoryContextSwitchTo(run->keycxt);
		if (!k->isnull)
			k->key = datumCopy(k->key, run->state->typbyval,
							   run->state->typlen);
		MemoryContextSwitchTo(oldcxt);
		return true;
	}
}

/*
 * Every key, or - with a k - every key whose bound is not below the k-th
 * count so far: a walk of the column in its order.
 */
static void
lion_facets_walk(LionFacetRun *run)
{
	LionEntryScan es;
	LionFacetKey *pending;
	int			npending = 0;

	pending = (LionFacetKey *) palloc(sizeof(LionFacetKey) * run->batch);
	lion_entry_scan_begin_col(&es, run->index, run->attno);
	while (lion_facets_next_entry(run, &es, &pending[npending]))
	{
		LionFacetKey *k = &pending[npending];

		if (run->k >= 0 && run->nheap >= run->k &&
			k->ntids < (uint64) run->heap_counts[0])
		{
			lion_facets_key_free(run, k);
			continue;
		}
		if (++npending == run->batch)
		{
			lion_facets_count_keep(run, pending, npending);
			npending = 0;
		}
	}
	lion_entry_scan_end(&es);
	if (npending > 0)
		lion_facets_count_keep(run, pending, npending);
	pfree(pending);
}

/* ---------------------------------------------------------------------
 * A selective filter: the keys of its rows, read from the heap
 * --------------------------------------------------------------------- */

/*
 * What one key costs to count against the filter's copy, against what one of
 * the copy's rows costs to fetch from the heap and take apart, as measured
 * on a synthetic table of 500,000 rows of three-tag arrays and five-lexeme
 * tsvectors (DESIGN.md §7, "Facet counts"): some 5 us a key in a batch,
 * 1 us a row.
 */
#define LION_FACET_KEY_ROWS		5

/*
 * May the filter's rows be read from the heap instead?  The key column must
 * be a column of the table - its value is read from the row - and the index
 * not partial, whose rows a heap row would have to be tested against.
 */
static bool
lion_facets_heap_ok(LionFacetRun *run)
{
	return run->hascopy &&
		run->index->rd_index->indkey.values[run->attno - 1] != 0 &&
		heap_attisnull(run->index->rd_indextuple, Anum_pg_index_indpred,
					   NULL);
}

/*
 * The keys of every row of the filter's copy that the snapshot sees, counted:
 * what `SELECT e, count(*) ... GROUP BY e` does over the rows the filter
 * selects, a row at a time.  Each TID of the copy is fetched as an index scan
 * fetches it (table_index_fetch_tuple(): the HOT chain followed, the
 * snapshot applied, the predicate lock taken), and the copy holds every row
 * the snapshot sees under the filter (lion_facets_filter()), so the counts
 * are exact without the visibility map.  The row's value is taken apart by
 * the index's own extraction, which drops NULL and repeated elements, and
 * grouped under the column's equality - the index's entries' classes.
 */
typedef struct LionFacetHeapRows
{
	HTAB	   *htab;
	AttrNumber	heapatt;
	int64		nullcount;
} LionFacetHeapRows;

static void
lion_facets_heap_row(LionFacetRun *run, TupleTableSlot *slot, void *arg)
{
	LionFacetHeapRows *h = (LionFacetHeapRows *) arg;
	bool		isnull;
	Datum		value = slot_getattr(slot, h->heapatt, &isnull);

	if (isnull)
	{
		/* the NULL group of GROUP BY col; unnest(NULL) is no row */
		if (!run->state->multikey)
			h->nullcount++;
	}
	else if (run->state->multikey)
	{
		Datum	   *keys;
		int			nkeys = lion_extract_value(run->state, value, &keys);
		int			j;

		for (j = 0; j < nkeys; j++)
		{
			if (lion_is_length_key(run->state, keys[j]))
				continue;
			/* a hashed key: the row's own values under it (above) */
			if (lion_key_is_ambiguous(run->state, keys[j]))
			{
				Datum	   *values;
				int			n = lion_ambiguous_values(run->state, value,
													  keys[j], &values);
				int			i;

				for (i = 0; i < n; i++)
					lion_facets_heap_add(run, h->htab, values[i]);
			}
			else
				lion_facets_heap_add(run, h->htab, keys[j]);
		}
	}
	else
		lion_facets_heap_add(run, h->htab, value);
}

static void
lion_facets_heap(LionFacetRun *run)
{
	LionFacetHeapRows h;
	LionSetStream *stream;

	h.htab = lion_facets_htab(run);
	h.heapatt = run->index->rd_index->indkey.values[run->attno - 1];
	h.nullcount = 0;

	stream = lion_stream_begin(1, &run->copy, NULL, false);
	lion_facets_rows(run, stream, lion_facets_heap_row, &h);
	lion_stream_end(stream);

	lion_facets_keep_htab(run, h.htab, 0);
	if (h.nullcount > 0)
	{
		LionFacetKey k;

		k.ntids = 0;
		k.count = h.nullcount;
		k.key = (Datum) 0;
		k.isnull = true;
		lion_facets_keep(run, &k, h.nullcount);
	}
}

/* How many entries of the column hold a key's rows: a walk of the headers. */
static int64
lion_facets_nentries(LionFacetRun *run)
{
	LionEntryScan es;
	LionEntryTuple *entry;
	Size		itemlen;
	int64		n = 0;

	lion_entry_scan_begin_col(&es, run->index, run->attno);
	while ((entry = lion_entry_scan_next_copy(&es, &itemlen)) != NULL)
	{
		run->walked++;
		if (!LionEntryIsEmptyKey(entry) && lion_entry_ntids(entry) > 0 &&
			!(LionEntryIsNullKey(entry) && run->state->multikey))
			n++;
	}
	lion_entry_scan_end(&es);
	return n;
}

/*
 * The top k, from the candidates with the largest bounds (DESIGN.md §36):
 * true when they settled it.  cap candidates are kept from a walk of the
 * headers - in an array of twice that, cut back to the largest cap whenever
 * it fills - with maxout, the largest bound left out, and counted largest
 * bound first until no bound to come can reach or tie with the k-th count.
 */
static bool
lion_facets_topk(LionFacetRun *run, int cap)
{
	LionEntryScan es;
	LionFacetKey *cand;
	LionFacetKey k;
	int			maxcand = Min(2 * cap, 1024);
	int			n = 0;
	int			i;
	uint64		maxout = 0;
	uint64		floor = 0;
	bool		trimmed = false;

	cand = (LionFacetKey *) palloc(sizeof(LionFacetKey) * maxcand);
	lion_entry_scan_begin_col(&es, run->index, run->attno);
	while (lion_facets_next_entry(run, &es, &k))
	{
		if (trimmed && k.ntids <= floor)
		{
			maxout = Max(maxout, k.ntids);
			lion_facets_key_free(run, &k);
			continue;
		}
		if (n == maxcand && maxcand < 2 * cap)
		{
			maxcand = Min(2 * maxcand, 2 * cap);
			cand = (LionFacetKey *) repalloc(cand,
											 sizeof(LionFacetKey) * maxcand);
		}
		cand[n++] = k;
		if (n == 2 * cap)
		{
			qsort(cand, n, sizeof(LionFacetKey), lion_facets_cand_cmp);
			maxout = Max(maxout, cand[cap].ntids);
			for (i = cap; i < n; i++)
				lion_facets_key_free(run, &cand[i]);
			n = cap;
			floor = cand[n - 1].ntids;
			trimmed = true;
		}
	}
	lion_entry_scan_end(&es);

	qsort(cand, n, sizeof(LionFacetKey), lion_facets_cand_cmp);
	if (n > cap)
	{
		maxout = Max(maxout, cand[cap].ntids);
		for (i = cap; i < n; i++)
			lion_facets_key_free(run, &cand[i]);
		n = cap;
	}

	/*
	 * A selective filter: fewer rows than the keys it would take counting,
	 * each LION_FACET_KEY_ROWS rows' worth - the candidates whose bound
	 * reaches what the k-th is likely to count, the share of its bound the
	 * filter keeps - are read from the heap instead.
	 */
	if (n > 0 && lion_facets_heap_ok(run))
	{
		double		likely = run->share *
			(double) cand[Min((int64) n, run->k) - 1].ntids;
		double		tocount = 0;

		for (i = 0; i < n && (double) cand[i].ntids >= likely; i++)
			tocount++;
		if (i == n && maxout > 0)
			tocount = Max(tocount, (double) run->walked);
		if ((double) run->copy.ntids < tocount * LION_FACET_KEY_ROWS)
		{
			for (i = 0; i < n; i++)
				lion_facets_key_free(run, &cand[i]);
			pfree(cand);
			run->heapread = true;
			lion_facets_heap(run);
			return true;
		}
	}

	i = 0;
	while (i < n)
	{
		uint64		next;
		int			want = 0;

		/*
		 * A batch of what has to be counted anyway: as many as the top k
		 * still lacks, or every candidate whose bound reaches the k-th count
		 * so far.
		 */
		if (run->nheap < run->k)
			want = (int) Min(run->k - run->nheap, (int64) n);
		else
		{
			while (i + want < n &&
				   cand[i + want].ntids >= (uint64) run->heap_counts[0])
				want++;
		}
		want = Min(Max(want, 1), Min(run->batch, n - i));
		lion_facets_count_keep(run, &cand[i], want);
		i += want;

		next = (i < n) ? cand[i].ntids : maxout;
		if (next == 0 || lion_facets_settled(run, next))
		{
			/* the candidates not counted */
			for (; i < n; i++)
				lion_facets_key_free(run, &cand[i]);
			pfree(cand);
			return true;
		}
	}
	pfree(cand);
	return maxout == 0;
}

/* The key as text: its type's output, or a jsonb_contains_ops path key's. */
static text *
lion_facets_key_text(LionFacetRun *run, Datum key, FmgrInfo *outfn)
{
	if (run->state->extractvalue.fn_addr == lion_jsonb_extract_value)
		return cstring_to_text(lion_jsonb_key_out(DatumGetByteaPP(key)));
	return cstring_to_text(OutputFunctionCall(outfn, key));
}

/*
 * lion_index_facets(idx regclass, k int DEFAULT NULL, filter text DEFAULT
 * NULL, attno int2 DEFAULT 1, OUT key text, OUT count bigint): the k keys of
 * key column attno with the most rows - every key with a row when k is NULL
 * - that the filter keeps, largest count first and ties in the key's order.
 */
Datum
lion_index_facets(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	Oid			idxoid;
	AttrNumber	attno = PG_ARGISNULL(3) ? 1 : (AttrNumber) PG_GETARG_INT16(3);
	LionFacetRun run;
	LionFacetFilter ff;
	LionCountCall call;
	FmgrInfo	outfn;
	Oid			outfunc;
	bool		isvarlena;
	bool		any = true;
	int			nout;
	int			i;

	InitMaterializedSRF(fcinfo, 0);
	if (PG_ARGISNULL(0))
		PG_RETURN_VOID();
	idxoid = PG_GETARG_OID(0);

	memset(&run, 0, sizeof(run));
	memset(&ff, 0, sizeof(ff));
	run.limit = PG_ARGISNULL(1) ? -1 : PG_GETARG_INT32(1);
	if (!PG_ARGISNULL(1) && run.limit < 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("the number of facets must not be negative")));
	run.k = (run.limit > LION_FACET_K_MAX) ? -1 : run.limit;

	run.snapshot = GetActiveSnapshot();
	if (run.snapshot == NULL)
		elog(ERROR, "lion index count requires an active snapshot");

	/* as lion_index_count_group_stats() refuses it, before anything else */
	if (attno < 1)
	{
		char	   *idxname = get_rel_name(idxoid);

		if (idxname == NULL)
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_TABLE),
					 errmsg("relation with OID %u does not exist", idxoid)));
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("lion index \"%s\" has no key column %d",
						idxname, attno)));
	}

	/*
	 * The privileges, EXECUTE on count() and on the grouping equality, the
	 * collation, row-level security, snapshot eligibility and the predicate
	 * lock, as for lion_index_count_group_stats().
	 */
	lion_count_open_indexes(run.snapshot, 1, &idxoid, NULL, attno, true,
							&call);
	run.heap = call.heap;
	run.index = call.index[0];
	run.attno = attno;
	run.state = lion_index_column_state(run.index, attno);
	run.cache = lion_vis_cache_create(CurrentMemoryContext);
	run.percount = AllocSetContextCreate(CurrentMemoryContext,
										 "lion index facet count",
										 ALLOCSET_DEFAULT_SIZES);
	run.keycxt = AllocSetContextCreate(CurrentMemoryContext,
									   "lion index facet keys",
									   ALLOCSET_DEFAULT_SIZES);
	run.maxres = 64;
	run.res = (LionFacetKey *) palloc(sizeof(LionFacetKey) * run.maxres);
	if (run.k >= 0)
		run.heap_counts = (int64 *) palloc(sizeof(int64) * Max(run.k, 1));

	if (!PG_ARGISNULL(2))
		any = lion_facets_filter(&run, text_to_cstring(PG_GETARG_TEXT_PP(2)),
								 &ff);

	if (any && run.k != 0)
	{
		run.share = 1.0;
		if (run.hascopy && run.heap->rd_rel->reltuples > 0)
			run.share = Min(1.0, Max((double) run.copy.ntids /
									 run.heap->rd_rel->reltuples, 1e-6));
		run.batch = 1;
		if (run.hascopy)
		{
			run.batch = lion_count_groups_batch(run.index);
			run.images = (PGAlignedBlock *)
				palloc(sizeof(PGAlignedBlock) * run.batch);
		}

		if (run.k < 0)
		{
			/* every key: by the keys, or by the filter's rows when fewer */
			if (lion_facets_heap_ok(&run) &&
				(double) run.copy.ntids <
				(double) lion_facets_nentries(&run) * LION_FACET_KEY_ROWS)
			{
				run.heapread = true;
				lion_facets_heap(&run);
			}
			else
				lion_facets_walk(&run);
		}
		else
		{
			double		cap;

			/*
			 * As many candidates as the filter makes likely enough (§36): a
			 * filter that keeps a share s of the rows, spread evenly over the
			 * keys, leaves the k-th count about s of its bound, and the bounds
			 * after it must fall below that.
			 */
			cap = Min(2.0 * run.k / run.share, (double) LION_FACET_CAND_MAX);
			cap = Max(cap, (double) run.k + 64);
			cap = Min(cap, (double) MaxAllocSize / sizeof(LionFacetKey) / 4);
			if (!lion_facets_topk(&run, (int) cap))
			{
				/* not settled: every key whose bound may still pass */
				run.whole = true;
				for (i = 0; i < run.nres; i++)
					lion_facets_key_free(&run, &run.res[i]);
				run.nres = 0;
				run.nheap = 0;
				lion_facets_walk(&run);
			}
		}
	}

	elog(DEBUG1, "lion_index_facets(%s): %lld entries read, %lld keys counted%s, %lld rows read from the heap%s",
		 RelationGetRelationName(run.index), (long long) run.walked,
		 (long long) run.counted, run.whole ? " (walked whole)" : "",
		 (long long) run.fetched, run.heapread ? " (by the filter's rows)" : "");

	lion_facets_sort(&run);
	nout = (run.limit >= 0) ? (int) Min((int64) run.nres, run.limit) :
		run.nres;
	getTypeOutputInfo(run.state->typid, &outfunc, &isvarlena);
	fmgr_info(outfunc, &outfn);
	for (i = 0; i < nout; i++)
	{
		Datum		values[2];
		bool		nulls[2] = {false, false};

		if (run.res[i].isnull)
			nulls[0] = true;
		else
			values[0] = PointerGetDatum(lion_facets_key_text(&run,
															 run.res[i].key,
															 &outfn));
		values[1] = Int64GetDatum(run.res[i].count);
		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values,
							 nulls);
	}

	lion_facets_filter_end(&ff);
	if (run.hascopy)
		lion_posting_set_release(&run.copy);
	lion_vis_cache_destroy(run.cache);
	if (run.estate != NULL)
		FreeExecutorState(run.estate);
	MemoryContextDelete(run.percount);
	MemoryContextDelete(run.keycxt);
	lion_count_sql_close(&call);

	PG_RETURN_VOID();
}
