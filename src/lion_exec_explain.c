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
	const char *attname = get_attname(st->heapoid, cl->attno, false);

	switch (cl->kind)
	{
		case LION_CLAUSE_NULL:
			appendStringInfo(buf, "%s IS NULL", attname);
			break;
		case LION_CLAUSE_NOTNULL:
			appendStringInfo(buf, "%s IS NOT NULL", attname);
			break;
		default:
			{
				List	   *context;
				char	   *opname = get_opname(cl->opno);
				char	   *val;

				if (opname == NULL)
					elog(ERROR, "LionCount: cache lookup failed for operator %u",
						 cl->opno);

				context = set_deparse_context_plan(es->deparse_cxt,
												   st->css.ss.ps.plan,
												   ancestors);

				/*
				 * The FK-side join's key is a column of the OTHER table
				 * (DESIGN.md §27), so it is printed qualified: `fk = d.pk`,
				 * as core prints a join clause.
				 */
				val = deparse_expression((Node *) cl->valexpr, context,
										 st->joinclause >= 0 &&
										 cl == &st->clause[st->joinclause],
										 false);
				if (cl->kind == LION_CLAUSE_ARRAY)
					appendStringInfo(buf, "%s %s ANY (%s)", attname, opname,
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
 */
static const char *
lion_explain_col(Oid idxoid, AttrNumber heapattno, bool multikey)
{
	static char buf[NAMEDATALEN + 2];
	Relation	idx;
	AttrNumber	col;

	if (!OidIsValid(idxoid) || heapattno <= 0)
		return "";

	idx = index_open(idxoid, AccessShareLock);
	if (IndexRelationGetNumberOfKeyAttributes(idx) <= 1)
	{
		index_close(idx, AccessShareLock);
		return "";
	}

	col = lion_index_col_for(idx, heapattno, multikey);
	snprintf(buf, sizeof(buf), ".%s",
			 NameStr(TupleDescAttr(RelationGetDescr(idx), col - 1)->attname));
	index_close(idx, AccessShareLock);

	return buf;
}

/* ... for one clause, whose kind says which kind of column answers it. */
static const char *
lion_explain_clause_col(const LionClauseState *cl)
{
	return lion_explain_col(cl->idxoid, cl->attno,
							cl->kind == LION_CLAUSE_MULTI);
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
 * The entry of "Lion Indexes" for the index whose entries drive the count
 * (hasgroupidx): the index, what is walked of it - a range's bounds, the
 * column or all of its keys - and the inner index of a pair walked, if any.
 */
static void
lion_explain_group_index(LionCountScanState *st, List *ancestors,
						 ExplainState *es, StringInfo buf)
{
	int			i;

	if (st->npart == 0)
		appendStringInfo(buf, "%s%s ", get_rel_name(st->groupidxoid),
						 lion_explain_col(st->groupidxoid,
										  st->driveattno, false));
	if (st->hasrange)
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
	else if (st->groupattno != 0 || st->distattno != 0)
		appendStringInfo(buf, "(%s)",
						 get_attname(st->heapoid, st->driveattno, false));
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
					!st->inor[i] && st->clause[i].attno == st->driveattno);
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
			appendStringInfo(buf, "%s%s ", get_rel_name(st->groupidxoid2),
							 lion_explain_col(st->groupidxoid2,
											  st->innerattno, false));
		appendStringInfo(buf, "(%s)",
						 get_attname(st->heapoid, st->innerattno, false));
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
											  st->decode->attno[i], false),
							 get_attname(st->heapoid, st->decode->attno[i],
										 false));
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
	int			j;

	for (j = 0; j < st->nclause; j++)
	{
		if (st->clause[j].kind == LION_CLAUSE_RANGESRC &&
			!st->inor[j] && st->clause[j].attno == first->attno &&
			!lion_explain_dropped_all(st, j))
			break;
	}
	if (j == st->nclause)
		return;

	if (buf->len > 0)
		appendStringInfoString(buf, ", ");
	if (st->npart == 0)
		appendStringInfo(buf, "%s%s ", get_rel_name(first->idxoid),
						 lion_explain_clause_col(first));
	appendStringInfoChar(buf, '(');
	for (j = 0; j < st->nclause; j++)
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
							 lion_explain_clause_col(cl));
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
								 lion_explain_clause_col(cl));
			}
			appendStringInfoChar(buf, ' ');
		}
		lion_explain_or(st, o, ancestors, es, buf);
	}
}

/*
 * The "Lion Indexes" line: the index whose entries drive the count, the
 * FK-side join's key and the plan's items, each with what it answers.
 */
static void
lion_explain_indexes(LionCountScanState *st, List *ancestors,
					 ExplainState *es)
{
	StringInfoData buf;
	int			i;

	initStringInfo(&buf);

	if (st->hasgroupidx)
		lion_explain_group_index(st, ancestors, es, &buf);

	/*
	 * The FK-side join's key first (DESIGN.md §27): the fk index and the
	 * clause, whose value prints as the dimension column it is read from.
	 */
	if (st->joinclause >= 0)
	{
		LionClauseState *cl = &st->clause[st->joinclause];

		/* a partitioned fact table's fk index is one per partition (§16) */
		if (st->npart == 0)
			appendStringInfo(&buf, "%s%s ", get_rel_name(cl->idxoid),
							 lion_explain_clause_col(cl));
		appendStringInfoChar(&buf, '(');
		lion_explain_clause(st, cl, ancestors, es, &buf);
		appendStringInfoChar(&buf, ')');
	}

	/*
	 * The plan's items (a partition's own may leave some out).  One that
	 * every partition leaves out, because their bounds imply it, reads no
	 * index at all, and is printed on a line of its own below (DESIGN.md §16,
	 * "Clauses the partition bounds imply").
	 */
	for (i = 0; i < st->nplanitem; i++)
		lion_explain_item(st, &st->planitem[i], ancestors, es, &buf);

	ExplainPropertyText("Lion Indexes", buf.data, es);
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
	foreach(lc, st->implied)
	{
		if (buf.len > 0)
			appendStringInfoString(&buf, ", ");
		appendStringInfoString(&buf, strVal(lfirst(lc)));
	}
	for (i = 0; i < st->nplanitem; i++)
	{
		LionSourceItem *it = &st->planitem[i];
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
	if (buf.len > 0)
		ExplainPropertyText("Implied by Partition Bounds", buf.data, es);
	pfree(buf.data);

	/* ... and the arms of an OR a partition's bounds refute */
	initStringInfo(&buf);
	for (i = 0; i < st->nplanitem; i++)
	{
		LionSourceItem *it = &st->planitem[i];

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
	StringInfoData buf;
	int			i;

	/*
	 * A forward semi join over a non-unique key counts the fact rows of
	 * each DISTINCT key of the dimension, which it sorts to find them.
	 */
	/* ... which a semi or anti join path's name says (LionSemiJoin) */
	if (!st->joinouter &&
		(st->jointype == LION_JOIN_SEMI || st->joinunique))
		ExplainPropertyText("Join Type", "Semi", es);
	else if (!st->joinouter && st->jointype == LION_JOIN_ANTI)
		ExplainPropertyText("Join Type", "Anti", es);
	if (st->joinunique)
		ExplainPropertyText("Join Keys", "distinct, sorted", es);

	/*
	 * The keys are looked up a batch at a time, sorted into the fk
	 * index's order ("Lookups in key order").
	 */
	if (st->joinwalk)
		ExplainPropertyText("Join Key Lookups", "in index order", es);
	/*
	 * A semi or anti join path's rows are the outer side's (DESIGN.md
	 * §27, "The semi and anti join as a join path"), in the child's order
	 * where the path claims it.
	 */
	if (st->joinouter)
		ExplainPropertyText("Join Rows",
							st->jointype == LION_JOIN_ANTI ?
							(st->joinordered ?
							 "the outer rows without a match, in their order" :
							 "the outer rows without a match") :
							(st->joinordered ?
							 "the outer rows with a match, in their order" :
							 "the outer rows with a match"), es);
	else if (st->joinrows)
		ExplainPropertyText("Join Rows",
							st->jointype == LION_JOIN_ANTI ?
							"the dimension rows without a match" :
							st->joinunique ?
							(st->joincounts ?
							 "the distinct keys with a match, each with its count" :
							 "the distinct keys with a match") :
							st->joincounts ?
							"the dimension rows with a match, each with its count" :
							"the dimension rows with a match", es);
	if (st->joincollect)
		ExplainPropertyText("Fact Filters", "collected once", es);
	else if (es->analyze &&
			 st->joinswitches + st->joinworkerswitches > 0)
		ExplainPropertyText("Fact Filters", "probed, then collected", es);

	/*
	 * A fact column grouped by ("Grouped by a fact column"): each key is
	 * counted once per group of it - an entry of its index, or, in a
	 * partition whose bounds give it one value, that value.
	 */
	if (st->fgattno != 0)
	{
		int			bound = 0;

		ExplainPropertyText("Fact Group Key",
							get_attname(st->heapoid, st->fgattno, false),
							es);
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

	if (st->hasgroupidx && st->groupattno != 0 && st->hascoal)
	{
		/*
		 * GROUP BY coalesce(g, c) (DESIGN.md §10), printed as core prints the
		 * expression, with c deparsed like a clause value.
		 */
		List	   *context = set_deparse_context_plan(es->deparse_cxt,
													   st->css.ss.ps.plan,
													   ancestors);
		char	   *val = deparse_expression((Node *) st->coalconst, context,
											 false, false);

		initStringInfo(&buf);
		appendStringInfo(&buf, "COALESCE(%s, %s)",
						 get_attname(st->heapoid, st->groupattno, false), val);
		ExplainPropertyText("Group Key", buf.data, es);
		pfree(buf.data);
		pfree(val);
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
							 get_attname(st->heapoid, st->decode->attno[c],
										 false));
		ExplainPropertyText("Group Key", buf.data, es);
		ExplainPropertyText("Group Strategy", "Decoded", es);
		pfree(buf.data);
	}
	else if (st->hasgroupidx && st->groupattno != 0)
	{
		initStringInfo(&buf);
		appendStringInfoString(&buf,
							   get_attname(st->heapoid, st->groupattno, false));
		if (st->groupattno2 != 0)
			appendStringInfo(&buf, ", %s",
							 get_attname(st->heapoid, st->groupattno2, false));
		ExplainPropertyText("Group Key", buf.data, es);
		pfree(buf.data);
	}

	/* The column count(DISTINCT) counts (DESIGN.md §26). */
	if (st->distattno != 0)
		ExplainPropertyText("Distinct Key",
							get_attname(st->heapoid, st->distattno, false), es);

	/* the columns aggregates are taken over (DESIGN.md §37) */
	if (st->nwcol > 0)
	{
		int			c;

		initStringInfo(&buf);
		for (c = 0; c < st->nwcol; c++)
			appendStringInfo(&buf, "%s%s (%s)", (c > 0) ? ", " : "",
							 get_rel_name(st->wcol[c].idxoid),
							 get_attname(st->heapoid, st->wcol[c].attno,
										 false));
		ExplainPropertyText("Aggregates Over Keys", buf.data, es);
		pfree(buf.data);
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
						   st->dirpages + st->joinworkerdirpages, es);
}

/*
 * EXPLAIN ANALYZE's counters of ranges and sums: how the range bounding a
 * sum was evaluated, the summaries and walks it used, and the ranges taken
 * as sources.
 */
static void
lion_explain_range_counters(LionCountScanState *st, ExplainState *es)
{
	StringInfoData buf;

	/*
	 * How the range bounding a sum was evaluated (DESIGN.md §28, "The
	 * complement"): from the entries it selects ("inside"), from the ones
	 * it does not ("complement"), or from none because it selects them all
	 * ("full domain").  A partitioned table, or a rescan, may take more
	 * than one way; each is then printed with how often it was taken.
	 */
	if (st->hasrange && st->sumall)
	{
		static const char *const evalname[] = {"inside", "complement",
		"full domain"};
		int			kinds = 0;
		int			k;

		initStringInfo(&buf);
		for (k = 0; k < (int) lengthof(evalname); k++)
			if (st->rangeeval[k] > 0)
				kinds++;
		for (k = 0; k < (int) lengthof(evalname); k++)
		{
			if (st->rangeeval[k] == 0)
				continue;
			if (buf.len > 0)
				appendStringInfoString(&buf, ", ");
			appendStringInfoString(&buf, evalname[k]);
			if (kinds > 1)
				appendStringInfo(&buf, " %lld",
								 (long long) st->rangeeval[k]);
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
	if (st->summaries > 0)
		ExplainPropertyInteger("Summaries Summed", NULL, st->summaries, es);

	/*
	 * Of those sums, the walks that were probed at the other sources'
	 * rows rather than counted a set at a time (DESIGN.md §32, "Summed
	 * ranges: dense and probed").  Only when there were any.
	 */
	if (st->rangeprobed > 0)
		ExplainPropertyInteger("Range Walks Probed", NULL, st->rangeprobed,
							   es);

	/*
	 * The ranges taken as sources (DESIGN.md §32): collected into memory,
	 * or - too large for it - summed over their walk at every count they
	 * are part of.  Again only when there were any.
	 */
	if (st->rangesrc_collected > 0)
		ExplainPropertyInteger("Range Sources Collected", NULL,
							   st->rangesrc_collected, es);
	if (st->rangesrc_walked > 0)
		ExplainPropertyInteger("Range Sources Walked", NULL,
							   st->rangesrc_walked, es);
	/* ... and of the collected, those an OR's leaf spilled to a file */
	if (st->rangesrc_spilled > 0)
		ExplainPropertyInteger("Range Sources Spilled", NULL,
							   st->rangesrc_spilled, es);
}

/*
 * EXPLAIN ANALYZE's counters of a GROUP BY's WHERE sets, batches and key
 * ranges, of an IN list's batches and of a count(DISTINCT)'s tests.
 */
static void
lion_explain_group_counters(LionCountScanState *st, ExplainState *es)
{
	/*
	 * The WHERE items of a GROUP BY collected into one set that the groups
	 * were counted against (DESIGN.md §10, "The WHERE sets, collected
	 * once"): once per relation - per partition, and again on a rescan -
	 * that did; and of those, the ones past a hash table's memory that
	 * went to a temporary file.  Only when there were any.
	 */
	if (st->wherecollected + st->workerwherecollected > 0)
		ExplainPropertyInteger("WHERE Sets Collected", NULL,
							   st->wherecollected +
							   st->workerwherecollected, es);
	if (st->wherespilled + st->workerwherespilled > 0)
		ExplainPropertyInteger("WHERE Sets Spilled", NULL,
							   st->wherespilled + st->workerwherespilled,
							   es);

	/*
	 * ... and the batches of groups counted together against it, in one
	 * walk of container keys each (DESIGN.md §10, "The groups of a walk,
	 * counted together").  Only when there were any.
	 */
	if (st->groupbatches + st->workergroupbatches > 0)
	{
		ExplainPropertyInteger("Group Batches", NULL,
							   st->groupbatches + st->workergroupbatches,
							   es);
		ExplainPropertyInteger("Groups Counted in Batches", NULL,
							   st->groupsbatched + st->workergroupsbatched,
							   es);
	}

	/*
	 * The decoded walk's passes (DESIGN.md §34): the container keys it
	 * decoded, the rows it counted under their combinations - from the map
	 * or the heap - and those the heap was asked about, the partial rows it
	 * handed up, and how often a pass's tally went to its temporary file.
	 */
	if (st->decode != NULL && st->decode->passes > 0)
	{
		LionDecodeRun *dr = st->decode;

		ExplainPropertyInteger("Decoded Passes", NULL, dr->passes, es);
		ExplainPropertyInteger("Decoded Keys", NULL, dr->stats.keys, es);
		ExplainPropertyInteger("Decoded Rows", NULL, dr->stats.rows, es);
		ExplainPropertyInteger("Decoded Rows Rechecked", NULL,
							   dr->stats.rechecked, es);
		ExplainPropertyInteger("Partial Rows", NULL, dr->rowsup, es);
		if (dr->stats.spills > 0)
			ExplainPropertyInteger("Tally Spills", NULL, dr->stats.spills, es);
	}

	/*
	 * The ranges of container keys a parallel GROUP BY's participants
	 * counted (DESIGN.md §10, "A GROUP BY in parallel"), the leader's and
	 * its workers' - each range a WHERE collected and a walk of the
	 * entries - which is every range of the cut, whichever participants
	 * took them.
	 */
	if (st->granged)
		ExplainPropertyInteger("Key Ranges", NULL,
							   st->granges + st->workerranges, es);

	/*
	 * The batches an IN list too long to locate at once was counted in
	 * (DESIGN.md §15, "A list too long to locate at once").  Only when
	 * there were any.
	 */
	if (st->listbatches > 0)
		ExplainPropertyInteger("List Batches", NULL, st->listbatches, es);

	/*
	 * The existence (or count) tests a count(DISTINCT k) made: one per
	 * entry of k without a GROUP BY, one per group and one per (g, k) pair
	 * with one (DESIGN.md §26).
	 */
	if (st->distattno != 0)
		ExplainPropertyInteger("Distinct Keys Tested", NULL, st->disttests,
							   es);

	/*
	 * The aggregates over keys (DESIGN.md §37): the entries they took, and
	 * how many walks read the entries' own counts and how many counted them.
	 */
	if (st->nwcol > 0)
	{
		ExplainPropertyInteger("Keys Aggregated", NULL, st->wentries, es);
		ExplainPropertyInteger("Key Walks From Entry Counts", NULL, st->wfast,
							   es);
		if (st->wslow > 0)
			ExplainPropertyInteger("Key Walks Counted", NULL, st->wslow, es);
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
	/*
	 * The rows of the collected fact filters, or -1 when the plan
	 * collected them and the run could not (a standby) and every
	 * count read the filters instead - in a parallel plan, the
	 * largest copy any participant made, which is the one they all
	 * read when they share it.
	 */
	if (st->joincollect || switched)
		ExplainPropertyInteger("Fact Filter Rows Collected", NULL,
							   Max(st->joinfilterrows,
								   st->joinworkerfilterrows), es);

	/*
	 * Copies past a hash join's memory, which went to a temporary
	 * file instead: one per participant and run - or one a run, a
	 * copy shared by the participants (any of whose chunks went to
	 * a file).  Only when there were any.
	 */
	if (st->joinspilled + st->joinworkerspilled > 0)
		ExplainPropertyInteger("Fact Filter Copies Spilled", NULL,
							   st->joinspilled + st->joinworkerspilled,
							   es);

	/*
	 * The copies collected once for all the participants of a
	 * parallel plan - one a run - and the chunks the participants
	 * collected of them, summed.  Only when there were any.
	 */
	if (st->joincopies + st->joinworkercopies > 0)
	{
		ExplainPropertyInteger("Fact Filter Copies Shared", NULL,
							   st->joincopies + st->joinworkercopies,
							   es);
		ExplainPropertyInteger("Fact Filter Copy Chunks", NULL,
							   st->joincopychunks +
							   st->joinworkercopychunks, es);
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
							   st->joinswitchkeys +
							   st->joinworkerswitchkeys, es);
	}

	/*
	 * What the counts read of the copy: its containers, the binary
	 * searches that found them, and of those the ones read back from
	 * a spilled copy's temporary file - one read each, never the
	 * copy again.
	 */
	if (st->joincollect || switched)
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
	int64		switches = st->joinswitches + st->joinworkerswitches;
	bool		switched = (switches > 0);
	int			i;

	/*
	 * A forward semi join's keys: the child rows with one, sorted - by
	 * every participant of a parallel plan, so what one of them sorted
	 * in each run, summed over the runs - and how the sort went here,
	 * if it ran here.  "Looked Up" below is then the DISTINCT keys,
	 * each by one participant.
	 */
	if (st->joinunique)
	{
		ExplainPropertyInteger("Join Keys Sorted", NULL,
							   Max(st->joinsorted,
								   st->joinworkersorted), es);
		if (st->joinhavesortstats)
		{
			ExplainPropertyText("Join Key Sort Method",
								tuplesort_method_name(st->joinsortstats.sortMethod),
								es);
			ExplainPropertyInteger("Join Key Sort Space Used", "kB",
								   st->joinsortstats.spaceUsed, es);
			ExplainPropertyText("Join Key Sort Space Type",
								tuplesort_space_type_name(st->joinsortstats.spaceType),
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
						   st->joinchildrows + st->joinworkerchildrows,
						   es);
	ExplainPropertyInteger("Join Keys Looked Up", NULL,
						   st->joinlookups + st->joinworkerlookups, es);
	ExplainPropertyInteger("Join Keys Without Entry", NULL,
						   st->joinmissing + st->joinworkermissing, es);

	/* ... and counted in each group of a fact column grouped by */
	if (st->fgattno != 0)
		ExplainPropertyInteger("Fact Group Counts", NULL,
							   st->fggroupcounts +
							   st->fgworkergroupcounts, es);

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
						   st->joinposting + st->joinworkerposting, es);
	ExplainPropertyInteger("Visibility Map Checks", NULL,
						   tot->vm_checks, es);
	ExplainPropertyInteger("Visibility Map Pages Pinned", NULL,
						   tot->vm_pins, es);

	/*
	 * The batches the keys were looked up in, in the fk index's order:
	 * one per work_mem of the child's rows, per participant and run.
	 */
	if (st->joinwalk)
		ExplainPropertyInteger("Join Key Batches", NULL,
							   st->joinbatches + st->joinworkerbatches,
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
			instr_time	t = st->jointime[i];

			if (i == LION_JT_COLLECT && !st->joincollect && !switched)
				continue;
			INSTR_TIME_ADD(t, st->joinworkertime[i]);
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
	 * The leader's own counters, plus - for a parallel FK-side join
	 * (DESIGN.md §27, "Parallel") - what its workers added up
	 * (lion_shutdown_custom_scan()).
	 */
	LionCountStats tot = st->stats;

	lion_count_stats_add(&tot, &st->joinworkerstats);

	lion_explain_scan_counters(st, &tot, es);
	lion_explain_range_counters(st, es);
	lion_explain_group_counters(st, es);

	/*
	 * The FK-side join (DESIGN.md §27): the dimension rows whose key was
	 * looked up (a NULL key joins nothing and is not), and how many of
	 * those keys have no entry in the fk index at all.
	 */
	if (st->joinclause >= 0)
		lion_explain_join_counters(st, &tot, es);
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
	if (st->joinclause >= 0)
		lion_explain_join(st, es);

	lion_explain_group_keys(st, ancestors, es);

	if (es->analyze)
		lion_explain_analyze(st, es);
}
