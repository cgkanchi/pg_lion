/*-------------------------------------------------------------------------
 *
 * lion_plan_match.c
 *		Which lion index can answer a clause, and what each WHERE clause is:
 *		the index matching and the clause analysis of the LionCount planner.
 *
 * Part of the LionCount custom scan: lion_customscan.h describes the node
 * and declares what its files share.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "lion_customscan.h"

/*
 * Peel binary-coercion relabels off an expression.  A varchar column
 * compared with a text constant arrives as RelabelType(Var) = Const, and the
 * lion index on that column is a text_ops index, so the relabelled form is
 * exactly what we want to match.
 */
Node *
lion_strip(Node *node)
{
	while (node != NULL && IsA(node, RelabelType))
		node = (Node *) ((RelabelType *) node)->arg;
	return node;
}

/*
 * Does this opfamily extract many keys from one value (DESIGN.md §17)?  The
 * presence of support function 2 is the same test lion_fill_state() makes.
 */
bool
lion_opfamily_is_multikey(Oid opfamily, Oid opcintype)
{
	return OidIsValid(get_opfamily_proc(opfamily, opcintype, opcintype,
										LION_EXTRACTVALUE_PROC));
}

/* ---------------------------------------------------------------------
 * Expression columns (DESIGN.md §41)
 *
 * The count path being planned names its expression columns once
 * (lion_vcol_collect()) and plans inside lion_vcol_enter() /
 * lion_vcol_leave(): everything below it - the clause analysis, the grouping
 * columns, the index matching, the estimates - then sees a Var of attno
 * LION_VCOL_ATTNO(i) where the query had the i'th expression, and asks
 * lion_vcol_expr() for the expression where a column's number is not enough:
 * which index answers it, and what the statistics know of it.  The scope is
 * this file's, as the planner's own state for one count path is that path's:
 * lion_try_count_path() enters it after deciding what relation it counts and
 * leaves it on every way out, errors included, and nothing it calls plans
 * another query.
 * ---------------------------------------------------------------------
 */
static LionVColScope lion_vcol_scope = {NIL, 0};

/*
 * idx's expression for key column i (0-based), relabels stripped, or NULL for
 * a column of the heap.  The planner hands the expressions in order, one per
 * key column whose indexkeys[] is 0.
 */
static Node *
lion_index_col_expr(IndexOptInfo *idx, int i)
{
	ListCell   *lc;
	int			j;

	if (idx->indexkeys[i] != 0)
		return NULL;
	lc = list_head(idx->indexprs);
	for (j = 0; j < i && lc != NULL; j++)
	{
		if (idx->indexkeys[j] == 0)
			lc = lnext(idx->indexprs, lc);
	}
	return (lc != NULL) ? lion_strip((Node *) lfirst(lc)) : NULL;
}

/*
 * The expressions of rel's lion indexes that a count can treat as columns:
 * each key column of a whole (non-partial) index that is an expression and
 * has a scalar opclass, whose entries are the expression's values, one per
 * row, and that reads a column: an index on `(1)` has one entry for every
 * row and stands for no expression a query writes.  Nor is a binary-coercible
 * cast of a column one, `(v::text)` of a varchar v: stripped of its relabel it
 * is the bare column, which is a column already, not an expression the plan
 * can carry (lion_count_priv_decode() refuses it).  A multi-key column's
 * entries are keys, which answer only its own
 * operators (DESIGN.md §17), and a partial index needs its predicate.  Each
 * expression once, relabels stripped, in the order the indexes come: the
 * position is its LION_VCOL_ATTNO().  The planner's index expressions are
 * normalised as the query's clauses are - folded and fixed up the same way -
 * so equal() is the test, as it is for core (match_index_to_operand()).
 */
List *
lion_vcol_collect(RelOptInfo *rel)
{
	Oid			amoid = lion_get_am_oid();
	List	   *vcols = NIL;
	ListCell   *lc;

	foreach(lc, rel->indexlist)
	{
		IndexOptInfo *idx = (IndexOptInfo *) lfirst(lc);
		int			i;

		if (idx->relam != amoid || idx->hypothetical ||
			idx->indpred != NIL || idx->indexprs == NIL)
			continue;
		for (i = 0; i < idx->nkeycolumns; i++)
		{
			Node	   *expr = lion_index_col_expr(idx, i);

			if (expr == NULL || IsA(expr, Var) || !contain_var_clause(expr) ||
				lion_opfamily_is_multikey(idx->opfamily[i],
										 idx->opcintype[i]) ||
				list_member(vcols, expr))
				continue;
			if (list_length(vcols) >= LION_MAX_VCOLS)
				return vcols;
			vcols = lappend(vcols, expr);
		}
	}
	return vcols;
}

/*
 * Plan with `vcols` (Vars of `rti`) as the expression columns, until
 * lion_vcol_leave() is handed back what this returns.
 */
LionVColScope
lion_vcol_enter(List *vcols, Index rti)
{
	LionVColScope saved = lion_vcol_scope;

	lion_vcol_scope.vcols = vcols;
	lion_vcol_scope.rti = rti;
	return saved;
}

void
lion_vcol_leave(LionVColScope saved)
{
	lion_vcol_scope = saved;
}

/* The expression of expression column attno, or NULL outside the scope. */
static Node *
lion_vcol_expr(AttrNumber attno)
{
	int			i = LION_VCOL_INDEX(attno);

	if (i < 0 || i >= list_length(lion_vcol_scope.vcols))
		return NULL;
	return (Node *) list_nth(lion_vcol_scope.vcols, i);
}

typedef struct LionVColCxt
{
	List	   *vcols;
	Index		rti;
} LionVColCxt;

/*
 * Every expression of node that is one of the expression columns, replaced
 * by its Var.  The outermost match wins, so an expression column inside
 * another is never reached: `(doc->>'a')::int` is one column when it is one,
 * and `doc->>'a'` only where it stands alone.  Constants and Vars are never
 * expressions of an index; a sub-select's are not this query level's.
 */
static Node *
lion_vcol_subst_mutator(Node *node, LionVColCxt *cx)
{
	if (node == NULL)
		return NULL;
	if (IsA(node, Query))
		return node;
	if (!IsA(node, Var) && !IsA(node, Const) && !IsA(node, Param) &&
		!IsA(node, RelabelType))
	{
		ListCell   *lc;
		int			i = 0;

		foreach(lc, cx->vcols)
		{
			Node	   *e = (Node *) lfirst(lc);

			if (equal(node, e))
				return (Node *) makeVar(cx->rti, LION_VCOL_ATTNO(i),
										exprType(e), exprTypmod(e),
										exprCollation(e), 0);
			i++;
		}
	}
	return expression_tree_mutator(node, lion_vcol_subst_mutator, cx);
}

/* node with the expression columns of the scope replaced by their Vars */
Node *
lion_vcol_subst(Node *node)
{
	return lion_vcol_subst_with(node, lion_vcol_scope.vcols,
								lion_vcol_scope.rti);
}

/* ... or of any list of them, as the plan carries it (LION_PRIV_VCOLS) */
Node *
lion_vcol_subst_with(Node *node, List *vcols, Index rti)
{
	LionVColCxt cx;

	if (vcols == NIL || node == NULL)
		return node;
	cx.vcols = vcols;
	cx.rti = rti;
	return lion_vcol_subst_mutator(node, &cx);
}

static Node *
lion_vcol_unvar_mutator(Node *node, void *context)
{
	if (node == NULL)
		return NULL;
	if (IsA(node, Var))
	{
		Var		   *v = (Var *) node;

		if (v->varlevelsup == 0 && LION_ATTNO_IS_VCOL(v->varattno) &&
			v->varno == (int) lion_vcol_scope.rti)
		{
			Node	   *e = lion_vcol_expr(v->varattno);

			if (e == NULL)
				elog(ERROR, "LionCount: expression column %d out of scope",
					 (int) v->varattno);
			return copyObject(e);
		}
		return node;
	}
	if (IsA(node, Query))
		return node;

	/* the clause of a restriction, which keeps none of its caches */
	if (IsA(node, RestrictInfo))
		return lion_vcol_unvar_mutator((Node *) ((RestrictInfo *) node)->clause,
									   context);
	return expression_tree_mutator(node, lion_vcol_unvar_mutator, context);
}

/*
 * What an expression refers to, as pull_var_clause() finds it with aggregates
 * kept whole and window functions recursed into - the Vars, Aggrefs,
 * GroupingFuncs and PlaceHolderVars - except that an expression column is one
 * reference, its expression, rather than the columns inside it.  A tuple
 * that holds these can evaluate the expression, and the node's tuple holds an
 * expression column where it could not hold the column it reads.
 */
typedef struct LionVColRefs
{
	List	   *vcols;
	List	   *refs;
} LionVColRefs;

static bool
lion_vcol_refs_walker(Node *node, LionVColRefs *cxt)
{
	ListCell   *lc;

	if (node == NULL)
		return false;
	if (IsA(node, Aggref) || IsA(node, GroupingFunc) ||
		IsA(node, PlaceHolderVar) || IsA(node, Var))
	{
		cxt->refs = lappend(cxt->refs, node);
		return false;
	}
	foreach(lc, cxt->vcols)
	{
		if (equal(lion_strip(node), lfirst(lc)))
		{
			cxt->refs = lappend(cxt->refs, node);
			return false;
		}
	}
	return expression_tree_walker(node, lion_vcol_refs_walker, (void *) cxt);
}

/* node's references, with the expression columns of the scope ... */
List *
lion_vcol_refs(Node *node)
{
	return lion_vcol_refs_with(node, lion_vcol_scope.vcols);
}

/* ... or of any list of them, as the plan carries it (LION_PRIV_VCOLS) */
List *
lion_vcol_refs_with(Node *node, List *vcols)
{
	LionVColRefs cxt;

	cxt.vcols = vcols;
	cxt.refs = NIL;
	(void) lion_vcol_refs_walker(node, &cxt);
	return cxt.refs;
}

/*
 * node with every expression column put back: what the statistics are asked
 * about.  An expression column has no pg_statistic row of its own as a
 * column, but core keeps one for each expression of an index and finds it
 * from the expression (examine_variable()), so the estimates of `doc->>'k'
 * = 'x'` are the ones core would make of the query.
 */
Node *
lion_vcol_unvar(Node *node)
{
	if (lion_vcol_scope.vcols == NIL || !lion_vcol_used(node))
		return node;
	return lion_vcol_unvar_mutator(node, NULL);
}

static bool
lion_vcol_used_walker(Node *node, void *context)
{
	if (node == NULL || IsA(node, Query))
		return false;
	if (IsA(node, Var))
		return LION_ATTNO_IS_VCOL(((Var *) node)->varattno);
	return expression_tree_walker(node, lion_vcol_used_walker, context);
}

/* Does node mention an expression column? */
bool
lion_vcol_used(Node *node)
{
	return lion_vcol_used_walker(node, NULL);
}

/*
 * Is key column col (0-based) of the index `index` the expression column
 * vcol (DESIGN.md §41)?  The executor's half of the test the planner made
 * (lion_find_roaring_index()): vcol is the plan's expression with its Vars
 * renumbered 1, as an index's own expressions are.
 */
bool
lion_index_col_is_vcol(Relation index, int col, Node *vcol)
{
	List	   *exprs;
	ListCell   *lc;
	int			j;

	if (index->rd_index->indkey.values[col] != 0)
		return false;
	exprs = RelationGetIndexExpressions(index);
	lc = list_head(exprs);
	for (j = 0; j < col && lc != NULL; j++)
	{
		if (index->rd_index->indkey.values[j] == 0)
			lc = lnext(exprs, lc);
	}
	return lc != NULL && equal(lion_strip((Node *) lfirst(lc)), vcol);
}

/*
 * Expression column vcol of relation relid as EXPLAIN and errors print it,
 * deparsed as core deparses an index's expression: its Vars are varno 1.
 */
char *
lion_vcol_name(Oid relid, Node *vcol)
{
	return deparse_expression(vcol,
							  deparse_context_for(get_rel_name(relid), relid),
							  false, false);
}

/*
 * A usable lion index on one plain column of rel, or NULL.  Only indexes
 * the planner put in rel->indexlist are considered, which already excludes
 * invalid ones (get_relation_info() skips !indisvalid).
 *
 * multikey selects between the two shapes of opclass, and the caller always
 * knows which one it needs: a multi-key index's ENTRIES are keys and not
 * column values, so it can answer `tags @> '{a}'` but can neither drive a
 * GROUP BY (the entries would be lexemes, not arrays) nor be summed over
 * (a row appears under each of its keys, so the sum of the entries is not
 * the number of rows - which is what DESIGN.md §14's sum-over-all rests on).
 *
 * *colp receives the INDEX COLUMN (1-based) that indexes `attno` (DESIGN.md
 * §24).  A multicolumn lion index holds each column's keys as an independent
 * set of entries, so ANY of its columns will do and the rest of this file
 * carries that number beside the index; colp may be NULL for a caller that
 * only asks whether such an index exists.
 */
IndexOptInfo *
lion_find_roaring_index(RelOptInfo *rel, AttrNumber attno, bool multikey,
					   AttrNumber *colp)
{
	Oid			amoid = lion_get_am_oid();
	Node	   *vcol = NULL;
	ListCell   *lc;

	/*
	 * An expression column (DESIGN.md §41) is answered by any key column whose
	 * expression is that one: the first, in index order, like a column.
	 */
	if (LION_ATTNO_IS_VCOL(attno))
	{
		vcol = lion_vcol_expr(attno);
		if (vcol == NULL)
			return NULL;
	}
	else if (attno <= 0)
		return NULL;			/* no expression's key column answers it */

	foreach(lc, rel->indexlist)
	{
		IndexOptInfo *idx = (IndexOptInfo *) lfirst(lc);
		int			i;

		if (idx->relam != amoid)
			continue;
		if (idx->hypothetical)
			continue;
		if (idx->indpred != NIL)
			continue;

		/*
		 * ANY key column, not just the first (DESIGN.md §24).  INCLUDE columns
		 * (ncolumns > nkeycolumns) cannot happen - amcaninclude is false - but
		 * the loop is bounded by nkeycolumns anyway, because an INCLUDE column
		 * has no opclass to ask about.
		 *
		 * An expression column has indexkeys[i] == 0, which no heap column's
		 * attno is, and answers only the expression column whose expression
		 * it is (DESIGN.md §41).  `indpred`: a partial index would need its
		 * predicate applied, which this node does not do.
		 */
		for (i = 0; i < idx->nkeycolumns; i++)
		{
			if (vcol != NULL ?
				!equal(lion_index_col_expr(idx, i), vcol) :
				idx->indexkeys[i] != attno)
				continue;
			if (lion_opfamily_is_multikey(idx->opfamily[i],
										 idx->opcintype[i]) != multikey)
				continue;

			if (colp != NULL)
				*colp = (AttrNumber) (i + 1);
			return idx;
		}
	}

	return NULL;
}

/*
 * The equality operator an index's opclass defines on its own key type: the
 * relation whose classes its entries are.  A scalar roaring opclass always
 * has it (the AM requires strategy 1), but an opfamily that only declares
 * cross-type members for (opcintype, opcintype) would not, and then nothing
 * below can be proved about the index.
 *
 * col is the index's KEY COLUMN (DESIGN.md §24): every column of a
 * multicolumn index has an opclass of its own, so every question about an
 * opclass has to name one.
 */
Oid
lion_index_equality_op(IndexOptInfo *idx, AttrNumber col)
{
	int			i = col - 1;

	return get_opfamily_member(idx->opfamily[i], idx->opcintype[i],
							   idx->opcintype[i], LION_STRAT_EQUAL);
}

/*
 * Does equality on this type imply that equal values have the same binary
 * representation?
 *
 * This is the question btree deduplication asks before it may replace one
 * tuple with another that compares equal (_bt_allequalimage() in
 * src/backend/access/nbtree/nbtutils.c), and it is exactly the question the
 * count pushdown has to ask before it prints a key an index stored instead of
 * a value a visible row holds: a posting set keeps ONE representative per
 * equality class, and if the type allows two equal values to look different
 * (citext 'Bob'/'BOB', numeric 1.0/1.00, a nondeterministic collation) that
 * representative may be a spelling no visible row contains (the 2026-09-20
 * review, finding 4).
 *
 * The test is the type's DEFAULT btree opclass (lookup_type_cache with
 * TYPECACHE_BTREE_OPFAMILY, the same family SortGroupClause.eqop comes from),
 * its BTEQUALIMAGE_PROC support function, called under the collation the
 * index compared its keys with - which is how btequalimage/btvarstrequalimage
 * decide determinism.  No support function means no (that is btree's rule as
 * well).
 *
 * That answer is necessary but not sufficient.  equalimage promises that
 * equal values are "interchangeable without loss of semantic information",
 * which is what deduplication needs, and bpchar - whose trailing blanks carry
 * no meaning to it - registers btvarstrequalimage although 'a   ' = 'a' and
 * bpcharout prints the blanks: an entry indexed as 'a   ' then printed a
 * deleted row's spelling for a visible 'a' (the 2026-09-23 review).  An
 * extension's function is only its author's word, on the same weaker
 * promise.  So the type also has to be one of the core types below, each of
 * whose equality compares every byte its output function prints: fixed-width
 * integers and the date/time types (timetz compares the zone as well as the
 * instant), uuid, bytea, bit strings (their lengths too), MAC addresses,
 * inet (family, prefix length and the whole address; cidr is indexed as inet),
 * enums, and text and name, whose equality under a deterministic collation -
 * which the support function still decides - is a byte comparison.  bpchar
 * is left out on purpose; numeric, the floats (-0 and 0), interval ('1 day'
 * and '24 hours'), jsonb, arrays and ranges have no support function and are
 * refused either way.  A domain is indexed under its base type's opclass.
 */
bool
lion_type_equalimage(Oid typid, Oid collation)
{
	TypeCacheEntry *typentry;
	Oid			proc;

	switch (typid)
	{
		case BOOLOID:
		case CHAROID:
		case NAMEOID:
		case INT2OID:
		case INT4OID:
		case INT8OID:
		case OIDOID:
		case OIDVECTOROID:
		case XID8OID:
		case MONEYOID:
		case PG_LSNOID:
		case TEXTOID:
		case BYTEAOID:
		case BITOID:
		case VARBITOID:
		case DATEOID:
		case TIMEOID:
		case TIMETZOID:
		case TIMESTAMPOID:
		case TIMESTAMPTZOID:
		case UUIDOID:
		case INETOID:
		case MACADDROID:
		case MACADDR8OID:
		case ANYENUMOID:
			break;
		default:
			return false;
	}

	typentry = lookup_type_cache(typid, TYPECACHE_BTREE_OPFAMILY);
	if (!OidIsValid(typentry->btree_opf) || !OidIsValid(typentry->btree_opintype))
		return false;

	proc = get_opfamily_proc(typentry->btree_opf, typentry->btree_opintype,
							 typentry->btree_opintype, BTEQUALIMAGE_PROC);
	if (!OidIsValid(proc))
		return false;

	/*
	 * A collatable type's support function insists on being told a collation
	 * (check_collation_set() in btvarstrequalimage()), so a key type that has
	 * one but an index that does not is simply refused here.
	 */
	if (OidIsValid(get_typcollation(typentry->btree_opintype)) &&
		!OidIsValid(collation))
		return false;

	return DatumGetBool(OidFunctionCall1Coll(proc, collation,
											 ObjectIdGetDatum(typentry->btree_opintype)));
}

/*
 * May the node print a value taken from this index's stored keys?
 *
 * Two things have to hold, and both are properties of the index rather than
 * of the query, so they are checked once per relation (per partition: nothing
 * stops two partitions from using different opclasses):
 *
 *	- the index's own equality has to BE the type's equality, so that "in the
 *	  same entry" implies "equal" in the sense the next test is about.  The
 *	  index groups rows by strategy 1 of its opfamily, which is free to be a
 *	  coarser relation than the type's default equality (the review's
 *	  lower()-based text opclass is a valid opclass and a coarser one);
 *	- and equality has to imply an identical representation, or the stored
 *	  representative may be a spelling no visible row has.
 *
 * When either fails the query may still be pushed down as a COUNT: counting
 * an equality class needs no representative.  Only value-producing pushdowns
 * - the GROUP BY column in the output, or a column a WHERE clause pins whose
 * value the target list prints - come through here.
 */
bool
lion_index_can_emit_value(IndexOptInfo *idx, AttrNumber col)
{
	Oid			typid = idx->opcintype[col - 1];
	Oid			idxeq = lion_index_equality_op(idx, col);
	TypeCacheEntry *typentry;
	Oid			typeeq;

	if (!OidIsValid(idxeq))
		return false;

	typentry = lookup_type_cache(typid, TYPECACHE_BTREE_OPFAMILY);
	if (!OidIsValid(typentry->btree_opf) || !OidIsValid(typentry->btree_opintype))
		return false;
	typeeq = get_opfamily_member(typentry->btree_opf, typentry->btree_opintype,
								 typentry->btree_opintype,
								 BTEqualStrategyNumber);
	if (!OidIsValid(typeeq) || typeeq != idxeq)
		return false;

	return lion_type_equalimage(typid, idx->indexcollations[col - 1]);
}

/*
 * The same, but for a column a particular clause is applied to.
 *
 * For a scalar clause (`=`, `= ANY`, a null test) opno has to be strategy 1
 * of the index's opfamily (the index's opfamily and the operator Oid, so
 * cross-type integer equality is fine), and cmptype - the type the column is
 * compared with, InvalidOid for a null test - has to be one the opfamily can
 * compare with the indexed type and can hash, or the lookup in lion_set.c
 * would fail at run time.
 *
 * For a multi-key clause (DESIGN.md §17) the index must be a multi-key one
 * whose opfamily gives opno the strategy the planner decided on, and whose
 * extractQuery function is the very one the plan-time extraction used: the
 * plan was only made because that function called the query exact, and a
 * different function might not.  An OR's leaf the posting sets only bound
 * (`positions`) needs a column that stores positions, which decide it.
 *
 * Every partition is checked separately, because nothing stops one of them
 * from carrying a lion index built with a different opclass.
 */
IndexOptInfo *
lion_match_index(RelOptInfo *rel, AttrNumber attno, int kind, Oid opno,
				Oid cmptype, StrategyNumber strategy, Oid extractquery,
				Oid exprcoll, bool positions, AttrNumber *colp)
{
	bool		multikey = (kind == LION_CLAUSE_MULTI);
	AttrNumber	col = 1;
	IndexOptInfo *idx = lion_find_roaring_index(rel, attno, multikey, &col);
	int			i;

	if (idx == NULL)
		return NULL;
	i = col - 1;				/* the KEY COLUMN's opclass (DESIGN.md §24) */

	/*
	 * The planner's own rule, IndexCollMatchesExprColl(): a collation-
	 * sensitive clause may only use an index built under that collation.
	 * The index hashed and compared its keys with its own collation and the
	 * count never rechecks the predicate, so a mismatch (say a case-
	 * insensitive index under a case-sensitive query) would count rows the
	 * query does not select.  An index with no collation compared nothing
	 * under one, and matches any clause, as in the planner's rule
	 * (jsonb_contains_ops under `doc ? 'k'`, whose text has one).
	 */
	if (OidIsValid(exprcoll) && OidIsValid(idx->indexcollations[i]) &&
		idx->indexcollations[i] != exprcoll)
		return NULL;

	if (multikey)
	{
		if (get_op_opfamily_strategy(opno, idx->opfamily[i]) != strategy)
			return NULL;
		if (get_opfamily_proc(idx->opfamily[i], idx->opcintype[i],
							  idx->opcintype[i],
							  LION_EXTRACTQUERY_PROC) != extractquery)
			return NULL;
		if (positions && !lion_index_stores_positions(idx, col))
			return NULL;
		if (colp != NULL)
			*colp = col;
		return idx;
	}

	/*
	 * A class declared on a polymorphic type (enum_ops is FOR TYPE anyenum)
	 * names its members on that type - (anyenum, anyenum), and the hash proc
	 * for it - while the value a clause compares the column with is of the
	 * column's own enum: the constant of `k = 'x'`, the elements of `k IN
	 * (...)`, a range bound.  That is one of the class's own values and not a
	 * cross-type search, so it is resolved to the class's type before any
	 * member is looked up, for EVERY kind of clause.  Only the range
	 * comparison of DESIGN.md §28 used to do this, and `k = 'x'` and `k IN
	 * (...)` on an enum column asked for an (anyenum, mood) member that no
	 * family has: the index was declined and the node never reached, on the
	 * very columns it is best at.  The test is the one lion_probe_init()
	 * makes at run time (lion_type_is_column(), lion_set.c): the same BASE
	 * type as the key column's own - a domain over the enum is its enum, and
	 * a different enum, whose OIDs mean nothing to this column, is not.
	 */
	if (OidIsValid(cmptype) && IsPolymorphicType(idx->opcintype[i]))
	{
		Oid			coltype = get_atttype(idx->indexoid, col);

		if (OidIsValid(coltype) &&
			getBaseType(cmptype) == getBaseType(coltype))
			cmptype = idx->opcintype[i];
	}

	/*
	 * A range comparison (DESIGN.md §28) has to be one of the index's range
	 * strategies, with the ordering its walk needs - proc 4 for the pair - in
	 * the same family; lionvalidate() insists on both together, and this is
	 * where a catalogue that disagrees is declined rather than walked
	 * linearly.  The equality and hash checks below apply to it as well: the
	 * executor resolves the bound's comparison through lion_probe_init(),
	 * which needs them.
	 */
	if (kind == LION_CLAUSE_RANGE || kind == LION_CLAUSE_RANGESRC)
	{
		if (!LION_STRAT_IS_RANGE(get_op_opfamily_strategy(opno,
														  idx->opfamily[i])))
			return NULL;
		if (!OidIsValid(get_opfamily_proc(idx->opfamily[i], idx->opcintype[i],
										  cmptype, LION_CMP_PROC)))
			return NULL;
	}
	else if (kind == LION_CLAUSE_NE)
	{
		/*
		 * `<>` (DESIGN.md §35) has to be the index's strategy 10 for the pair:
		 * the promise that c's entry is exactly the rows `<>` rejects.  The
		 * entry is looked up as an equality's is, which the checks below make
		 * possible.
		 */
		if (get_op_opfamily_strategy(opno, idx->opfamily[i]) != LION_STRAT_NE)
			return NULL;
	}
	else if (OidIsValid(opno) &&
			 get_op_opfamily_strategy(opno, idx->opfamily[i]) != LION_STRAT_EQUAL)
		return NULL;

	if (OidIsValid(cmptype))
	{
		if (!OidIsValid(get_opfamily_member(idx->opfamily[i],
											idx->opcintype[i], cmptype,
											LION_STRAT_EQUAL)))
			return NULL;
		if (!OidIsValid(get_opfamily_proc(idx->opfamily[i], cmptype, cmptype,
										  LION_HASH_PROC)))
			return NULL;
	}

	if (colp != NULL)
		*colp = col;
	return idx;
}

/*
 * The strategy number an operator has in some roaring opfamily, with that
 * family and the type its members are declared on.  Returns 0 when no roaring
 * family knows the operator.
 *
 * The clause analysis has to tell a multi-key clause from an equality one
 * BEFORE any index has been matched, because the parent of a partitioned
 * table has no index list of its own (DESIGN.md §16) and the answer decides
 * what the clause even means.  Taking it from the operator rather than from
 * an index is safe because lion_match_index() checks the strategy again
 * against the index that will really answer the clause, per partition.
 */
static StrategyNumber
lion_op_roaring_strategy(Oid opno, Oid *opfamily, Oid *lefttype)
{
	Oid			amoid = lion_get_am_oid();
	CatCList   *catlist;
	StrategyNumber result = 0;
	int			i;

	*opfamily = InvalidOid;
	*lefttype = InvalidOid;

	catlist = SearchSysCacheList1(AMOPOPID, ObjectIdGetDatum(opno));
	for (i = 0; i < catlist->n_members; i++)
	{
		Form_pg_amop amop =
			(Form_pg_amop) GETSTRUCT(&catlist->members[i]->tuple);

		if (amop->amopmethod != amoid || amop->amoppurpose != AMOP_SEARCH)
			continue;

		result = amop->amopstrategy;
		*opfamily = amop->amopfamily;
		*lefttype = amop->amoplefttype;
		break;
	}
	ReleaseSysCacheList(catlist);

	return result;
}

/*
 * Does key column col of idx store positions (DESIGN.md §17)?  Its opclass
 * has support function 5 and the index was built with store_positions = true,
 * which only its meta page knows (LION_META_POSITIONS) - the reloption may
 * have been changed by ALTER INDEX since.
 */
bool
lion_index_stores_positions(IndexOptInfo *idx, AttrNumber col)
{
	Relation	indexrel;
	LionIndexState *ix;
	bool		result;

	if (col < 1 || col > idx->nkeycolumns)
		return false;
	indexrel = index_open(idx->indexoid, AccessShareLock);
	ix = lion_get_index_state(indexrel);
	result = col <= ix->ncolumns && ix->cols[col - 1].positions;
	index_close(indexrel, AccessShareLock);

	return result;
}

/*
 * Could a lion opfamily that has opno, extractquery as its extractQuery and
 * stored positions decide the superset of `col opno con` exactly
 * (lion_query_posexact())?  Any one: lion_match_index() then takes only an
 * index of such a family.
 */
static bool
lion_op_posexact(Oid opno, Oid extractquery, Const *con)
{
	Oid			amoid = lion_get_am_oid();
	CatCList   *catlist;
	bool		result = false;
	int			i;

	catlist = SearchSysCacheList1(AMOPOPID, ObjectIdGetDatum(opno));
	for (i = 0; i < catlist->n_members && !result; i++)
	{
		Form_pg_amop amop =
			(Form_pg_amop) GETSTRUCT(&catlist->members[i]->tuple);

		if (amop->amopmethod != amoid || amop->amoppurpose != AMOP_SEARCH ||
			get_opfamily_proc(amop->amopfamily, amop->amoplefttype,
							  amop->amoplefttype,
							  LION_EXTRACTQUERY_PROC) != extractquery)
			continue;
		result = lion_query_posexact(amop->amopfamily, amop->amoplefttype,
									 opno, con->constvalue, con->constcollid);
	}
	ReleaseSysCacheList(catlist);

	return result;
}

/*
 * How extractquery answers the query in con, as the executor would ask it:
 * lion_extract_query()'s mode for a literal, and with superset
 * lion_extract_query_superset()'s, which is what it asks of a query it only
 * has at run time (DESIGN.md §17, "A query known only at run time").
 */
static LionQueryMode
lion_multikey_query_mode(Oid extractquery, StrategyNumber strategy,
						 Const *con, bool superset)
{
	FmgrInfo	flinfo;
	LionQuery	q;
	LionState	state;
	MemoryContext cxt;
	MemoryContext oldcxt;
	LionQueryMode mode;
	Datum		query;

	if (con->constisnull)
		return LION_QMODE_NONE; /* every operator involved is strict */

	/*
	 * lion_extract_query() wants an LionState, but only for the extractQuery
	 * FmgrInfo and the collation; nothing here touches an index.  The
	 * collation of a query is the clause's own, which for the collatable key
	 * types the multi-key classes use (text lexemes, text array elements) is
	 * what the extraction functions ignore anyway - they take the query
	 * apart, they do not compare it.
	 */
	cxt = AllocSetContextCreate(CurrentMemoryContext,
								"roaring count query extract",
								ALLOCSET_SMALL_SIZES);
	oldcxt = MemoryContextSwitchTo(cxt);

	memset(&state, 0, sizeof(state));
	state.multikey = true;
	state.collation = con->constcollid;
	fmgr_info(extractquery, &flinfo);
	state.extractquery = flinfo;

	/*
	 * The executor answers a prefix lexeme as the OR of the index's lexemes
	 * that have it (lion_tsquery_expand_prefixes()), which combines like the
	 * one lexeme the stand-in leaves (DESIGN.md §17, "Prefix lexemes").
	 */
	query = con->constvalue;
	if (strategy == LION_STRAT_MATCH)
		query = lion_tsquery_strip_prefixes(query);

	if (superset)
		lion_extract_query_superset(&state, query, strategy, &q);
	else
		lion_extract_query(&state, query, strategy, &q);
	mode = q.mode;

	MemoryContextSwitchTo(oldcxt);
	MemoryContextDelete(cxt);

	return mode;
}

/*
 * Extract a multi-key query at plan time and say whether the posting sets can
 * answer it exactly (DESIGN.md §17).  Only then is a LITERAL query pushed
 * down: an ALL-mode query would need every row rechecked against the heap,
 * which is what the ordinary bitmap plan already does and does better.
 *
 * *extractquery receives the support function used, which lion_match_index()
 * then insists on finding on every index that will answer the clause, so that
 * the run-time extraction cannot come out differently from this one.
 */
static bool
lion_multikey_query_is_exact(Oid opfamily, Oid lefttype,
							StrategyNumber strategy, Const *con,
							Oid *extractquery)
{
	*extractquery = get_opfamily_proc(opfamily, lefttype, lefttype,
									  LION_EXTRACTQUERY_PROC);
	if (!OidIsValid(*extractquery))
		return false;

	return lion_multikey_query_mode(*extractquery, strategy, con,
									false) == LION_QMODE_KEYS;
}

/*
 * The walker of lion_is_value_expr(): true at anything that makes an
 * expression something other than one value per scan - a column of this
 * query level or of any other, a subquery, an aggregate or a window function
 * - or that the node's ExprContext has nothing to evaluate with (a Param
 * other than a statement's own or an exec one).
 */
static bool
lion_not_value_walker(Node *node, void *context)
{
	if (node == NULL)
		return false;
	if (IsA(node, Var) || IsA(node, PlaceHolderVar))
		return true;
	if (IsA(node, Param))
	{
		Param	   *p = (Param *) node;

		return (p->paramkind != PARAM_EXTERN && p->paramkind != PARAM_EXEC);
	}
	if (IsA(node, SubLink) || IsA(node, SubPlan) ||
		IsA(node, AlternativeSubPlan))
		return true;
	if (IsA(node, Aggref) || IsA(node, WindowFunc) ||
		IsA(node, GroupingFunc))
		return true;
	if (IsA(node, CurrentOfExpr))
		return true;
	return expression_tree_walker(node, lion_not_value_walker, context);
}

/*
 * Is this expression a value the node can compare a column with - a literal,
 * or an expression it evaluates once at the start of the scan (DESIGN.md
 * §10)?
 *
 * A Param is accepted wherever a Const is, which is what lets a prepared
 * statement's GENERIC plan reach the pushdown: the planner leaves `k = $1` as
 * a Param, the cost model uses its default selectivity, and the executor
 * evaluates it through the node's own ExprContext.  Both parameter kinds
 * qualify: PARAM_EXTERN for a prepared statement's own parameters and
 * PARAM_EXEC for the ones a nested loop or a LATERAL reference supplies,
 * which change between rescans.
 *
 * So is any other expression that the executor would take as an index scan's
 * RUN-TIME key (ExecIndexBuildScanKeys()): no Var of any level, no volatile
 * function, no subquery, no aggregate or window function.  That is what a
 * time window is written as - `ts >= now() - interval '90 days'`, `d >=
 * current_date - 30`, `d = $1::date + 1` in a generic plan - and none of it
 * is a Const or a Param: eval_const_expressions() folds only immutable
 * functions, and now(), current_date and timestamptz arithmetic (which
 * depends on the time zone) are stable.  A stable expression has one value
 * throughout a statement, so evaluating it once per scan, beside the Params
 * (lion_eval_clause_values()), means what evaluating it per row means; a
 * volatile one does not (`k = random()`), and is declined.  At plan time its
 * value is only estimated, which clause_selectivity() does itself through
 * estimate_expression_value(); a cached generic plan evaluates it anew at
 * every execution.
 *
 * An ArrayExpr is accepted for the array of an IN list, because that is the
 * shape `k IN ($1, $2)` keeps in a generic plan, when each of its elements is
 * a value by the same rule: the node evaluates the array ONCE per scan.
 */
static bool
lion_is_value_expr(Node *node, bool allow_array_expr)
{
	if (node == NULL)
		return false;
	if (IsA(node, Const))
		return true;
	if (IsA(node, ArrayExpr))
	{
		ArrayExpr  *a = (ArrayExpr *) node;
		ListCell   *lc;

		if (!allow_array_expr || a->multidims || a->elements == NIL)
			return false;
		foreach(lc, a->elements)
		{
			if (!lion_is_value_expr(lion_strip((Node *) lfirst(lc)), false))
				return false;
		}
		return true;
	}
	if (expression_returns_set(node) || lion_not_value_walker(node, NULL))
		return false;
	return !contain_volatile_functions(node);
}

/*
 * Number of elements of a Const array, or -1 when it is not a plain array.
 */
static int
lion_array_const_nelems(Const *con)
{
	ArrayType  *arr;
	int			nelems;

	if (con->constisnull)
		return -1;
	if (!OidIsValid(get_element_type(con->consttype)))
		return -1;

	arr = DatumGetArrayTypeP(con->constvalue);
	if (ARR_NDIM(arr) == 0)
		nelems = 0;
	else if (ARR_NDIM(arr) != 1)
		nelems = -1;
	else
		nelems = ARR_DIMS(arr)[0];

	if ((Pointer) arr != DatumGetPointer(con->constvalue))
		pfree(arr);

	return nelems;
}

/*
 * Is this clause a test of a boolean column by itself, which core's
 * match_boolean_index_clause() hands an index scan as `col = true` or `col =
 * false`?  `col`, `NOT col`, `col IS TRUE` and `col IS FALSE` are, and set
 * *var and *value; eval_const_expressions() has already folded `col = true`
 * into the first and `col = false` and `col <> true` into the second, so no
 * boolean equality ever reaches the pushdown as an OpExpr.  All four are
 * false for NULL, as `=` is.  A domain over boolean is its base type here as
 * everywhere: the WHERE clause relabels it to boolean, and bool_ops answers.
 */
static bool
lion_boolean_eq_test(Node *clause, Var **var, bool *value)
{
	Node	   *arg = clause;
	bool		val = true;

	if (IsA(clause, BoolExpr))
	{
		BoolExpr   *b = (BoolExpr *) clause;

		if (b->boolop != NOT_EXPR || list_length(b->args) != 1)
			return false;
		arg = (Node *) linitial(b->args);
		val = false;
	}
	else if (IsA(clause, BooleanTest))
	{
		BooleanTest *bt = (BooleanTest *) clause;

		if (bt->booltesttype == IS_TRUE)
			val = true;
		else if (bt->booltesttype == IS_FALSE)
			val = false;
		else
			return false;
		arg = (Node *) bt->arg;
	}

	arg = lion_strip(arg);
	if (arg == NULL || !IsA(arg, Var) ||
		getBaseType(((Var *) arg)->vartype) != BOOLOID)
		return false;

	*var = (Var *) arg;
	*value = val;
	return true;
}

/*
 * `col IS NOT TRUE` and `col IS NOT FALSE` of a boolean column hold for the
 * NULL rows as well, so they are no equality: they are the OR of two clauses
 * the posting sets answer, `col = false OR col IS NULL` and `col = true OR
 * col IS NULL`, and that OR is returned for the machinery of DESIGN.md §19 to
 * take apart like any other.  NULL for every other clause.
 */
BoolExpr *
lion_boolean_not_test(Node *clause)
{
	BooleanTest *bt;
	Node	   *arg;
	NullTest   *nt;
	Expr	   *eq;

	if (clause == NULL || !IsA(clause, BooleanTest))
		return NULL;
	bt = (BooleanTest *) clause;
	if (bt->booltesttype != IS_NOT_TRUE && bt->booltesttype != IS_NOT_FALSE)
		return NULL;
	arg = lion_strip((Node *) bt->arg);
	if (arg == NULL || !IsA(arg, Var) ||
		getBaseType(((Var *) arg)->vartype) != BOOLOID)
		return NULL;

	eq = make_opclause(BooleanEqualOperator, BOOLOID, false, (Expr *) arg,
					   (Expr *) makeBoolConst(bt->booltesttype == IS_NOT_FALSE,
											  false),
					   InvalidOid, InvalidOid);
	nt = makeNode(NullTest);
	nt->arg = (Expr *) arg;
	nt->nulltesttype = IS_NULL;
	nt->argisrow = false;
	nt->location = -1;

	return (BoolExpr *) makeBoolExpr(OR_EXPR, list_make2(eq, nt), -1);
}

/*
 * `tsv @@ ANY (array)` - a multi-key operator (DESIGN.md §17) over a literal
 * array, which only a query type that is not itself an array can have -
 * holds where one element's query does: they are the OR of one clause per element, which is returned for the
 * machinery of DESIGN.md §19 to take apart like any other.  A NULL element
 * holds nowhere and is left out.  NULL for every other clause, and for an
 * empty array or one of more elements than an OR may have leaves.
 */
Node *
lion_multikey_any_as_or(Node *clause)
{
	ScalarArrayOpExpr *saop;
	Node	   *left;
	Node	   *right;
	Const	   *arrc;
	Oid			opfamily;
	Oid			lefttype;
	StrategyNumber strat;
	Oid			elemtype;
	int16		elemlen;
	bool		elembyval;
	char		elemalign;
	ArrayType  *arr;
	Datum	   *elems;
	bool	   *nulls;
	int			nelems;
	List	   *args = NIL;
	int			i;

	if (clause == NULL || !IsA(clause, ScalarArrayOpExpr))
		return NULL;
	saop = (ScalarArrayOpExpr *) clause;
	if (!saop->useOr || list_length(saop->args) != 2 || !op_strict(saop->opno))
		return NULL;
	left = lion_strip((Node *) linitial(saop->args));
	right = lion_strip((Node *) lsecond(saop->args));
	if (left == NULL || right == NULL || !IsA(left, Var) || !IsA(right, Const))
		return NULL;
	strat = lion_op_roaring_strategy(saop->opno, &opfamily, &lefttype);
	if (!LION_STRAT_IS_MULTI_SEARCH(strat))
		return NULL;
	arrc = (Const *) right;
	nelems = lion_array_const_nelems(arrc);
	if (nelems <= 0 || nelems > LION_MAX_ARRAY_ELEMS)
		return NULL;

	elemtype = get_element_type(arrc->consttype);
	get_typlenbyvalalign(elemtype, &elemlen, &elembyval, &elemalign);
	arr = DatumGetArrayTypeP(arrc->constvalue);
	deconstruct_array(arr, elemtype, elemlen, elembyval, elemalign,
					  &elems, &nulls, &nelems);
	for (i = 0; i < nelems; i++)
	{
		Const	   *elem;

		if (nulls[i])
			continue;
		elem = makeConst(elemtype, -1, arrc->constcollid, elemlen,
						 elembyval ? elems[i] :
						 datumCopy(elems[i], elembyval, elemlen),
						 false, elembyval);
		args = lappend(args,
					   make_opclause(saop->opno, BOOLOID, false,
									 (Expr *) linitial(saop->args),
									 (Expr *) elem, InvalidOid,
									 saop->inputcollid));
	}
	if (args == NIL)
		return NULL;
	if (list_length(args) == 1)
		return (Node *) linitial(args);
	return (Node *) makeBoolExpr(OR_EXPR, args, -1);
}

/*
 * The walker of lion_or_arms(): node in disjunctive normal form, as a list of
 * arms that are each the list of their leaves, with the number of leaves of
 * them all in *nleaves; NIL once that number would pass LION_MAX_ARRAY_ELEMS.
 */
static List *
lion_or_dnf(Node *node, int *nleaves)
{
	BoolExpr   *orform = lion_boolean_not_test(node);
	Node	   *anyform = lion_multikey_any_as_or(node);
	List	   *result;
	double		total;
	ListCell   *lc;

	check_stack_depth();

	if (orform != NULL)
		node = (Node *) orform;
	else if (anyform != NULL)
		node = anyform;

	if (IsA(node, BoolExpr) && ((BoolExpr *) node)->boolop == OR_EXPR)
	{
		result = NIL;
		total = 0;
		foreach(lc, ((BoolExpr *) node)->args)
		{
			int			n;
			List	   *sub = lion_or_dnf((Node *) lfirst(lc), &n);

			if (sub == NIL)
				return NIL;
			total += n;
			if (total > LION_MAX_ARRAY_ELEMS)
				return NIL;
			result = list_concat(result, sub);
		}
		*nleaves = (int) total;
		return result;
	}

	if (IsA(node, BoolExpr) && ((BoolExpr *) node)->boolop == AND_EXPR)
	{
		/* one empty arm, which the first term's arms extend */
		result = list_make1(NIL);
		total = 0;
		foreach(lc, ((BoolExpr *) node)->args)
		{
			int			n;
			List	   *sub = lion_or_dnf((Node *) lfirst(lc), &n);
			List	   *product = NIL;
			ListCell   *la;
			ListCell   *lb;

			if (sub == NIL)
				return NIL;

			/* every arm so far ANDed with every arm of this term */
			total = total * list_length(sub) + (double) n * list_length(result);
			if (total > LION_MAX_ARRAY_ELEMS)
				return NIL;
			foreach(la, result)
			{
				foreach(lb, sub)
					product = lappend(product,
									  list_concat_copy((List *) lfirst(la),
													   (List *) lfirst(lb)));
			}
			result = product;
		}
		*nleaves = (int) total;
		return result;
	}

	/* a leaf, for lion_analyze_leaf() to accept or decline */
	*nleaves = 1;
	return list_make1(list_make1(node));
}

/*
 * An OR restriction as the arms of one union (DESIGN.md §19): a list of arms,
 * each the list of the clauses it ANDs, or NIL when it is too big to count.
 *
 * The OR machinery takes an OR of ANDs of leaves, one level deep, because
 * that is what a source's tree is built from (lion_locate_or()).  Anything
 * nested deeper is DISTRIBUTED into that shape: `(a AND (b OR c)) OR d` is
 * `(a AND b) OR (a AND c) OR d`, and `(a AND flag IS NOT TRUE) OR d` - whose
 * `flag IS NOT TRUE` is `flag = false OR flag IS NULL` (lion_boolean_not_test())
 * and used to decline the whole query - is `(a AND flag = false) OR (a AND
 * flag IS NULL) OR d`.  The union of the arms is the same set of rows.
 *
 * Distributing repeats a term in every arm it is distributed into, and each
 * repetition is a leaf of its own: its own lookup, priced as such, and its own
 * posting set in the union.  An AND of k two-way ORs is 2^k arms of k leaves.
 * So the leaves of the result are bounded by the number an IN list's sets are
 * (LION_MAX_ARRAY_ELEMS, DESIGN.md §15), for the same reason: every set of the
 * union may hold a buffer pin for as long as the node runs (§9), and an OR's
 * leaves are never materialized (§19).  Past it the query is declined.
 */
List *
lion_or_arms(Node *clause)
{
	int			nleaves;

	return lion_or_dnf(clause, &nleaves);
}

/*
 * Is this clause one the posting sets can answer, and on a plain column of
 * rti?  Fills *out and returns true, or returns false and leaves the caller
 * to decline the whole query.
 *
 * allow_negated says whether `col IS NOT NULL` - the one clause kind that
 * subtracts rather than selects (DESIGN.md §14) - is acceptable here.  Under
 * an OR it is not: the union of the arms would have to be the union of one
 * arm's complement with the others', and the complement of a posting set is
 * not a posting set (DESIGN.md §19).
 *
 * allow_range says the same of a range comparison (DESIGN.md §28), which
 * bounds the entry walk that drives the count or, on another column, is a
 * range taken as a source (§32).  Every caller allows it now - an OR's arm
 * takes a range as a source, the rows the union of its sets holds - and it is
 * the caller that decides which of the two a range is.
 *
 * allow_recheck says the same of a multi-key clause whose query the node only
 * has at run time (DESIGN.md §17, "A query known only at run time"), which may
 * turn out to need every candidate rechecked in the heap.  The recheck tests
 * the clause alone, which is right for a clause the other sources are ANDed
 * with; a leaf of an OR is not tested alone - the row passes when ANY arm
 * holds, and the other arms' leaves are answered from posting sets and never
 * evaluated - so under an OR the query has to be a literal the posting sets
 * answer exactly, as before.
 */
bool
lion_analyze_leaf(PlannerInfo *root, Node *clause, Index rti,
				 bool allow_negated, bool allow_range, bool allow_recheck,
				 LionLeafInfo *out)
{
	Var		   *boolvar;
	bool		boolval;

	memset(out, 0, sizeof(LionLeafInfo));
	out->opno = InvalidOid;
	out->cmptype = InvalidOid;
	out->extractquery = InvalidOid;
	out->collation = InvalidOid;

	if (clause == NULL)
		return false;

	if (lion_boolean_eq_test(clause, &boolvar, &boolval))
	{
		/*
		 * A boolean column by itself (lion_boolean_eq_test()): the equality
		 * `col = true` or `col = false` it stands for, with bool_ops' own
		 * strategy 1.  The Const is the clause's value like any literal's, so
		 * EXPLAIN prints `flag = true` as core prints the index condition,
		 * and the cost model is handed that OpExpr rather than the bare
		 * column, so that everything it asks of an equality is asked of
		 * this one.
		 */
		out->var = boolvar;
		out->val = (Node *) makeBoolConst(boolval, false);
		out->opno = BooleanEqualOperator;
		out->cmptype = BOOLOID;
		out->strategy = LION_STRAT_EQUAL;
		out->kind = LION_CLAUSE_EQ;
		out->costclause = (Node *) make_opclause(BooleanEqualOperator, BOOLOID,
												 false, (Expr *) boolvar,
												 (Expr *) out->val,
												 InvalidOid, InvalidOid);
	}
	else if (IsA(clause, OpExpr))
	{
		OpExpr	   *op = (OpExpr *) clause;
		Node	   *left;
		Node	   *right;
		Oid			opfamily;
		Oid			lefttype;

		if (list_length(op->args) != 2)
			return false;
		if (!op_strict(op->opno))
			return false;

		left = lion_strip((Node *) linitial(op->args));
		right = lion_strip((Node *) lsecond(op->args));
		if (left == NULL || right == NULL)
			return false;

		out->strategy = lion_op_roaring_strategy(op->opno, &opfamily, &lefttype);

		if (out->strategy == LION_STRAT_EQUAL)
		{
			/* Equality commutes, so either side may hold the column. */
			if (IsA(left, Var) && lion_is_value_expr(right, false))
			{
				out->var = (Var *) left;
				out->val = right;
			}
			else if (lion_is_value_expr(left, false) && IsA(right, Var))
			{
				out->var = (Var *) right;
				out->val = left;
			}
			else
				return false;

			/*
			 * A literal NULL equals nothing.  A parameter that turns out to be
			 * NULL is the same answer, but only the executor can see it, so it
			 * selects no rows there instead.
			 */
			if (IsA(out->val, Const) && ((Const *) out->val)->constisnull)
				return false;

			out->opno = op->opno;
			out->cmptype = exprType(out->val);
			out->kind = LION_CLAUSE_EQ;
		}
		else if (out->strategy == LION_STRAT_NE)
		{
			/*
			 * `col <> c` (DESIGN.md §35): every row but those of c's entry
			 * and the NULL one, which it subtracts - a negated source, as
			 * `IS NOT NULL` is, so it is not taken where one is not (under an
			 * OR).  `<>` commutes like `=`, so either side may hold the
			 * column; a literal NULL makes it true of no row.
			 */
			Oid			neop = op->opno;

			if (!allow_negated)
				return false;
			if (IsA(left, Var) && lion_is_value_expr(right, false))
			{
				out->var = (Var *) left;
				out->val = right;
			}
			else if (lion_is_value_expr(left, false) && IsA(right, Var))
			{
				/*
				 * The column on the left, as a range's is: the operator is
				 * also what a row is tested with (lion_count_scan_filtered()),
				 * column first.
				 */
				neop = get_commutator(op->opno);
				if (!OidIsValid(neop) ||
					lion_op_roaring_strategy(neop, &opfamily,
											 &lefttype) != LION_STRAT_NE)
					return false;
				out->var = (Var *) right;
				out->val = left;
			}
			else
				return false;
			if (IsA(out->val, Const) && ((Const *) out->val)->constisnull)
				return false;

			out->opno = neop;
			out->cmptype = exprType(out->val);
			out->kind = LION_CLAUSE_NE;
		}
		else if (LION_STRAT_IS_RANGE(out->strategy))
		{
			Oid			rangeop = op->opno;

			/*
			 * A range comparison (DESIGN.md §28).  The column may be on either
			 * side: `5 < k` is `k > 5`, and the commutator is what the index's
			 * opfamily has to know - a family that has `<(int8, int4)` but not
			 * `>(int4, int8)` cannot answer it, and is not asked to.
			 */
			if (!allow_range)
				return false;
			if (IsA(left, Var) && lion_is_value_expr(right, false))
			{
				out->var = (Var *) left;
				out->val = right;
			}
			else if (IsA(right, Var) && lion_is_value_expr(left, false))
			{
				rangeop = get_commutator(op->opno);
				if (!OidIsValid(rangeop))
					return false;
				out->var = (Var *) right;
				out->val = left;
				out->strategy = lion_op_roaring_strategy(rangeop, &opfamily,
														 &lefttype);
			}
			else
				return false;
			if (!LION_STRAT_IS_RANGE(out->strategy) || !op_strict(rangeop))
				return false;

			/* A literal NULL bound compares with nothing, as for equality. */
			if (IsA(out->val, Const) && ((Const *) out->val)->constisnull)
				return false;

			out->opno = rangeop;
			out->cmptype = exprType(out->val);
			out->kind = LION_CLAUSE_RANGE;
		}
		else if (LION_STRAT_IS_MULTI_SEARCH(out->strategy))
		{
			/*
			 * A multi-key operator (DESIGN.md §17).  Unlike equality it does
			 * not commute - `'{a}' @> tags` is a containment the other way
			 * round, which is strategy 4 and not pushed down - so the column
			 * has to be the left operand.
			 */
			if (!IsA(left, Var))
				return false;
			out->var = (Var *) left;
			out->val = right;

			if (IsA(right, Const))
			{
				if (((Const *) out->val)->constisnull)
					return false;

				/*
				 * A prefix lexeme is expanded at run time into the index's
				 * lexemes that have it, and one too common to expand is
				 * answered from every row and a recheck (DESIGN.md §17,
				 * "Prefix lexemes"), which no leaf of an OR can have.
				 */
				if (!allow_recheck && out->strategy == LION_STRAT_MATCH &&
					lion_tsquery_has_prefix(((Const *) out->val)->constvalue))
					return false;

				/*
				 * A literal query's SHAPE is known now, so only an EXACT one
				 * is pushed down.  `tags @> '{}'`, a tsquery with
				 * NOT/phrase/prefix/weights and anything with a NULL element
				 * want rows rechecked in the heap, which is what the ordinary
				 * plan does anyway.
				 */
				if (!lion_multikey_query_is_exact(opfamily, lefttype,
												 out->strategy,
												 (Const *) out->val,
												 &out->extractquery))
				{
					/*
					 * One the sets can only bound - a phrase is its lexemes'
					 * AND, a weight the lexeme at any weight - is counted
					 * from that superset with every candidate rechecked in
					 * the heap, exactly as a value known only at run time is
					 * (below), where a recheck is allowed.  One no key
					 * narrows stays with the ordinary plan.
					 */
					if (!OidIsValid(out->extractquery) ||
						lion_multikey_query_mode(out->extractquery,
												 out->strategy,
												 (Const *) out->val,
												 true) != LION_QMODE_LOSSY)
						return false;

					/*
					 * Under an OR no recheck sees the leaf alone (below).
					 * There it has to be one an index that stores positions
					 * decides exactly (lion_posfilter.c, LION_KN_POSFILTER),
					 * and lion_match_index() takes only such an index.
					 */
					if (!allow_recheck)
					{
						if (!lion_op_posexact(op->opno, out->extractquery,
											  (Const *) out->val))
							return false;
						out->positions = true;
					}
				}
			}
			else
			{
				Node	   *est;

				/*
				 * A query the node only has at run time: a generic plan's
				 * `tags @> $1`, `tsv @@ to_tsquery(current_setting(...))`
				 * (DESIGN.md §17, "A query known only at run time").  Its
				 * shape decides whether the posting sets answer it exactly,
				 * and the shape is not known until the executor has the value
				 * - `$1 = '{}'` is every row, a phrase is its lexemes' rows
				 * and more - so the node cannot decline the ones they do not
				 * answer.  It answers them from a SUPERSET instead and
				 * rechecks every candidate in the heap
				 * (lion_extract_query_superset(), LionRowFilter), which is
				 * exact for every value the clause can take.  A value the
				 * executor evaluates once per scan qualifies, by the rule
				 * equality follows (lion_is_value_expr()), and so does the
				 * `ARRAY[$1, $2]` a generic plan keeps for an array written
				 * out of parameters.
				 */
				if (!allow_recheck || !lion_is_value_expr(right, true))
					return false;
				out->extractquery = get_opfamily_proc(opfamily, lefttype,
													  lefttype,
													  LION_EXTRACTQUERY_PROC);
				if (!OidIsValid(out->extractquery))
					return false;

				/*
				 * The cost model extracts the plan-time estimate of the value
				 * the way the executor will extract the value itself
				 * (lion_multikey_cost_mode()), so it is handed the clause
				 * over that estimate; a value without one - a Param - is
				 * priced as the expensive shape.  The executor evaluates the
				 * expression, never the estimate.
				 */
				est = estimate_expression_value(root, right);
				if (IsA(est, Const))
					out->costclause = (Node *)
						make_opclause(op->opno, op->opresulttype,
									  op->opretset,
									  (Expr *) linitial(op->args),
									  (Expr *) est,
									  op->opcollid, op->inputcollid);
			}

			out->opno = op->opno;
			out->cmptype = InvalidOid;	/* the query is not a key */
			out->kind = LION_CLAUSE_MULTI;
		}
		else
			return false;		/* strategy 4 (`<@`), or not ours at all */

		out->collation = op->inputcollid;
	}
	else if (IsA(clause, ScalarArrayOpExpr))
	{
		ScalarArrayOpExpr *saop = (ScalarArrayOpExpr *) clause;
		Node	   *left;
		Node	   *right;
		Oid			opfamily;
		Oid			lefttype;
		int			nelems;

		/* `= ALL (...)` is not a union of keys (DESIGN.md §15). */
		if (!saop->useOr)
			return false;
		if (list_length(saop->args) != 2)
			return false;
		if (!op_strict(saop->opno))
			return false;

		left = lion_strip((Node *) linitial(saop->args));
		right = lion_strip((Node *) lsecond(saop->args));
		if (left == NULL || right == NULL)
			return false;
		if (!IsA(left, Var) || !lion_is_value_expr(right, true))
			return false;
		out->var = (Var *) left;
		out->val = right;

		/*
		 * The length cap of DESIGN.md §15 applies to the lists whose length is
		 * known now: a literal array and the ARRAY[...] a generic plan keeps
		 * for `k IN ($1, $2)`.  A parameter that IS an array has no length
		 * until the executor has it, and by then there is no plan to decline
		 * in favour of, so it is answered whatever its length.
		 *
		 * Any other array expression - a stable function's, `k = ANY
		 * (string_to_array(current_setting(...), ','))` - is estimated as
		 * core's selectivity functions estimate it, and when that gives a
		 * literal the cap is applied to it and the cost model is handed the
		 * clause over it, so that it prices the list's real length rather
		 * than estimate_array_length()'s guess for an expression.  The
		 * executor still evaluates the expression itself (the estimate is
		 * the plan-time value, which a stable function need not keep).
		 */
		if (IsA(out->val, Const))
		{
			if (((Const *) out->val)->constisnull)
				return false;
			nelems = lion_array_const_nelems((Const *) out->val);
			if (nelems < 0 || nelems > LION_MAX_ARRAY_ELEMS)
				return false;
		}
		else if (IsA(out->val, ArrayExpr))
		{
			nelems = list_length(((ArrayExpr *) out->val)->elements);
			if (nelems > LION_MAX_ARRAY_ELEMS)
				return false;
		}
		else if (!IsA(out->val, Param))
		{
			Node	   *est = estimate_expression_value(root, out->val);

			if (IsA(est, Const) && !((Const *) est)->constisnull)
			{
				ScalarArrayOpExpr *costsaop;

				nelems = lion_array_const_nelems((Const *) est);
				if (nelems > LION_MAX_ARRAY_ELEMS)
					return false;

				costsaop = (ScalarArrayOpExpr *) copyObject(saop);
				costsaop->args = list_make2(linitial(costsaop->args), est);
				out->costclause = (Node *) costsaop;
			}
		}

		/*
		 * `col op ANY (array)` is a union of single-key lookups, so the
		 * operator has to be equality; `tags @> ANY (...)` would be a union of
		 * multi-key queries, which nothing here builds.
		 */
		if (lion_op_roaring_strategy(saop->opno, &opfamily,
									&lefttype) != LION_STRAT_EQUAL)
			return false;

		out->opno = saop->opno;
		out->cmptype = get_element_type(exprType(out->val));
		if (!OidIsValid(out->cmptype))
			return false;
		out->kind = LION_CLAUSE_ARRAY;
		out->collation = saop->inputcollid;
	}
	else if (IsA(clause, NullTest) ||
			 (IsA(clause, BooleanTest) &&
			  (((BooleanTest *) clause)->booltesttype == IS_UNKNOWN ||
			   ((BooleanTest *) clause)->booltesttype == IS_NOT_UNKNOWN)))
	{
		bool		isnull;
		Node	   *arg;

		/* `flag IS UNKNOWN` is `flag IS NULL`, and its negation likewise. */
		if (IsA(clause, NullTest))
		{
			NullTest   *nt = (NullTest *) clause;

			if (nt->argisrow)
				return false;
			isnull = (nt->nulltesttype == IS_NULL);
			arg = (Node *) nt->arg;
		}
		else
		{
			isnull = (((BooleanTest *) clause)->booltesttype == IS_UNKNOWN);
			arg = (Node *) ((BooleanTest *) clause)->arg;
		}
		if (!isnull && !allow_negated)
			return false;
		arg = lion_strip(arg);
		if (arg == NULL || !IsA(arg, Var))
			return false;
		out->var = (Var *) arg;

		out->kind = isnull ? LION_CLAUSE_NULL : LION_CLAUSE_NOTNULL;
		/* The executor needs no value; keep the lists in step. */
		out->val = (Node *) makeNullConst(out->var->vartype,
										  out->var->vartypmod,
										  out->var->varcollid);
	}
	else
		return false;

	if (out->var->varno != (int) rti || out->var->varattno <= 0 ||
		out->var->varlevelsup != 0)
		return false;

	return true;
}

/*
 * Append one analysed clause to the parallel lists the planner carries.  The
 * flattened clause array holds the leaves of an OR restriction alongside the
 * plain clauses (DESIGN.md §19), so everything that follows - matching an
 * index per relation, pricing the lookup, moving a Param into custom_exprs -
 * treats them alike; inor says which are which.  The clause the cost model
 * is given is the leaf's own costclause when the analysis made one (a bare
 * boolean column's `col = true`), the query's otherwise.
 */
void
lion_append_clause(const LionLeafInfo *leaf, Node *clause, bool inor,
				  List **whereattnos, List **clauseinfos, List **whereclauses,
				  List **whereconsts, List **wherekinds, List **whereopnos,
				  List **whereinor)
{
	LionClauseInfo *ci = (LionClauseInfo *) palloc0(sizeof(LionClauseInfo));

	ci->attno = leaf->var->varattno;
	ci->kind = leaf->kind;
	ci->opno = leaf->opno;
	ci->cmptype = leaf->cmptype;
	ci->strategy = leaf->strategy;
	ci->extractquery = leaf->extractquery;
	ci->collation = leaf->collation;
	ci->inor = inor;
	ci->positions = leaf->positions;
	ci->rinfono = -1;			/* the caller's to say */

	*whereattnos = lappend_int(*whereattnos, (int) leaf->var->varattno);
	*clauseinfos = lappend(*clauseinfos, ci);
	*whereclauses = lappend(*whereclauses,
							leaf->costclause != NULL ? leaf->costclause : clause);
	*whereconsts = lappend(*whereconsts, leaf->val);
	*wherekinds = lappend_int(*wherekinds, leaf->kind);
	*whereopnos = lappend_oid(*whereopnos, leaf->opno);
	*whereinor = lappend_int(*whereinor, inor ? 1 : 0);
}

/*
 * Which clauses are leaves of an OR restriction (DESIGN.md §19)?  Decoded
 * from LION_PRIV_ORS, whose lists name a contiguous run of clauses each.
 */
bool *
lion_or_leaf_map(List *ors, int nclause)
{
	bool	   *map = (bool *) palloc0(sizeof(bool) * Max(nclause, 1));
	ListCell   *lc;

	foreach(lc, ors)
	{
		List	   *one = (List *) lfirst(lc);
		int			first = linitial_int(one);
		int			narms = lsecond_int(one);
		int			nleaves = 0;
		int			i;

		for (i = 0; i < narms; i++)
			nleaves += list_nth_int(one, 2 + i);
		for (i = first; i < first + nleaves && i < nclause; i++)
			map[i] = true;
	}

	return map;
}

/*
 * The same map, but naming WHICH OR restriction each clause is a leaf of (-1
 * for a clause that is not one).  The cost model needs the identity and not
 * just the fact: a union is ONE source of the AND (DESIGN.md §19), and which
 * source a clause belongs to is what decides whether it is walked or sought
 * (DESIGN.md §22).
 */
int *
lion_or_group_map(List *ors, int nclause)
{
	int		   *map = (int *) palloc(sizeof(int) * Max(nclause, 1));
	int			group = 0;
	int			i;
	ListCell   *lc;

	for (i = 0; i < Max(nclause, 1); i++)
		map[i] = -1;

	foreach(lc, ors)
	{
		List	   *one = (List *) lfirst(lc);
		int			first = linitial_int(one);
		int			narms = lsecond_int(one);
		int			nleaves = 0;

		for (i = 0; i < narms; i++)
			nleaves += list_nth_int(one, 2 + i);
		for (i = first; i < first + nleaves && i < nclause; i++)
			map[i] = group;
		group++;
	}

	return map;
}

/*
 * The ARM each clause is a leaf of, numbered across every OR restriction (-1
 * for a clause that is not an OR's leaf): the bounds of one range in one arm
 * are one source (DESIGN.md §32), and the same column's bounds in two arms
 * are two.
 */
static int *
lion_or_arm_map(List *ors, int nclause)
{
	int		   *map = (int *) palloc(sizeof(int) * Max(nclause, 1));
	int			arm = 0;
	int			i;
	ListCell   *lc;

	for (i = 0; i < Max(nclause, 1); i++)
		map[i] = -1;

	foreach(lc, ors)
	{
		List	   *one = (List *) lfirst(lc);
		int			leaf = linitial_int(one);
		int			narms = lsecond_int(one);
		int			a;

		for (a = 0; a < narms; a++, arm++)
		{
			int			len = list_nth_int(one, 2 + a);

			for (i = 0; i < len; i++, leaf++)
				if (leaf < nclause)
					map[leaf] = arm;
		}
	}

	return map;
}

/*
 * For each clause, the clause that stands for its range when it is a bound of
 * a range taken as a source (DESIGN.md §32): the first RANGESRC clause on the
 * same key column of the same index in the same conjunction - the top level,
 * or one OR arm.  -1 for every other clause.  The executor groups them the
 * same way (lion_begin_custom_scan(), lion_locate_or()).
 */
int *
lion_rangesrc_leaders(List *whereidx, List *wherecol, List *wherekinds,
					  List *ors, int nclause)
{
	int		   *lead = (int *) palloc(sizeof(int) * Max(nclause, 1));
	int		   *armof = lion_or_arm_map(ors, nclause);
	int			i;
	int			j;

	for (i = 0; i < nclause; i++)
	{
		lead[i] = -1;
		if (list_nth_int(wherekinds, i) != LION_CLAUSE_RANGESRC)
			continue;
		for (j = 0; j <= i; j++)
		{
			if (list_nth_int(wherekinds, j) == LION_CLAUSE_RANGESRC &&
				armof[j] == armof[i] &&
				list_nth(whereidx, j) == list_nth(whereidx, i) &&
				list_nth_int(wherecol, j) == list_nth_int(wherecol, i))
			{
				lead[i] = j;
				break;
			}
		}
	}
	pfree(armof);
	return lead;
}

static bool
lion_contains_param_walker(Node *node, void *context)
{
	if (node == NULL)
		return false;
	if (IsA(node, Param))
		return true;
	return expression_tree_walker(node, lion_contains_param_walker, context);
}

/* Does expr hold a Param of any kind? */
bool
lion_contains_param(Node *expr)
{
	return lion_contains_param_walker(expr, NULL);
}

/*
 * Is agg an aggregate the entries of one lion column answer, each entry's
 * key weighted by its rows (DESIGN.md §37) - and which?  sum or avg of an
 * int2, int4 or int8 argument, or an aggregate with a sort operator (min,
 * max, bool_and, bool_or: planagg.c's first value in that order), of an
 * argument that is an expression of one column of relation rti alone:
 * immutable, and with no parameter, subquery, aggregate, window or set-
 * returning function.  No DISTINCT, ORDER BY or FILTER.  *attno is the
 * column and *argwidth the byte width of a sum's or an average's argument.
 * Whether the column has an index whose keys are its values is the caller's
 * to ask.
 */
int
lion_wagg_classify(Aggref *agg, Index rti, AttrNumber *attno, int *argwidth)
{
	Node	   *arg;
	List	   *vars;
	ListCell   *lc;
	int			kind = LION_WAGG_NONE;
	int			width = 0;

	if (agg->aggdistinct != NIL || agg->aggorder != NIL ||
		agg->aggfilter != NULL || agg->agglevelsup != 0 || agg->aggstar ||
		agg->aggkind != AGGKIND_NORMAL || agg->aggsplit != AGGSPLIT_SIMPLE ||
		list_length(agg->args) != 1)
		return LION_WAGG_NONE;
	arg = (Node *) ((TargetEntry *) linitial(agg->args))->expr;

	switch (agg->aggfnoid)
	{
#ifdef HAVE_INT128
		case F_SUM_INT2:
			kind = LION_WAGG_SUM;
			width = 2;
			break;
		case F_SUM_INT4:
			kind = LION_WAGG_SUM;
			width = 4;
			break;
		case F_SUM_INT8:
			kind = LION_WAGG_SUM8;
			width = 8;
			break;
		case F_AVG_INT2:
			kind = LION_WAGG_AVG;
			width = 2;
			break;
		case F_AVG_INT4:
			kind = LION_WAGG_AVG;
			width = 4;
			break;
		case F_AVG_INT8:
			kind = LION_WAGG_AVG8;
			width = 8;
			break;
#endif
		default:
			{
				HeapTuple	tup;

				tup = SearchSysCache1(AGGFNOID,
									  ObjectIdGetDatum(agg->aggfnoid));
				if (!HeapTupleIsValid(tup))
					return LION_WAGG_NONE;
				if (OidIsValid(((Form_pg_aggregate) GETSTRUCT(tup))->aggsortop) &&
					agg->aggtype == exprType(arg))
					kind = LION_WAGG_EXTREME;
				ReleaseSysCache(tup);
			}
			break;
	}
	if (kind == LION_WAGG_NONE)
		return LION_WAGG_NONE;

	if (contain_mutable_functions(arg) || contain_agg_clause(arg) ||
		contain_window_function(arg) || expression_returns_set(arg) ||
		contain_subplans(arg) || lion_contains_param(arg))
		return LION_WAGG_NONE;
	vars = pull_var_clause(arg, PVC_RECURSE_PLACEHOLDERS);
	*attno = 0;
	foreach(lc, vars)
	{
		Var		   *v = (Var *) lfirst(lc);

		if (!IsA(v, Var) || v->varno != (int) rti || v->varattno <= 0 ||
			v->varlevelsup != 0 || (*attno != 0 && v->varattno != *attno))
			return LION_WAGG_NONE;
		*attno = v->varattno;
	}
	if (*attno == 0)
		return LION_WAGG_NONE;
	if (argwidth != NULL)
		*argwidth = width;
	return kind;
}
