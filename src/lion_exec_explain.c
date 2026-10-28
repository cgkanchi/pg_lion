/*-------------------------------------------------------------------------
 *
 * lion_exec_explain.c
 *		EXPLAIN output of the LionCount scan.
 *
 * Part of the LionCount custom scan: lion_customscan.h describes the node
 * and declares what its files share.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "lion_customscan.h"

/*
 * A column of the plan by name, or an expression column (DESIGN.md §41) as
 * its expression.
 */
static const char *
lion_explain_attname(LionCountScanState *st, AttrNumber attno)
{
	if (LION_ATTNO_IS_VCOL(attno) &&
		LION_VCOL_INDEX(attno) < list_length(st->plan.vcols))
		return lion_vcol_name(st->plan.heapoid,
							  list_nth(st->plan.vcols, LION_VCOL_INDEX(attno)));
	return get_attname(st->plan.heapoid, attno, false);
}

/*
 * Whether the array is empty (DESIGN.md §17, "Non-empty arrays"): the rows
 * of the reserved entries mask names, or with notreserved every row outside
 * them, as the five classes of row they are - NULL, empty, an empty first
 * dimension, an empty inner one, an element - in one spelling for each set
 * of classes, whichever the query had.  Those are the sets one clause can
 * select (lion_nonempty_leaf()), its column's own null tests apart; any
 * other is printed as the OR of its classes' all the same.
 */
#define LION_EX_NULL	0x01
#define LION_EX_EMPTY	0x02
#define LION_EX_DIM		0x04
#define LION_EX_INNER	0x08
#define LION_EX_SOME	0x10

static void
lion_explain_reserved(StringInfo buf, const char *attname, int32 mask,
					  bool notreserved)
{
	static const struct
	{
		int			classes;
		const char *fmt;
	}			spellings[] = {
		{LION_EX_EMPTY, "%s = '{}'"},
		{LION_EX_DIM | LION_EX_INNER | LION_EX_SOME, "%s <> '{}'"},
		{LION_EX_NULL | LION_EX_DIM | LION_EX_INNER | LION_EX_SOME,
		"%s IS DISTINCT FROM '{}'"},
		{LION_EX_EMPTY | LION_EX_DIM | LION_EX_INNER, "cardinality(%s) = 0"},
		{LION_EX_NULL | LION_EX_EMPTY | LION_EX_DIM | LION_EX_INNER,
		"coalesce(cardinality(%s), 0) = 0"},
		{LION_EX_SOME, "cardinality(%s) > 0"},
		{LION_EX_NULL | LION_EX_SOME, "cardinality(%s) IS DISTINCT FROM 0"},
		{LION_EX_NULL | LION_EX_EMPTY, "array_length(%s, 1) IS NULL"},
		{LION_EX_DIM, "array_length(%s, 1) = 0"},
		{LION_EX_INNER | LION_EX_SOME, "array_length(%s, 1) > 0"},
		{LION_EX_NULL | LION_EX_EMPTY | LION_EX_DIM,
		"coalesce(array_length(%s, 1), 0) = 0"},
		{LION_EX_NULL | LION_EX_EMPTY | LION_EX_INNER | LION_EX_SOME,
		"array_length(%s, 1) IS DISTINCT FROM 0"},
	};
	int			classes = 0;
	int			i;
	bool		first = true;

	if ((mask & LION_ENTRY_NULLKEY) != 0)
		classes |= LION_EX_NULL;
	if ((mask & LION_ENTRY_EMPTYKEY) != 0)
		classes |= LION_EX_EMPTY;
	if ((mask & LION_ENTRY_EMPTYDIM) != 0)
		classes |= LION_EX_DIM;
	if ((mask & LION_ENTRY_EMPTYINNER) != 0)
		classes |= LION_EX_INNER;
	if (notreserved)
		classes = (LION_EX_NULL | LION_EX_EMPTY | LION_EX_DIM | LION_EX_INNER |
				   LION_EX_SOME) & ~classes;

	for (i = 0; i < (int) lengthof(spellings); i++)
	{
		if (spellings[i].classes == classes)
		{
			appendStringInfo(buf, spellings[i].fmt, attname);
			return;
		}
	}

	appendStringInfoChar(buf, '(');
	for (i = 0; i < 5; i++)
	{
		int			cls = 1 << i;

		if ((classes & cls) == 0)
			continue;
		if (!first)
			appendStringInfoString(buf, " OR ");
		first = false;
		switch (cls)
		{
			case LION_EX_NULL:
				appendStringInfo(buf, "%s IS NULL", attname);
				break;
			case LION_EX_EMPTY:
				appendStringInfo(buf, "%s = '{}'", attname);
				break;
			case LION_EX_DIM:
				appendStringInfo(buf, "array_length(%s, 1) = 0", attname);
				break;
			case LION_EX_INNER:
				appendStringInfo(buf, "(array_length(%s, 1) > 0 AND cardinality(%s) = 0)",
								 attname, attname);
				break;
			case LION_EX_SOME:
				appendStringInfo(buf, "cardinality(%s) > 0", attname);
				break;
		}
	}
	appendStringInfoChar(buf, ')');
}

/*
 * "col = 3", "col = ANY ('{1,2,3}'::integer[])", "col IS NULL",
 * "col IS NOT NULL", "tags @> '{a,b}'::text[]", "col >= 10" (a range,
 * DESIGN.md §28, with the column on the left whichever side the query had it
 * on) - each with the clause's OWN operator and its value as core's EXPLAIN
 * would print it in a qual.
 *
 * Both used to be approximated, and the approximation hid a wrong answer (the
 * 2026-09-25 review): every equality was printed with `=`, so a clause on a
 * case-insensitive opclass's `===` read exactly like the `=` beside it that
 * the planner had dropped as its duplicate, and a literal was printed through
 * its type's output function alone, so `v = 'A'` came out as `(v = A)` and a
 * list as `ANY ({90,5,50,1})`.  Now the operator is named - `===` prints as
 * `===`, as the multi-key and range clauses always did - and the value is
 * deparsed like any other expression the plan carries: a literal with its
 * quotes and its type, and a parameter as `$1`, the same text core gives a
 * qual on one (DESIGN.md §10).
 */
static void
lion_explain_clause(LionCountScanState *st, LionClauseState *cl, List *ancestors,
				   ExplainState *es, StringInfo buf)
{
	const char *attname = lion_explain_attname(st, cl->attno);

	switch (cl->kind)
	{
		case LION_CLAUSE_NULL:
			appendStringInfo(buf, "%s IS NULL", attname);
			break;
		case LION_CLAUSE_NOTNULL:
			appendStringInfo(buf, "%s IS NOT NULL", attname);
			break;
		case LION_CLAUSE_RESERVED:
		case LION_CLAUSE_NOTRESERVED:
			{
				int32		mask;

				if (cl->con == NULL)
					elog(ERROR, "LionCount: the reserved entries of a clause are no constant");
				mask = DatumGetInt32(cl->con->constvalue);
				if (!LION_RESERVED_MASK_OK(mask))
					elog(ERROR, "LionCount: reserved entries 0x%x of a clause",
						 (unsigned int) mask);
				lion_explain_reserved(buf, attname, mask,
									  cl->kind == LION_CLAUSE_NOTRESERVED);
				break;
			}
		default:
			{
				List	   *context;
				char	   *opname = get_opname(cl->opno);
				char	   *val;
				int			filt = lion_join_filter_of(st,
													   (int) (cl - st->clause));

				if (opname == NULL)
					elog(ERROR, "LionCount: cache lookup failed for operator %u",
						 cl->opno);

				context = set_deparse_context_plan(es->deparse_cxt,
												   st->css.ss.ps.plan,
												   ancestors);

				/*
				 * The FK-side join's key is a column of the OTHER table
				 * (DESIGN.md §27), so it is printed qualified: `fk = d.pk`,
				 * as core prints a join clause.  So is the key of a dimension
				 * folded into a fact filter ("A second dimension that only
				 * filters"): the column of its child the keys are read from,
				 * `fk = d2.pk` - that clause - as that child prints it.
				 */
				if (filt >= 0)
				{
					Plan	   *fplan = st->join->filterps[filt]->plan;
					TargetEntry *tle = (TargetEntry *)
						list_nth(fplan->targetlist,
								 st->join->filtercol[filt] - 1);

					List	   *fancestors = lcons(st->css.ss.ps.plan,
													   list_copy(ancestors));

					context = set_deparse_context_plan(es->deparse_cxt, fplan,
													   fancestors);
					val = deparse_expression((Node *) tle->expr, context, true,
											 false);
					appendStringInfo(buf, "%s %s %s", attname, opname, val);
					pfree(opname);
					pfree(val);
					break;
				}
				val = deparse_expression((Node *) cl->valexpr, context,
										 st->join != NULL &&
										 cl == &st->clause[st->join->clause],
										 false);
				if (cl->kind == LION_CLAUSE_ARRAY)
					appendStringInfo(buf, "%s %s ANY (%s)", attname, opname,
									 val);
				else if (cl->kind == LION_CLAUSE_DISTINCT)
					appendStringInfo(buf, "%s IS DISTINCT FROM %s", attname,
									 val);
				else if (cl->kind == LION_CLAUSE_NOTMULTI)
					appendStringInfo(buf, "NOT (%s %s %s)", attname, opname,
									 val);
				else
					appendStringInfo(buf, "%s %s %s", attname, opname, val);
				pfree(opname);
				pfree(val);
				break;
			}
	}
}

/*
 * The KEY COLUMN an index answers one clause with, as ".col" (DESIGN.md §24).
 *
 * A SINGLE-column index prints nothing at all, so every plan the regression
 * suite had before multicolumn indexes existed is unchanged; a multicolumn one
 * has to say which of its columns it is being read for, because two clauses of
 * one query may now name the same index.
 *
 * The index is opened here rather than read from the executor state: EXPLAIN
 * without ANALYZE never opens anything (EXEC_FLAG_EXPLAIN_ONLY), and it is the
 * one case where the name is wanted and the relation is not in hand.  The heap
 * attribute number is the one the plan carries; a partitioned scan prints no
 * index name at all, so it never gets here with a parent's numbering.
 *
 * The name is palloc'd in the caller's context, EXPLAIN's, like the
 * get_rel_name() it is printed beside; it was a static buffer, which two
 * calls in one argument list would have overwritten, the first printing
 * the second's column.
 */
static const char *
lion_explain_col_kind(Oid idxoid, AttrNumber heapattno, int kind, List *vcols)
{
	Relation	idx;
	AttrNumber	col;
	char	   *name;

	if (!OidIsValid(idxoid) || heapattno <= 0)
		return "";

	idx = index_open(idxoid, AccessShareLock);
	if (IndexRelationGetNumberOfKeyAttributes(idx) <= 1)
	{
		index_close(idx, AccessShareLock);
		return "";
	}

	col = lion_index_col_for_kind(idx, heapattno, kind, vcols);
	name = psprintf(".%s",
					NameStr(TupleDescAttr(RelationGetDescr(idx), col - 1)->attname));
	index_close(idx, AccessShareLock);

	return name;
}

/* ... for a column whose entries are read as values: a scalar one */
const char *
lion_explain_col(Oid idxoid, AttrNumber heapattno, bool multikey, List *vcols)
{
	return lion_explain_col_kind(idxoid, heapattno,
								 multikey ? LION_CLAUSE_MULTI : LION_CLAUSE_EQ,
								 vcols);
}

/*
 * ... for one clause, whose kind says which kind of column answers it
 * (lion_index_col_for_kind()).
 */
static const char *
lion_explain_clause_col(LionCountScanState *st, const LionClauseState *cl)
{
	return lion_explain_col_kind(cl->idxoid, cl->attno, cl->kind,
								 st->plan.vcols);
}

/* One arm of an OR, `len` leaves from leaf `at`: `((b = 2) AND (c = 3))`. */
static void
lion_explain_arm(LionCountScanState *st, LionOrState *o, int at, int len,
				 List *ancestors, ExplainState *es, StringInfo buf)
{
	int			j;

	if (len > 1)
		appendStringInfoChar(buf, '(');
	for (j = 0; j < len; j++)
	{
		if (j > 0)
			appendStringInfoString(buf, " AND ");
		appendStringInfoChar(buf, '(');
		lion_explain_clause(st, &st->clause[o->first + at + j], ancestors,
						   es, buf);
		appendStringInfoChar(buf, ')');
	}
	if (len > 1)
		appendStringInfoChar(buf, ')');
}

/* An OR restriction (DESIGN.md §19): `((a = 1) OR ((b = 2) AND (c = 3)))`. */
static void
lion_explain_or(LionCountScanState *st, LionOrState *o, List *ancestors,
				ExplainState *es, StringInfo buf)
{
	int			at = 0;
	int			arm;

	appendStringInfoChar(buf, '(');
	for (arm = 0; arm < o->narms; arm++)
	{
		if (arm > 0)
			appendStringInfoString(buf, " OR ");
		lion_explain_arm(st, o, at, o->armlen[arm], ancestors, es, buf);
		at += o->armlen[arm];
	}
	appendStringInfoChar(buf, ')');
}

/*
 * Does partition p leave out all of OR `o`'s leaves from leaf `at` on, `len`
 * of them - all of the OR, or all of one arm of it?
 */
static bool
lion_explain_left_out(LionCountScanState *st, LionOrState *o, int p, int at,
					  int len)
{
	int			j;

	for (j = at; j < at + len; j++)
	{
		if (OidIsValid(st->part[p].clauseidxoid[o->first + j]))
			return false;
	}
	return true;
}

/*
 * What partition p makes of leaf `leaf` of OR `o`, which is in the arm of
 * `len` leaves from `at` (DESIGN.md §16): 'o' the OR is left out whole, 'r'
 * the arm is refuted, 'i' the leaf alone is implied, or '-' it is counted.
 */
static char
lion_explain_or_leaf(LionCountScanState *st, LionOrState *o, int p, int at,
					 int len, int leaf)
{
	if (lion_explain_left_out(st, o, p, 0, o->nleaves))
		return 'o';
	if (lion_explain_left_out(st, o, p, at, len))
		return 'r';
	if (!OidIsValid(st->part[p].clauseidxoid[o->first + leaf]))
		return 'i';
	return '-';
}

/*
 * How many of the partitions leave clause i out, because their bounds imply
 * it (DESIGN.md §16, "Clauses the partition bounds imply"): the ones whose
 * index for it is InvalidOid.
 */
static int
lion_explain_dropped(LionCountScanState *st, int i)
{
	int			n = 0;
	int			p;

	for (p = 0; p < st->npart; p++)
	{
		if (!OidIsValid(st->part[p].clauseidxoid[i]))
			n++;
	}
	return n;
}

/* ... and whether that is every partition, so that no index answers it */
static bool
lion_explain_dropped_all(LionCountScanState *st, int i)
{
	return st->npart > 0 && lion_explain_dropped(st, i) == st->npart;
}

/* How many partitions make `what` of leaf `leaf` of OR `o`, in its arm. */
static int
lion_explain_or_count(LionCountScanState *st, LionOrState *o, int at, int len,
					  int leaf, char what)
{
	int			n = 0;
	int			p;

	for (p = 0; p < st->npart; p++)
	{
		if (lion_explain_or_leaf(st, o, p, at, len, leaf) == what)
			n++;
	}
	return n;
}

/* " in n of m partitions" after an entry, where not all of them are */
static void
lion_explain_part_count(LionCountScanState *st, int n, StringInfo buf)
{
	if (n < st->npart)
		appendStringInfo(buf, " in %d of %d partitions", n, st->npart);
}

/*
 * The entries of "Implied by Partition Bounds" for clause i - or for the OR
 * `o` it is the first leaf of - with how many partitions leave each out when
 * not all of them: the clause; the OR, where a partition leaves it out whole;
 * and each leaf of an OR that a partition leaves out of an arm it keeps.
 */
static void
lion_explain_implied(LionCountScanState *st, int i, LionOrState *o,
					 List *ancestors, ExplainState *es, StringInfo buf)
{
	int			n;
	int			at = 0;
	int			arm;
	int			j;

	if (o == NULL)
	{
		n = lion_explain_dropped(st, i);
		if (n == 0)
			return;
		if (buf->len > 0)
			appendStringInfoString(buf, ", ");
		appendStringInfoChar(buf, '(');
		lion_explain_clause(st, &st->clause[i], ancestors, es, buf);
		appendStringInfoChar(buf, ')');
		lion_explain_part_count(st, n, buf);
		return;
	}

	n = lion_explain_or_count(st, o, 0, o->nleaves, 0, 'o');
	if (n > 0)
	{
		if (buf->len > 0)
			appendStringInfoString(buf, ", ");
		lion_explain_or(st, o, ancestors, es, buf);
		lion_explain_part_count(st, n, buf);
	}
	for (arm = 0; arm < o->narms; arm++)
	{
		for (j = at; j < at + o->armlen[arm]; j++)
		{
			n = lion_explain_or_count(st, o, at, o->armlen[arm], j, 'i');
			if (n == 0)
				continue;
			if (buf->len > 0)
				appendStringInfoString(buf, ", ");
			appendStringInfoChar(buf, '(');
			lion_explain_clause(st, &st->clause[o->first + j], ancestors, es,
							   buf);
			appendStringInfoChar(buf, ')');
			lion_explain_part_count(st, n, buf);
		}
		at += o->armlen[arm];
	}
}

/*
 * The entries of "Refuted by Partition Bounds" for OR `o`: each arm some
 * partition's bounds refute, which that partition leaves out of the union.
 */
static void
lion_explain_refuted(LionCountScanState *st, LionOrState *o, List *ancestors,
					 ExplainState *es, StringInfo buf)
{
	int			at = 0;
	int			arm;

	for (arm = 0; arm < o->narms; arm++)
	{
		int			n = lion_explain_or_count(st, o, at, o->armlen[arm], at,
											  'r');

		if (n > 0)
		{
			if (buf->len > 0)
				appendStringInfoString(buf, ", ");
			lion_explain_arm(st, o, at, o->armlen[arm], ancestors, es, buf);
			lion_explain_part_count(st, n, buf);
		}
		at += o->armlen[arm];
	}
}

/*
 * The "Partitions" line of a partitioned scan (DESIGN.md §16): its
 * partitions, by name.
 */
static void
lion_explain_partitions(LionCountScanState *st, ExplainState *es)
{
	StringInfoData buf;
	int			i;

	initStringInfo(&buf);
	for (i = 0; i < st->npart; i++)
	{
		if (i > 0)
			appendStringInfoString(&buf, ", ");
		appendStringInfoString(&buf, get_rel_name(st->part[i].heapoid));
	}
	ExplainPropertyText("Partitions", buf.data, es);
	pfree(buf.data);
}

/*
 * The partial indexes the count reads (DESIGN.md §45): a partitioned scan's,
 * whose "Lion Indexes" name no index, on a line of their own; and for any
 * scan the WHERE clauses their predicates imply, which the plan leaves out,
 * as core leaves them out of a partial index scan.
 */
static void
lion_explain_partial(LionCountScanState *st, ExplainState *es)
{
	StringInfoData buf;
	ListCell   *lc;

	if (st->plan.partialidx != NIL && st->npart > 0)
	{
		initStringInfo(&buf);
		foreach(lc, st->plan.partialidx)
		{
			char	   *name = get_rel_name(lfirst_oid(lc));

			if (buf.len > 0)
				appendStringInfoString(&buf, ", ");
			appendStringInfoString(&buf, name != NULL ? name : "?");
		}
		ExplainPropertyText("Partial Indexes", buf.data, es);
		pfree(buf.data);
	}
	if (st->plan.predimplied != NIL)
	{
		initStringInfo(&buf);
		foreach(lc, st->plan.predimplied)
		{
			if (buf.len > 0)
				appendStringInfoString(&buf, ", ");
			appendStringInfoString(&buf, strVal(lfirst(lc)));
		}
		ExplainPropertyText("Implied by Index Predicate", buf.data, es);
		pfree(buf.data);
	}
}

/*
 * The entry of "Lion Indexes" for the index whose entries drive the count
 * (LION_FLAG_GROUPIDX): the index, what is walked of it - a range's bounds,
 * the column or all of its keys - and the inner index of a pair walked, if
 * any.
 */
static void
lion_explain_group_index(LionCountScanState *st, List *ancestors,
						 ExplainState *es, StringInfo buf)
{
	int			i;

	if (st->npart == 0)
		appendStringInfo(buf, "%s%s ", get_rel_name(st->plan.groupidxoid),
						 lion_explain_col(st->plan.groupidxoid,
										  st->driveattno, false, st->plan.vcols));
	if (lion_plan_flag(st, LION_FLAG_RANGE))
	{
		bool		firstrange = true;

		/* The walk's bounds (DESIGN.md §28): `(k >= 10 AND k < 20)`. */
		appendStringInfoChar(buf, '(');
		for (i = 0; i < st->nclause; i++)
		{
			if (st->clause[i].kind != LION_CLAUSE_RANGE)
				continue;
			if (!firstrange)
				appendStringInfoString(buf, " AND ");
			lion_explain_clause(st, &st->clause[i], ancestors, es, buf);
			firstrange = false;
		}
		appendStringInfoChar(buf, ')');
	}
	else if (st->plan.groupattno != 0 || st->plan.distattno != 0)
		appendStringInfo(buf, "(%s)",
						 lion_explain_attname(st, st->driveattno));
	else
	{
		bool		keys = false;

		/*
		 * Every entry but the NULL one when the column's own `IS NOT NULL`
		 * drives (DESIGN.md §14), every entry - every row - when nothing
		 * names the column (§35).
		 */
		for (i = 0; i < st->nclause && !keys; i++)
			keys = (st->clause[i].kind == LION_CLAUSE_NOTNULL &&
					!st->inor[i] && st->clause[i].attno == st->driveattno &&
					st->plan.clause[i].filter == 0);
		appendStringInfoString(buf, keys ? "(all keys)" : "(all rows)");
	}

	/*
	 * The inner index of a two-column GROUP BY (DESIGN.md §20), or of the
	 * (g, k) pairs of a count(DISTINCT k) per group (§26).
	 */
	if (st->innerattno != 0)
	{
		appendStringInfoString(buf, ", ");
		if (st->npart == 0)
			appendStringInfo(buf, "%s%s ", get_rel_name(st->plan.groupidxoid2),
							 lion_explain_col(st->plan.groupidxoid2,
											  st->innerattno, false, st->plan.vcols));
		appendStringInfo(buf, "(%s)",
						 lion_explain_attname(st, st->innerattno));
	}

	/*
	 * The other columns of the decoded walk (DESIGN.md §34), each read from
	 * its own index; the scan is never partitioned.
	 */
	if (st->decode != NULL)
	{
		for (i = 1; i < st->decode->ncol; i++)
			appendStringInfo(buf, ", %s%s (%s)",
							 get_rel_name(st->decode->idxoid[i]),
							 lion_explain_col(st->decode->idxoid[i],
											  st->decode->attno[i], false, st->plan.vcols),
							 lion_explain_attname(st, st->decode->attno[i]));
	}
}

/*
 * A range taken as a source (DESIGN.md §32): its index and all of its
 * bounds, `ix (k >= 10 AND k < 20)`.  Nothing, when every partition's bounds
 * imply every one of them.
 */
static void
lion_explain_range_item(LionCountScanState *st, LionSourceItem *it,
						List *ancestors, ExplainState *es, StringInfo buf)
{
	LionClauseState *first = &st->clause[it->clauseno];
	bool		firstbound = true;
	int			wfirst;
	int			wn;
	int			j;

	/* its own filter's bounds, or the WHERE's (DESIGN.md §10) */
	lion_clause_window(st, it->clauseno, &wfirst, &wn);
	for (j = wfirst; j < wfirst + wn; j++)
	{
		if (st->clause[j].kind == LION_CLAUSE_RANGESRC &&
			!st->inor[j] && st->clause[j].attno == first->attno &&
			!lion_explain_dropped_all(st, j))
			break;
	}
	if (j == wfirst + wn)
		return;

	if (buf->len > 0)
		appendStringInfoString(buf, ", ");
	if (st->npart == 0)
		appendStringInfo(buf, "%s%s ", get_rel_name(first->idxoid),
						 lion_explain_clause_col(st, first));
	appendStringInfoChar(buf, '(');
	for (j = wfirst; j < wfirst + wn; j++)
	{
		if (st->clause[j].kind != LION_CLAUSE_RANGESRC || st->inor[j] ||
			st->clause[j].attno != first->attno ||
			lion_explain_dropped_all(st, j))
			continue;
		if (!firstbound)
			appendStringInfoString(buf, " AND ");
		lion_explain_clause(st, &st->clause[j], ancestors, es, buf);
		firstbound = false;
	}
	appendStringInfoChar(buf, ')');
}

/*
 * The entry of "Lion Indexes" for one of the plan's items: its index - or an
 * OR's indexes - where the scan is not partitioned, and the clause, range or
 * OR it answers.  Nothing, when every partition leaves it out.
 */
static void
lion_explain_item(LionCountScanState *st, LionSourceItem *it,
				  List *ancestors, ExplainState *es, StringInfo buf)
{
	int			orno = it->orno;

	if (orno < 0 && it->rangesrc)
	{
		lion_explain_range_item(st, it, ancestors, es, buf);
		return;
	}

	if (orno < 0 ? lion_explain_dropped_all(st, it->clauseno) :
		(st->npart > 0 &&
		 lion_explain_or_count(st, &st->ors[orno], 0,
							   st->ors[orno].nleaves, 0, 'o') == st->npart))
		return;
	if (buf->len > 0)
		appendStringInfoString(buf, ", ");
	if (orno < 0)
	{
		if (st->npart == 0)
		{
			LionClauseState *cl = &st->clause[it->clauseno];

			appendStringInfo(buf, "%s%s ", get_rel_name(cl->idxoid),
							 lion_explain_clause_col(st, cl));
		}
		appendStringInfoChar(buf, '(');
		lion_explain_clause(st, &st->clause[it->clauseno],
						   ancestors, es, buf);
		appendStringInfoChar(buf, ')');
	}
	else
	{
		/*
		 * An OR is one source over several indexes (DESIGN.md §19), so it
		 * names all of them and then prints the boolean expression:
		 * `ix_a, ix_b ((a = 1) OR (b = 2))`.
		 */
		LionOrState *o = &st->ors[orno];
		int			leaf;

		if (st->npart == 0)
		{
			for (leaf = 0; leaf < o->nleaves; leaf++)
			{
				LionClauseState *cl = &st->clause[o->first + leaf];

				appendStringInfo(buf, "%s%s%s",
								 leaf > 0 ? ", " : "",
								 get_rel_name(cl->idxoid),
								 lion_explain_clause_col(st, cl));
			}
			appendStringInfoChar(buf, ' ');
		}
		lion_explain_or(st, o, ancestors, es, buf);
	}
}

/*
 * The plan's WHERE items: its items, or beside filtered counts (DESIGN.md
 * §10) run 0's, whichever run the node stands at.
 */
static LionSourceItem *
lion_explain_where(LionCountScanState *st)
{
	return (st->nrun > 1) ? st->runitem[0] : st->planitem;
}

static int
lion_explain_nwhere(LionCountScanState *st)
{
	return (st->nrun > 1) ? st->nrunitem[0] : st->nplanitem;
}

/*
 * The "Lion Indexes" line: the index whose entries drive the count, the
 * FK-side join's key and the plan's items, each with what it answers; and a
 * line of its own for each filtered count's.
 */
static void lion_explain_slice_ranges(LionCountScanState *st,
									  List *ancestors, ExplainState *es);

static void
lion_explain_indexes(LionCountScanState *st, List *ancestors,
					 ExplainState *es)
{
	StringInfoData buf;
	int			i;
	int			r;

	initStringInfo(&buf);

	if (lion_plan_flag(st, LION_FLAG_GROUPIDX))
		lion_explain_group_index(st, ancestors, es, &buf);

	/*
	 * The FK-side join's key first (DESIGN.md §27): the fk index and the
	 * clause, whose value prints as the dimension column it is read from.
	 */
	if (st->join != NULL)
	{
		LionClauseState *cl = &st->clause[st->join->clause];

		/* a partitioned fact table's fk index is one per partition (§16) */
		if (st->npart == 0)
			appendStringInfo(&buf, "%s%s ", get_rel_name(cl->idxoid),
							 lion_explain_clause_col(st, cl));
		appendStringInfoChar(&buf, '(');
		lion_explain_clause(st, cl, ancestors, es, &buf);
		appendStringInfoChar(&buf, ')');
	}

	/*
	 * The plan's items (a partition's own may leave some out).  One that
	 * every partition leaves out, because their bounds imply it, reads no
	 * index at all, and is printed on a line of its own below (DESIGN.md §16,
	 * "Clauses the partition bounds imply").  Beside filtered counts they
	 * are the WHERE's, run 0's.
	 */
	for (i = 0; i < lion_explain_nwhere(st); i++)
		lion_explain_item(st, &lion_explain_where(st)[i], ancestors, es, &buf);

	ExplainPropertyText("Lion Indexes", buf.data, es);
	pfree(buf.data);

	/*
	 * ... and each filtered count (DESIGN.md §10, "Filtered counts"): the
	 * items its filter adds to the WHERE's, which are its run's after them.
	 */
	for (r = 1; r < st->nrun; r++)
	{
		char		label[64];

		initStringInfo(&buf);
		for (i = st->nrunitem[0]; i < st->nrunitem[r]; i++)
			lion_explain_item(st, &st->runitem[r][i], ancestors, es, &buf);
		snprintf(label, sizeof(label), "Count Filter %d", r);
		ExplainPropertyText(label, buf.data, es);
		pfree(buf.data);
	}

	lion_explain_slice_ranges(st, ancestors, es);
}

/*
 * One range from slices (DESIGN.md §53) for "Range From Slices": the bounds
 * on lead's column among clauses [first, first + n) that are a range from
 * slices, ANDed - `x > 10 AND x <= 20` - and the slices its column has,
 * `(32 slices)`.
 */
static void
lion_explain_slice_range(LionCountScanState *st, int lead, int first, int n,
						 List *ancestors, ExplainState *es, StringInfo buf)
{
	LionClauseState *cl = &st->clause[lead];
	bool		firstbound = true;
	int			nbits = 0;
	int			j;

	if (buf->len > 0)
		appendStringInfoString(buf, ", ");
	for (j = first; j < first + n; j++)
	{
		LionClauseState *b = &st->clause[j];

		if (b->kind != LION_CLAUSE_RANGESRC || !b->sliced ||
			b->attno != cl->attno || st->inor[j] != st->inor[lead])
			continue;
		if (!firstbound)
			appendStringInfoString(buf, " AND ");
		lion_explain_clause(st, b, ancestors, es, buf);
		firstbound = false;
	}
	if (OidIsValid(cl->idxoid) && cl->attno > 0)
	{
		Relation	idx = index_open(cl->idxoid, AccessShareLock);
		AttrNumber	col = lion_index_col_for_kind(idx, cl->attno, cl->kind,
												  st->plan.vcols);

		nbits = lion_index_column_state(idx, col)->slicebits;
		index_close(idx, AccessShareLock);
	}
	appendStringInfo(buf, " (%d slices)", nbits);
}

/*
 * "Range From Slices" (DESIGN.md §53): each range the plan collects from the
 * slices of its column - a WHERE's, and an OR's leaf, one an arm - with its
 * bounds and the slices its column has.
 */
static void
lion_explain_slice_ranges(LionCountScanState *st, List *ancestors,
						  ExplainState *es)
{
	StringInfoData buf;
	int			i;
	int			j;
	int			o;

	initStringInfo(&buf);

	/* outside the ORs: the first bound of each column's range */
	for (i = 0; i < st->nclause; i++)
	{
		LionClauseState *cl = &st->clause[i];

		if (cl->kind != LION_CLAUSE_RANGESRC || !cl->sliced || st->inor[i])
			continue;
		for (j = 0; j < i; j++)
		{
			if (st->clause[j].kind == LION_CLAUSE_RANGESRC &&
				st->clause[j].sliced && !st->inor[j] &&
				st->clause[j].attno == cl->attno)
				break;
		}
		if (j < i)
			continue;
		lion_explain_slice_range(st, i, 0, st->nclause, ancestors, es, &buf);
	}

	/* ... and in each arm of an OR */
	for (o = 0; o < st->nor; o++)
	{
		LionOrState *orst = &st->ors[o];
		int			armfirst = orst->first;
		int			a;

		for (a = 0; a < orst->narms; a++)
		{
			int			len = orst->armlen[a];

			for (i = armfirst; i < armfirst + len; i++)
			{
				LionClauseState *cl = &st->clause[i];

				if (cl->kind != LION_CLAUSE_RANGESRC || !cl->sliced)
					continue;
				for (j = armfirst; j < i; j++)
				{
					if (st->clause[j].kind == LION_CLAUSE_RANGESRC &&
						st->clause[j].sliced &&
						st->clause[j].attno == cl->attno)
						break;
				}
				if (j < i)
					continue;
				lion_explain_slice_range(st, i, armfirst, len, ancestors, es,
										 &buf);
			}
			armfirst += len;
		}
	}

	if (buf.len > 0)
		ExplainPropertyText("Range From Slices", buf.data, es);
	pfree(buf.data);
}

/*
 * The lines of a partitioned scan's partition bounds (DESIGN.md §16): the
 * clauses they imply, and the arms of an OR they refute.
 */
static void
lion_explain_bounds(LionCountScanState *st, List *ancestors, ExplainState *es)
{
	StringInfoData buf;
	int			i;
	ListCell   *lc;

	initStringInfo(&buf);
	foreach(lc, st->plan.implied)
	{
		if (buf.len > 0)
			appendStringInfoString(&buf, ", ");
		appendStringInfoString(&buf, strVal(lfirst(lc)));
	}
	for (i = 0; i < lion_explain_nwhere(st); i++)
	{
		LionSourceItem *it = &lion_explain_where(st)[i];
		int			j;

		if (it->orno < 0 && it->rangesrc)
		{
			for (j = 0; j < st->nclause; j++)
			{
				if (st->clause[j].kind != LION_CLAUSE_RANGESRC ||
					st->inor[j] ||
					st->clause[j].attno != st->clause[it->clauseno].attno)
					continue;
				lion_explain_implied(st, j, NULL, ancestors, es, &buf);
			}
		}
		else if (it->orno < 0)
			lion_explain_implied(st, it->clauseno, NULL, ancestors, es,
								 &buf);
		else
			lion_explain_implied(st, st->ors[it->orno].first,
								 &st->ors[it->orno], ancestors, es, &buf);
	}

	/*
	 * ... and the residual clauses some partitions' bounds imply, which
	 * those test none of their rows for (DESIGN.md §16, "Residual clauses
	 * over a partitioned table"), deparsed as the Residual Filter line is.
	 */
	if (st->residexprs != NIL && st->npart > 0)
	{
		List	   *context = NIL;
		int			j = 0;

		foreach(lc, st->residexprs)
		{
			int			n = 0;
			int			p;

			for (p = 0; p < st->npart; p++)
			{
				if (st->part[p].residdrop != NULL && st->part[p].residdrop[j])
					n++;
			}
			if (n > 0)
			{
				if (context == NIL)
					context = set_deparse_context_plan(es->deparse_cxt,
													   st->css.ss.ps.plan,
													   ancestors);
				if (buf.len > 0)
					appendStringInfoString(&buf, ", ");
				appendStringInfoString(&buf,
									   deparse_expression((Node *) lfirst(lc),
														  context, false,
														  false));
				lion_explain_part_count(st, n, &buf);
			}
			j++;
		}
	}
	if (buf.len > 0)
		ExplainPropertyText("Implied by Partition Bounds", buf.data, es);
	pfree(buf.data);

	/* ... and the arms of an OR a partition's bounds refute */
	initStringInfo(&buf);
	for (i = 0; i < lion_explain_nwhere(st); i++)
	{
		LionSourceItem *it = &lion_explain_where(st)[i];

		if (it->orno >= 0)
			lion_explain_refuted(st, &st->ors[it->orno], ancestors, es,
								 &buf);
	}
	if (buf.len > 0)
		ExplainPropertyText("Refuted by Partition Bounds", buf.data, es);
	pfree(buf.data);
}

/*
 * The FK-side join's lines (DESIGN.md §27): its type, keys and rows, its fact
 * filters and the fact column it groups by.
 */
static void
lion_explain_join(LionCountScanState *st, ExplainState *es)
{
	LionJoinState *js = lion_st_join(st);
	StringInfoData buf;
	int			i;

	/*
	 * A forward semi join over a non-unique key counts the fact rows of
	 * each DISTINCT key of the dimension, which it sorts to find them.
	 */
	/* ... which a semi or anti join path's name says (LionSemiJoin) */
	if (!js->outer &&
		(js->type == LION_JOIN_SEMI || js->sort.unique))
		ExplainPropertyText("Join Type", "Semi", es);
	else if (!js->outer && js->type == LION_JOIN_ANTI)
		ExplainPropertyText("Join Type", "Anti", es);
	else if (js->left)
		ExplainPropertyText("Join Type", "Left", es);
	if (js->sort.unique)
		ExplainPropertyText("Join Keys", "distinct, sorted", es);

	/*
	 * The keys are looked up a batch at a time, sorted into the fk
	 * index's order ("Lookups in key order").
	 */
	if (js->batch.walk)
		ExplainPropertyText("Join Key Lookups", "in index order", es);
	/*
	 * A semi or anti join path's rows are the outer side's (DESIGN.md
	 * §27, "The semi and anti join as a join path"), in the child's order
	 * where the path claims it.
	 */
	if (js->outer)
		ExplainPropertyText("Join Rows",
							js->type == LION_JOIN_ANTI ?
							(js->ordered ?
							 "the outer rows without a match, in their order" :
							 "the outer rows without a match") :
							(js->ordered ?
							 "the outer rows with a match, in their order" :
							 "the outer rows with a match"), es);
	else if (js->left)
		ExplainPropertyText("Join Rows",
							"every dimension row, each with its count", es);
	else if (js->rows)
		ExplainPropertyText("Join Rows",
							js->type == LION_JOIN_ANTI ?
							"the dimension rows without a match" :
							js->sort.unique ?
							(js->counts ?
							 "the distinct keys with a match, each with its count" :
							 "the distinct keys with a match") :
							js->counts ?
							"the dimension rows with a match, each with its count" :
							"the dimension rows with a match", es);
	if (js->collect)
		ExplainPropertyText("Fact Filters", "collected once", es);
	else if (es->analyze &&
			 js->switches + js->worker.switches > 0)
		ExplainPropertyText("Fact Filters", "probed, then collected", es);

	/*
	 * A fact column grouped by ("Grouped by a fact column"): each key is
	 * counted once per group of it - an entry of its index, or, in a
	 * partition whose bounds give it one value, that value.
	 */
	if (st->fg != NULL)
	{
		int			bound = 0;

		ExplainPropertyText("Fact Group Key",
							lion_explain_attname(st, st->fg->attno),
							es);

		/* ... or counted once against the union of a batch's keys */
		if (js->unite)
			ExplainPropertyText("Fact Groups Counted Against",
								"united join keys", es);
		for (i = 0; i < st->npart; i++)
			bound += (st->part[i].fgconst != NULL) ? 1 : 0;
		if (bound > 0)
		{
			initStringInfo(&buf);
			appendStringInfo(&buf, "%d of %d partitions", bound,
							 st->npart);
			ExplainPropertyText("Fact Groups From Partition Bounds",
								buf.data, es);
			pfree(buf.data);
		}
	}
}

/*
 * The "Group Key" of a GROUP BY and the "Distinct Key" of a count(DISTINCT).
 */
static void
lion_explain_group_keys(LionCountScanState *st, List *ancestors,
						ExplainState *es)
{
	StringInfoData buf;

	if (lion_plan_flag(st, LION_FLAG_GROUPIDX) && st->plan.groupattno != 0 &&
		st->coal != NULL)
	{
		/*
		 * GROUP BY coalesce(g, c) (DESIGN.md §10), printed as core prints the
		 * expression, with c deparsed like a clause value.
		 */
		List	   *context = set_deparse_context_plan(es->deparse_cxt,
													   st->css.ss.ps.plan,
													   ancestors);
		char	   *val = deparse_expression((Node *) st->coal->value, context,
											 false, false);

		initStringInfo(&buf);
		appendStringInfo(&buf, "COALESCE(%s, %s)",
						 lion_explain_attname(st, st->plan.groupattno),
						 val);
		ExplainPropertyText("Group Key", buf.data, es);
		pfree(buf.data);
		pfree(val);
	}
	else if (st->cells != NULL)
	{
		/* a count by cells (DESIGN.md §10, "Counts by cells") */
		lion_explain_cells(st, ancestors, es);
	}
	else if (st->decode != NULL)
	{
		/*
		 * The decoded walk (DESIGN.md §34): every column, in the order it
		 * takes them, the first the one whose sets it pins.
		 */
		int			c;

		initStringInfo(&buf);
		for (c = 0; c < st->decode->ncol; c++)
			appendStringInfo(&buf, "%s%s", (c > 0) ? ", " : "",
							 lion_explain_attname(st, st->decode->attno[c]));
		ExplainPropertyText("Group Key", buf.data, es);
		ExplainPropertyText("Group Strategy", "Decoded", es);
		pfree(buf.data);
	}
	else if (lion_plan_flag(st, LION_FLAG_GROUPIDX) &&
			 st->plan.groupattno != 0)
	{
		initStringInfo(&buf);
		appendStringInfoString(&buf,
							   lion_explain_attname(st, st->plan.groupattno));
		if (st->plan.groupattno2 != 0)
			appendStringInfo(&buf, ", %s",
							 lion_explain_attname(st, st->plan.groupattno2));
		ExplainPropertyText("Group Key", buf.data, es);
		pfree(buf.data);
	}

	/* The column count(DISTINCT) counts (DESIGN.md §26). */
	if (st->plan.distattno != 0)
		ExplainPropertyText("Distinct Key",
							lion_explain_attname(st, st->plan.distattno),
							es);

	/* the columns aggregates are taken over (DESIGN.md §37) */
	if (st->wagg != NULL)
	{
		int			c;

		initStringInfo(&buf);
		for (c = 0; c < st->wagg->ncol; c++)
			appendStringInfo(&buf, "%s%s (%s)", (c > 0) ? ", " : "",
							 get_rel_name(st->wagg->col[c].idxoid),
							 lion_explain_attname(st, st->wagg->col[c].attno));
		ExplainPropertyText("Aggregates Over Keys", buf.data, es);
		pfree(buf.data);
	}

	/*
	 * The sliced columns aggregates are answered from (DESIGN.md §52): each
	 * index and column, and which of its slices are counted against the
	 * WHERE - every bit's and E's for a sum or an average, E's alone
	 * otherwise - and the descents its extremes take.  A partitioned scan
	 * names no index, as it names none in Lion Indexes: each partition reads
	 * its own (§52.13).
	 */
	if (st->slice != NULL)
	{
		int			c;

		initStringInfo(&buf);
		for (c = 0; c < st->slice->ncol; c++)
		{
			LionSCol   *col = &st->slice->col[c];
			int			want = col->want;

			appendStringInfo(&buf, "%s%s%s(%s: %s%s%s)", (c > 0) ? ", " : "",
							 (st->npart > 0) ? "" :
							 get_rel_name(col->idxoid),
							 (st->npart > 0) ? "" : " ",
							 lion_explain_attname(st, col->attno),
							 (want & LION_SWANT_SUM) ?
							 "every bit counted" : "existence counted",
							 (want & LION_SWANT_MIN) ? ", min descent" : "",
							 (want & LION_SWANT_MAX) ? ", max descent" : "");
		}
		ExplainPropertyText("Aggregates From Slices", buf.data, es);
		pfree(buf.data);

		/* a GROUP BY's: a group at a time, or decoded a batch at a time */
		if (st->plan.groupattno != 0)
			ExplainPropertyText("Slice Groups",
								st->slice->decode ? "decoded by batch" :
								"one at a time", es);
	}

	/*
	 * The top k by count (DESIGN.md §36): k, and how many entries the walk of
	 * their counts keeps as candidates.
	 */
	if (st->topk != NULL)
	{
		initStringInfo(&buf);
		appendStringInfo(&buf, INT64_FORMAT " by count%s, %d candidates",
						 st->topk->n, st->topk->strict ? " with ties" : "",
						 st->topk->cand);
		ExplainPropertyText("Top K", buf.data, es);
		pfree(buf.data);
	}
}

/*
 * EXPLAIN ANALYZE's counters of every count: the heap and its recheck, the
 * containers and the AND merge, the heap cache, the posting sets summed and
 * the directory pages read.
 */
static void
lion_explain_scan_counters(LionCountScanState *st, const LionCountStats *tot,
						   ExplainState *es)
{
	ExplainPropertyInteger("Heap Blocks Skipped via VM", NULL,
						   tot->blocks_skipped_via_vm, es);
	ExplainPropertyInteger("Heap TIDs Rechecked", NULL,
						   tot->tids_rechecked, es);
	ExplainPropertyInteger("Heap Blocks Rechecked", NULL,
						   tot->blocks_rechecked, es);

	/*
	 * The candidates of a count taken in passes - a disjoint sum, a list in
	 * batches - that outgrew the recheck budget and were sorted, past
	 * work_mem in temporary files, so that every dirty block was still read
	 * once (DESIGN.md §9 step 5).  Printed only when there were some, as a
	 * Sort prints its disk use only when it has any.
	 */
	if (tot->tids_spilled > 0)
		ExplainPropertyInteger("Heap TIDs Spilled", NULL,
							   tot->tids_spilled, es);

	/*
	 * Rows the heap recheck of a multi-key query turned away (DESIGN.md
	 * §17, "A query known only at run time"): candidates of a superset
	 * that the query, tested on the row, does not select.  Printed only
	 * when there were some, as core prints "Rows Removed by Index
	 * Recheck".
	 */
	if (tot->rows_removed > 0)
		ExplainPropertyInteger("Rows Removed by Recheck", NULL,
							   tot->rows_removed, es);

	/*
	 * ... and what a query decided from stored positions did instead
	 * (DESIGN.md §17, "Stored positions"): the candidates it decided, and
	 * those it turned away without the heap.  Only for such a query.
	 */
	if (tot->pos_checked > 0)
	{
		ExplainPropertyInteger("Position Checks", NULL,
							   tot->pos_checked, es);
		ExplainPropertyInteger("Rows Removed by Positions", NULL,
							   tot->pos_removed, es);
	}
	ExplainPropertyInteger("Containers Visited", NULL,
						   tot->containers_visited, es);
	/*
	 * Probes the AND merge did not make because the container key was
	 * already ruled out (DESIGN.md §25).  Each one is a seek into another
	 * source - a descent, or a step or two right - that the merge used to
	 * make before it knew whether anything survived at that key.
	 */
	ExplainPropertyInteger("Probes Avoided", NULL,
						   tot->probes_avoided, es);

	/*
	 * How the AND met its unions (DESIGN.md §29.11, "Unions probed"): the
	 * container keys at which an IN list's, a multi-key query's or an OR's
	 * union of two containers or more was built, and those at which the
	 * running intersection was looked up in the containers instead.
	 * Printed only where there was a union to meet.
	 */
	if (tot->unions_built > 0 || tot->unions_probed > 0)
	{
		ExplainPropertyInteger("Unions Built", NULL,
							   tot->unions_built, es);
		ExplainPropertyInteger("Unions Probed", NULL,
							   tot->unions_probed, es);
	}

	/*
	 * ... and its nested trees (DESIGN.md §29.11, "Trees probed"): the
	 * keys at which a tree it met past its driver - an AND of ORs, an OR
	 * of ANDs - was built, and those at which it was evaluated for the
	 * running intersection's members alone.  Printed only where it met
	 * one there.
	 */
	if (tot->trees_built > 0 || tot->trees_probed > 0)
	{
		ExplainPropertyInteger("Trees Built", NULL,
							   tot->trees_built, es);
		ExplainPropertyInteger("Trees Probed", NULL,
							   tot->trees_probed, es);
	}
	ExplainPropertyInteger("Heap Blocks From Cache", NULL,
						   tot->cache_hits, es);
	ExplainPropertyInteger("Heap Blocks Past Cache Budget", NULL,
						   tot->cache_full, es);
	/*
	 * Posting sets counted on their own and added up instead of merged:
	 * the disjoint-sum short-circuit of DESIGN.md §15.  Zero means every
	 * container key went through the k-way union, which is what an IN list
	 * ANDed with another clause, and every multi-key clause, still do.
	 */
	ExplainPropertyInteger("Posting Sets Summed", NULL,
						   tot->sets_summed, es);
	/*
	 * Directory pages - leaves and internal pages both - this node read
	 * (DESIGN.md §21).  A sorted IN list should cost about the leaves its
	 * values live on plus one descent, not a descent per value.
	 */
	ExplainPropertyInteger("Directory Pages Read", NULL,
						   st->dirpages + st->worker.dirpages, es);
}

/*
 * EXPLAIN ANALYZE's counters of ranges and sums: how the range bounding a
 * sum was evaluated, the summaries and walks it used, and the ranges and
 * lists taken as sources - the leader's and, under a Gather, its workers'
 * (st->worker, st->workercounted).
 */
static void
lion_explain_range_counters(LionCountScanState *st, ExplainState *es)
{
	StringInfoData buf;
	int64		eval[lengthof(st->rangeeval)];
	int64		ranges[3] = {0, 0, 0};	/* collected, walked, spilled */
	int64		lists[3] = {0, 0, 0};
	int			i;
	int			k;

	for (k = 0; k < (int) lengthof(eval); k++)
		eval[k] = st->rangeeval[k] + st->worker.rangeeval[k];

	/*
	 * How the range bounding a sum was evaluated (DESIGN.md §28, "The
	 * complement"): from the entries it selects ("inside"), from the ones
	 * it does not ("complement"), or from none because it selects them all
	 * ("full domain").  A partitioned table, or a rescan, may take more
	 * than one way; each is then printed with how often it was taken.
	 */
	if (lion_plan_flag(st, LION_FLAG_RANGE) &&
		lion_plan_flag(st, LION_FLAG_SUMALL))
	{
		static const char *const evalname[] = {"inside", "complement",
		"full domain"};
		int			kinds = 0;

		StaticAssertDecl(lengthof(evalname) == lengthof(eval),
						 "a name for every way of evaluating a range");
		initStringInfo(&buf);
		for (k = 0; k < (int) lengthof(evalname); k++)
			if (eval[k] > 0)
				kinds++;
		for (k = 0; k < (int) lengthof(evalname); k++)
		{
			if (eval[k] == 0)
				continue;
			if (buf.len > 0)
				appendStringInfoString(&buf, ", ");
			appendStringInfoString(&buf, evalname[k]);
			if (kinds > 1)
				appendStringInfo(&buf, " %lld", (long long) eval[k]);
		}
		if (buf.len > 0)
			ExplainPropertyText("Range Evaluation", buf.data, es);
		pfree(buf.data);
	}

	/*
	 * The summaries (DESIGN.md §32) a sum added up in place of the keys
	 * they cover, among the Posting Sets Summed.  Only when there were
	 * any, so that an index without summaries prints what it always did.
	 */
	if (st->summaries + st->worker.summaries > 0)
		ExplainPropertyInteger("Summaries Summed", NULL,
							   st->summaries + st->worker.summaries, es);

	/*
	 * Of those sums, the walks that were probed at the other sources'
	 * rows rather than counted a set at a time (DESIGN.md §32, "Summed
	 * ranges: dense and probed").  Only when there were any.
	 */
	if (st->rangeprobed + st->worker.rangeprobed > 0)
		ExplainPropertyInteger("Range Walks Probed", NULL,
							   st->rangeprobed + st->worker.rangeprobed, es);

	/*
	 * The ranges taken as sources (DESIGN.md §32): collected, or walked at
	 * every count they are part of, where the plan walks those - and of the
	 * collected, those spilled to a file, past the memory; and the IN lists
	 * too long to locate at once that were collected the same way (§15, "A
	 * list too long to locate at once"), and of those the ones spilled.  How
	 * many sources, each once however many partitions, runs, rescans or
	 * participants collected it (LionClauseState.counted); again only when
	 * there were any.
	 */
	for (i = 0; i < st->nclause; i++)
	{
		uint8		counted = st->clause[i].counted |
			((st->workercounted != NULL) ? st->workercounted[i] : 0);
		int64	   *n = (st->clause[i].kind == LION_CLAUSE_ARRAY) ? lists :
			ranges;

		if (counted & LION_SRC_COLLECTED)
			n[0]++;
		if (counted & LION_SRC_WALKED)
			n[1]++;
		if (counted & LION_SRC_SPILLED)
			n[2]++;
	}
	if (ranges[0] > 0)
		ExplainPropertyInteger("Range Sources Collected", NULL, ranges[0], es);
	if (ranges[1] > 0)
		ExplainPropertyInteger("Range Sources Walked", NULL, ranges[1], es);
	if (ranges[2] > 0)
		ExplainPropertyInteger("Range Sources Spilled", NULL, ranges[2], es);
	if (lists[0] > 0)
		ExplainPropertyInteger("Lists Collected", NULL, lists[0], es);
	if (lists[2] > 0)
		ExplainPropertyInteger("Lists Spilled", NULL, lists[2], es);

	/*
	 * The ranges collected from slices (DESIGN.md §53): the collections -
	 * each range once a relation, a run and a rescan, in every participant
	 * - the container keys of E they folded, their bit steps (a comparison
	 * of a slice's container with the key's rows each: the container
	 * operations the planner prices) and the collections that went to a
	 * file.  Only when there were any.
	 */
	{
		const LionSliceRangeStats *a = &st->sliceranges;
		const LionSliceRangeStats *w = &st->worker.sliceranges;

		if (a->descents + w->descents > 0)
		{
			ExplainPropertyInteger("Slice Range Descents", NULL,
								   a->descents + w->descents, es);
			ExplainPropertyInteger("Slice Range Keys", NULL,
								   a->keys + w->keys, es);
			ExplainPropertyInteger("Slice Range Steps", NULL,
								   a->steps + w->steps, es);
		}
		if (a->spilled + w->spilled > 0)
			ExplainPropertyInteger("Slice Range Copies Spilled", NULL,
								   a->spilled + w->spilled, es);
	}
}

/*
 * EXPLAIN ANALYZE's counters of a GROUP BY's WHERE sets, batches and key
 * ranges, of the decoded walk, of an IN list's batches and of a
 * count(DISTINCT)'s tests - the leader's and, under a Gather, its workers'
 * (st->worker).
 */
static void
lion_explain_group_counters(LionCountScanState *st, ExplainState *es)
{
	LionRangedState *rs = st->ranged;
	const LionScanWorkerSums *w = &st->worker;

	/*
	 * The WHERE items of a GROUP BY collected into one set that the groups
	 * were counted against (DESIGN.md §10, "The WHERE sets, collected
	 * once"): once per relation - per partition, and again on a rescan -
	 * that did; and of those, the ones past a hash table's memory that
	 * went to a temporary file.  Only when there were any.
	 */
	if (st->wherecollected + w->wherecollected > 0)
		ExplainPropertyInteger("WHERE Sets Collected", NULL,
							   st->wherecollected + w->wherecollected, es);
	if (st->wherespilled + w->wherespilled > 0)
		ExplainPropertyInteger("WHERE Sets Spilled", NULL,
							   st->wherespilled + w->wherespilled, es);

	/*
	 * ... and the batches of groups counted together against it, in one
	 * walk of container keys each (DESIGN.md §10, "The groups of a walk,
	 * counted together").  Only when there were any.
	 */
	if (st->groupbatches + w->groupbatches > 0)
	{
		ExplainPropertyInteger("Group Batches", NULL,
							   st->groupbatches + w->groupbatches, es);
		ExplainPropertyInteger("Groups Counted in Batches", NULL,
							   st->groupsbatched + w->groupsbatched, es);
	}

	/*
	 * The decoded walk's passes (DESIGN.md §34): the container keys it
	 * decoded, the rows it counted under their combinations - from the map
	 * or the heap - and those the heap was asked about, the partial rows it
	 * handed up, and how often a pass's tally went to its temporary file.
	 */
	if (st->decode != NULL && st->decode->passes + w->decodepasses > 0)
	{
		LionDecodeRun *dr = st->decode;

		ExplainPropertyInteger("Decoded Passes", NULL,
							   dr->passes + w->decodepasses, es);
		ExplainPropertyInteger("Decoded Keys", NULL,
							   dr->stats.keys + w->decode.keys, es);
		ExplainPropertyInteger("Decoded Rows", NULL,
							   dr->stats.rows + w->decode.rows, es);
		ExplainPropertyInteger("Decoded Rows Rechecked", NULL,
							   dr->stats.rechecked + w->decode.rechecked, es);
		ExplainPropertyInteger("Partial Rows", NULL,
							   dr->rowsup + w->decoderowsup, es);
		if (dr->stats.spills + w->decode.spills > 0)
			ExplainPropertyInteger("Tally Spills", NULL,
								   dr->stats.spills + w->decode.spills, es);
	}

	/*
	 * The ranges of container keys a parallel GROUP BY's participants
	 * counted (DESIGN.md §10, "A GROUP BY in parallel"), the leader's and
	 * its workers' - each range a WHERE collected and a walk of the
	 * entries - which is every range of the cut, whichever participants
	 * took them; of a parallel count (§10, "A count in parallel"), each
	 * range of each run's count.
	 */
	if (rs != NULL)
		ExplainPropertyInteger("Key Ranges", NULL,
							   rs->ranges + rs->workerranges, es);

	/*
	 * The batches an IN list too long to locate at once was counted in
	 * (DESIGN.md §15, "A list too long to locate at once"), or located in as
	 * it drove the groups.  Only when there were any.
	 */
	{
		int64		batches = st->glistbatches + w->listbatches;

		if (st->lbatch != NULL)
			batches += st->lbatch->batches;

		if (batches > 0)
			ExplainPropertyInteger("List Batches", NULL, batches, es);
	}

	/*
	 * The existence (or count) tests a count(DISTINCT k) made: one per
	 * entry of k without a GROUP BY, one per group and one per (g, k) pair
	 * with one (DESIGN.md §26).
	 */
	if (st->dist != NULL)
		ExplainPropertyInteger("Distinct Keys Tested", NULL, st->dist->tests,
							   es);

	/*
	 * The aggregates over keys (DESIGN.md §37): the entries they took, and
	 * how many walks read the entries' own counts and how many counted them.
	 */
	if (st->wagg != NULL)
	{
		ExplainPropertyInteger("Keys Aggregated", NULL, st->wagg->entries,
							   es);
		ExplainPropertyInteger("Key Walks From Entry Counts", NULL,
							   st->wagg->fast, es);
		if (st->wagg->slow > 0)
			ExplainPropertyInteger("Key Walks Counted", NULL, st->wagg->slow,
								   es);
	}

	/*
	 * The aggregates of sliced columns (DESIGN.md §52): the slices located
	 * and how many of them had no entry, the counted ANDs, the slices'
	 * containers the walks counted - and of those the plane kernel's, at how
	 * many keys, and which kernel - the descents' steps and the candidate
	 * sets they collected, and the copies of E ∧ WHERE made.
	 */
	if (st->slice != NULL && st->slice->runs > 0)
	{
		LionSliceState *ss = st->slice;

		ExplainPropertyInteger("Slices Located", NULL, ss->located, es);
		if (ss->absent > 0)
			ExplainPropertyInteger("Slices Absent", NULL, ss->absent, es);
		if (ss->counts > 0 || ss->batches == 0)
		{
			ExplainPropertyInteger("Slice Counts", NULL, ss->counts, es);
			ExplainPropertyInteger("Slice Containers Counted", NULL,
								   ss->containers, es);
		}
		if (ss->kernelplanes > 0)
		{
			ExplainPropertyInteger("Slice Containers by Kernel", NULL,
								   ss->kernelplanes, es);
			ExplainPropertyInteger("Slice Kernel Keys", NULL, ss->kernelkeys,
								   es);
			ExplainPropertyText("Slice Kernel",
								lion_simd_name(lion_simd_current()), es);
		}
		if (ss->steps > 0)
			ExplainPropertyInteger("Slice Descent Steps", NULL, ss->steps,
								   es);
		if (ss->collects > 0)
			ExplainPropertyInteger("Slice Candidate Sets", NULL,
								   ss->collects, es);
		ExplainPropertyInteger("Slice WHERE Copies", NULL, ss->copies, es);
		if (ss->batches > 0)
		{
			ExplainPropertyInteger("Slice Batches Decoded", NULL, ss->batches,
								   es);
			ExplainPropertyInteger("Slice Keys Decoded", NULL, ss->decodekeys,
								   es);
			ExplainPropertyInteger("Slice Rows Decoded", NULL, ss->decoderows,
								   es);
			if (ss->decoderechecked > 0)
				ExplainPropertyInteger("Slice Rows Rechecked", NULL,
									   ss->decoderechecked, es);
		}
		if (ss->tidsrechecked > 0)
		{
			ExplainPropertyInteger("Slice Heap TIDs Rechecked", NULL,
								   ss->tidsrechecked, es);
			ExplainPropertyInteger("Slice Heap Blocks Rechecked", NULL,
								   ss->blocksrechecked, es);
		}
	}

	/*
	 * The top k (DESIGN.md §36): the entries whose counts were read, the
	 * groups counted, and how often the candidates were not enough and every
	 * group was counted instead.
	 */
	if (st->topk != NULL)
	{
		ExplainPropertyInteger("Top K Entries Walked", NULL, st->topk->walked,
							   es);
		ExplainPropertyInteger("Top K Groups Counted", NULL,
							   st->topk->counted, es);
		if (st->topk->wholes > 0)
			ExplainPropertyInteger("Top K Walked Whole", NULL,
								   st->topk->wholes, es);
	}
}

/*
 * EXPLAIN ANALYZE's counters of the FK-side join's fact filters (DESIGN.md
 * §27): the rows collected, and the copies made of them and read.
 */
static void
lion_explain_fact_filter_counters(LionCountScanState *st,
								  const LionCountStats *tot, int64 switches,
								  bool switched, ExplainState *es)
{
	LionJoinState *js = lion_st_join(st);

	/*
	 * The rows of the collected fact filters, or -1 when the plan
	 * collected them and the run could not (a standby) and every
	 * count read the filters instead - in a parallel plan, the
	 * largest copy any participant made, which is the one they all
	 * read when they share it.
	 */
	if (js->collect || switched)
		ExplainPropertyInteger("Fact Filter Rows Collected", NULL,
							   Max(js->filterrows,
								   js->worker.filterrows), es);

	/*
	 * Copies past a hash join's memory, which went to a temporary
	 * file instead: one per participant and run - or one a run, a
	 * copy shared by the participants (any of whose chunks went to
	 * a file).  Only when there were any.
	 */
	if (js->spilled + js->worker.spilled > 0)
		ExplainPropertyInteger("Fact Filter Copies Spilled", NULL,
							   js->spilled + js->worker.spilled,
							   es);

	/*
	 * The copies collected once for all the participants of a
	 * parallel plan - one a run - and the chunks the participants
	 * collected of them, summed.  Only when there were any.
	 */
	if (js->pcopy.copies + js->worker.copies > 0)
	{
		ExplainPropertyInteger("Fact Filter Copies Shared", NULL,
							   js->pcopy.copies + js->worker.copies,
							   es);
		ExplainPropertyInteger("Fact Filter Copy Chunks", NULL,
							   js->pcopy.chunks +
							   js->worker.copychunks, es);
	}

	/*
	 * A plan that probed the filters and collected them part way
	 * through (DESIGN.md §27, "Probed, then collected"): the copies
	 * made so - one a run, a leaf of a partitioned fact or a
	 * participant of a parallel plan - and the keys counted by
	 * probing before each, summed.  Only when there were any.
	 */
	if (switched)
	{
		ExplainPropertyInteger("Fact Filter Switches", NULL,
							   switches, es);
		ExplainPropertyInteger("Fact Filter Keys Probed", NULL,
							   js->switchkeys +
							   js->worker.switchkeys, es);
	}

	/*
	 * What the counts read of the copy: its containers, the binary
	 * searches that found them, and of those the ones read back from
	 * a spilled copy's temporary file - one read each, never the
	 * copy again.
	 */
	if (js->collect || switched)
	{
		ExplainPropertyInteger("Fact Filter Copy Containers Read", NULL,
							   tot->copy_containers, es);
		ExplainPropertyInteger("Fact Filter Copy Seeks", NULL,
							   tot->copy_seeks, es);
		ExplainPropertyInteger("Fact Filter Copy File Reads", NULL,
							   tot->copy_file_reads, es);
	}
}

/*
 * EXPLAIN ANALYZE's counters of the FK-side join (DESIGN.md §27): its keys,
 * what their counts read, its fact filters and, with TIMING, its phases.
 */
static void
lion_explain_join_counters(LionCountScanState *st, const LionCountStats *tot,
						   ExplainState *es)
{
	LionJoinState *js = lion_st_join(st);
	int64		switches = js->switches + js->worker.switches;
	bool		switched = (switches > 0);
	int			i;

	/*
	 * A forward semi join's keys: the child rows with one, sorted - by
	 * every participant of a parallel plan, so what one of them sorted
	 * in each run, summed over the runs - and how the sort went here,
	 * if it ran here.  "Looked Up" below is then the DISTINCT keys,
	 * each by one participant.
	 */
	if (js->sort.unique)
	{
		ExplainPropertyInteger("Join Keys Sorted", NULL,
							   Max(js->sort.sorted,
								   js->worker.sorted), es);
		if (js->sort.havestats)
		{
			ExplainPropertyText("Join Key Sort Method",
								tuplesort_method_name(js->sort.stats.sortMethod),
								es);
			ExplainPropertyInteger("Join Key Sort Space Used", "kB",
								   js->sort.stats.spaceUsed, es);
			ExplainPropertyText("Join Key Sort Space Type",
								tuplesort_space_type_name(js->sort.stats.spaceType),
								es);
		}
	}

	/*
	 * Where a key's time goes (DESIGN.md §27): the rows the child
	 * returned - the keys looked up, and the NULL keys, which join
	 * nothing; every participant's, so for a forward semi join over a
	 * non-unique key, whose participants each run the whole child,
	 * that many times the dimension's rows - ...
	 */
	ExplainPropertyInteger("Join Child Rows", NULL,
						   js->childrows + js->worker.childrows,
						   es);
	ExplainPropertyInteger("Join Keys Looked Up", NULL,
						   js->lookups + js->worker.lookups, es);
	ExplainPropertyInteger("Join Keys Without Entry", NULL,
						   js->missing + js->worker.missing, es);

	/* ... and the keys of the dimensions folded into fact filters */
	if (js->nfilter > 0)
		ExplainPropertyInteger("Join Filter Keys", NULL, js->filterkeys, es);

	/* ... and counted in each group of a fact column grouped by */
	if (st->fg != NULL)
		ExplainPropertyInteger("Fact Group Counts", NULL,
							   st->fg->groupcounts +
							   st->fg->workergroupcounts, es);

	/*
	 * ... what their counts read of the keys' own fk sets - their
	 * containers, and the pages of posting trees, which a set of a
	 * few rows stored INLINE in its directory leaf has none of - and
	 * of the visibility map: the containers it was asked about and the
	 * map pages pinned for them.
	 */
	ExplainPropertyInteger("Join Key Containers Read", NULL,
						   tot->key_containers, es);
	ExplainPropertyInteger("Join Posting Pages Read", NULL,
						   js->posting + js->worker.posting, es);
	ExplainPropertyInteger("Visibility Map Checks", NULL,
						   tot->vm_checks, es);
	ExplainPropertyInteger("Visibility Map Pages Pinned", NULL,
						   tot->vm_pins, es);

	/*
	 * The batches the keys were looked up in, in the fk index's order:
	 * one per work_mem of the child's rows, per participant and run.
	 */
	if (js->batch.walk)
		ExplainPropertyInteger("Join Key Batches", NULL,
							   js->batch.batches + js->worker.batches,
							   es);

	lion_explain_fact_filter_counters(st, tot, switches, switched, es);

	/*
	 * With TIMING, the time spent in each phase, summed over the
	 * participants and the leaves of a partitioned fact table: the
	 * child producing its rows, the lookups, the counts, the locating
	 * of the fact filters (a range among them collected, §32), and
	 * the collection of their copy.  What the node took besides - its
	 * batches, its rows handed up - is its own total less these.
	 */
	if (es->timing)
	{
		static const char *const phasename[LION_JT_N] = {
			"Join Child Time", "Join Lookup Time", "Join Count Time",
			"Fact Filter Locate Time", "Fact Filter Collect Time"
		};

		for (i = 0; i < LION_JT_N; i++)
		{
			instr_time	t = js->time[i];

			if (i == LION_JT_COLLECT && !js->collect && !switched)
				continue;
			INSTR_TIME_ADD(t, js->worker.time[i]);
			ExplainPropertyFloat(phasename[i], "ms",
								 INSTR_TIME_GET_MILLISEC(t), 3, es);
		}
	}
}

/* The counters of EXPLAIN ANALYZE, after the description of the scan. */
static void
lion_explain_analyze(LionCountScanState *st, ExplainState *es)
{
	/*
	 * The leader's own counters, plus - under a Gather - what its workers
	 * added up (lion_shutdown_custom_scan()).
	 */
	LionCountStats tot = st->stats;

	lion_count_stats_add(&tot, &st->worker.stats);

	lion_explain_scan_counters(st, &tot, es);
	lion_explain_range_counters(st, es);
	lion_explain_group_counters(st, es);

	/*
	 * The FK-side join (DESIGN.md §27): the dimension rows whose key was
	 * looked up (a NULL key joins nothing and is not), and how many of
	 * those keys have no entry in the fk index at all.
	 */
	if (st->join != NULL)
		lion_explain_join_counters(st, &tot, es);

	/* ... and the anti join over a small inner side's (DESIGN.md §27) */
	if (st->anti != NULL)
		lion_explain_anti_counters(st, es);
}

void
lion_explain_custom_scan(CustomScanState *node, List *ancestors,
						ExplainState *es)
{
	LionCountScanState *st = (LionCountScanState *) node;

	/*
	 * A partitioned scan uses one index per partition per clause, so there is
	 * no single name to print: the entries are the clauses alone, and the
	 * partitions get a line of their own (DESIGN.md §16).
	 */
	if (st->npart > 0)
		lion_explain_partitions(st, es);

	lion_explain_indexes(st, ancestors, es);
	lion_explain_partial(st, es);

	/*
	 * The residual clauses (DESIGN.md §10, "Residual clauses"), tested on the
	 * heap row of each row the others select, as core prints a scan's Filter.
	 */
	if (st->residexprs != NIL)
	{
		List	   *context = set_deparse_context_plan(es->deparse_cxt,
													   st->css.ss.ps.plan,
													   ancestors);

		ExplainPropertyText("Residual Filter",
							deparse_expression((Node *)
											   make_ands_explicit(st->residexprs),
											   context, false, false),
							es);
	}

	/*
	 * The clauses partitions leave out because their bounds imply them
	 * (DESIGN.md §16), each followed by how many of the partitions do when it
	 * is not all of them.  A range taken as a source is a restriction per
	 * bound, and so is printed a bound at a time.
	 */
	if (st->npart > 0)
		lion_explain_bounds(st, ancestors, es);

	/*
	 * The FK-side join (DESIGN.md §27): what a dimension row counts - its
	 * join pairs, or whether it has any, for a semi or an anti join - and
	 * whether the fact filters are collected once for all of them.  An inner
	 * join that reads the filters per count prints neither, as it always has.
	 */
	if (st->join != NULL)
		lion_explain_join(st, es);

	/*
	 * The anti join over a small inner side (DESIGN.md §27): the B-tree its
	 * child's keys are looked up in, and the clauses each row found is
	 * tested on.
	 */
	if (st->anti != NULL)
		lion_explain_anti(st, ancestors, es);

	lion_explain_group_keys(st, ancestors, es);

	if (es->analyze)
		lion_explain_analyze(st, es);
}
