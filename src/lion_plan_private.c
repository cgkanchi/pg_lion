/*-------------------------------------------------------------------------
 *
 * lion_plan_private.c
 *		A LionCount plan's custom_private: the positional list decoded into
 *		a LionCountPriv and encoded back from one, and the rules between its
 *		members checked.
 *
 * Part of the LionCount custom scan: lion_customscan.h describes the node
 * and declares what its files share, and lion_plan_private.h the struct.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "lion_plan_private.h"

/* Every LION_FLAG_* and LION_JOINFLAG_* bit this build knows. */
#define LION_FLAG_ALL \
	(LION_FLAG_SINGLEGROUP | LION_FLAG_SUMALL | LION_FLAG_GROUPIDX | \
	 LION_FLAG_RANGE | LION_FLAG_DECODE)
#define LION_JOINFLAG_ALL \
	(LION_JOINFLAG_COLLECT | LION_JOINFLAG_ROWS | LION_JOINFLAG_UNIQUE | \
	 LION_JOINFLAG_WALK | LION_JOINFLAG_SUM | LION_JOINFLAG_COUNTS | \
	 LION_JOINFLAG_OUTER | LION_JOINFLAG_ORDERED)

/*
 * Each member's name, for the messages, and the node it is when it is not
 * empty, in LION_PRIV_* order.
 */
static const char *const lion_priv_name[LION_PRIV_NMEMBERS] = {
	"VERSION", "OIDS", "INTS", "CONSTS", "CLAUSEKINDS", "PARTS", "CLAUSEOPS",
	"ORS", "HAVING", "DISTINCT", "JOIN", "EXECUTE", "COALESCE", "IMPLIED",
	"FACTGROUP", "GROUPN", "ALLROWS", "TOPK", "WAGG", "TLKINDS"
};

static const NodeTag lion_priv_tag[LION_PRIV_NMEMBERS] = {
	T_IntList,					/* VERSION */
	T_OidList,					/* OIDS */
	T_IntList,					/* INTS */
	T_List,						/* CONSTS */
	T_IntList,					/* CLAUSEKINDS */
	T_List,						/* PARTS */
	T_OidList,					/* CLAUSEOPS */
	T_List,						/* ORS */
	T_List,						/* HAVING */
	T_IntList,					/* DISTINCT */
	T_IntList,					/* JOIN */
	T_List,						/* EXECUTE */
	T_List,						/* COALESCE */
	T_List,						/* IMPLIED */
	T_List,						/* FACTGROUP */
	T_List,						/* GROUPN */
	T_IntList,					/* ALLROWS */
	T_IntList,					/* TOPK */
	T_List,						/* WAGG */
	T_IntList					/* TLKINDS */
};

/* A member's length has to be exactly `expected`. */
static void
lion_priv_expect_length(List *member, int n, int expected)
{
	if (list_length(member) != expected)
		elog(ERROR, "LionCount: custom_private member %s has length %d, not %d",
			 lion_priv_name[n], list_length(member), expected);
}

/* Is `node` empty or of node type `tag`? */
static inline bool
lion_priv_is(void *node, NodeTag tag)
{
	return node == NULL || nodeTag(node) == tag;
}

/*
 * The relation and its clauses: OIDS and INTS, whose heads are the table's
 * and whose tails are one per clause, and the clauses' kinds and operators.
 * The number of clauses is CLAUSEKINDS', and every other per-clause list
 * has to agree with it.  The Oids are not checked for validity: a
 * partitioned table's are all InvalidOid (its indexes are its partitions'),
 * and so is the inner group index of the decoded walk (DESIGN.md §34).
 */
static void
lion_priv_decode_relation(List *priv, LionCountPriv *out)
{
	List	   *oids = (List *) list_nth(priv, LION_PRIV_OIDS);
	List	   *ints = (List *) list_nth(priv, LION_PRIV_INTS);
	List	   *ckinds = (List *) list_nth(priv, LION_PRIV_CLAUSEKINDS);
	List	   *ops = (List *) list_nth(priv, LION_PRIV_CLAUSEOPS);
	int			i;

	out->nclause = list_length(ckinds);
	lion_priv_expect_length(oids, LION_PRIV_OIDS, 3 + out->nclause);
	lion_priv_expect_length(ints, LION_PRIV_INTS, 4 + out->nclause);
	lion_priv_expect_length(ops, LION_PRIV_CLAUSEOPS, out->nclause);

	out->heapoid = linitial_oid(oids);
	out->groupidxoid = lsecond_oid(oids);
	out->groupidxoid2 = lthird_oid(oids);
	out->scanrelid = (Index) linitial_int(ints);
	out->groupattno = (AttrNumber) lsecond_int(ints);
	out->groupattno2 = (AttrNumber) lthird_int(ints);
	out->flags = lfourth_int(ints);
	if ((out->flags & ~LION_FLAG_ALL) != 0)
		elog(ERROR, "LionCount: unknown flags 0x%x", out->flags);

	out->clause = (LionCountPrivClause *)
		palloc0(sizeof(LionCountPrivClause) * Max(out->nclause, 1));
	for (i = 0; i < out->nclause; i++)
	{
		LionCountPrivClause *cl = &out->clause[i];

		cl->kind = list_nth_int(ckinds, i);
		cl->idxoid = list_nth_oid(oids, 3 + i);
		cl->attno = (AttrNumber) list_nth_int(ints, 4 + i);
		cl->opno = list_nth_oid(ops, i);
		if (cl->kind < LION_CLAUSE_EQ || cl->kind > LION_CLAUSE_NE)
			elog(ERROR, "LionCount: unknown clause kind %d", cl->kind);
	}
}

/*
 * One target per live leaf partition (DESIGN.md §16): its heap, its two
 * group indexes and one index per clause, InvalidOid wherever it has none.
 */
static void
lion_priv_decode_parts(List *priv, LionCountPriv *out)
{
	List	   *parts = (List *) list_nth(priv, LION_PRIV_PARTS);
	int			i;
	int			j;

	out->npart = list_length(parts);
	if (out->npart == 0)
		return;
	out->part = (LionCountPrivPart *)
		palloc0(sizeof(LionCountPrivPart) * out->npart);
	for (i = 0; i < out->npart; i++)
	{
		List	   *one = (List *) list_nth(parts, i);
		LionCountPrivPart *part = &out->part[i];

		if (one == NIL || !IsA(one, OidList) ||
			list_length(one) != 3 + out->nclause)
			elog(ERROR, "LionCount: partition %d has %d Oids for %d clauses",
				 i, list_length(one), out->nclause);
		part->heapoid = linitial_oid(one);
		part->groupidxoid = lsecond_oid(one);
		part->groupidxoid2 = lthird_oid(one);
		part->clauseidxoid = (Oid *) palloc0(sizeof(Oid) *
											 Max(out->nclause, 1));
		for (j = 0; j < out->nclause; j++)
			part->clauseidxoid[j] = list_nth_oid(one, 3 + j);
	}
}

/*
 * The OR restrictions (DESIGN.md §19): {first, narms, armlen...} each, an
 * arm of at least one leaf, and every leaf a clause of the plan.
 */
static void
lion_priv_decode_ors(List *priv, LionCountPriv *out)
{
	List	   *ors = (List *) list_nth(priv, LION_PRIV_ORS);
	int			i;
	int			k;

	out->nor = list_length(ors);
	if (out->nor == 0)
		return;
	out->ors = (LionOrState *) palloc0(sizeof(LionOrState) * out->nor);
	for (i = 0; i < out->nor; i++)
	{
		List	   *one = (List *) list_nth(ors, i);
		LionOrState *o = &out->ors[i];

		if (one == NIL || !IsA(one, IntList) || list_length(one) < 2 ||
			list_length(one) != 2 + lsecond_int(one))
			elog(ERROR, "LionCount: malformed OR structure");
		o->first = linitial_int(one);
		o->narms = lsecond_int(one);
		o->armlen = (int *) palloc0(sizeof(int) * Max(o->narms, 1));
		o->nleaves = 0;
		for (k = 0; k < o->narms; k++)
		{
			o->armlen[k] = list_nth_int(one, 2 + k);
			if (o->armlen[k] < 1)
				elog(ERROR, "LionCount: malformed OR structure");
			o->nleaves += o->armlen[k];
		}
		if (o->narms < 1 || o->first < 0 ||
			o->first + o->nleaves > out->nclause)
			elog(ERROR, "LionCount: malformed OR structure");
	}
}

/*
 * The FK-side join (DESIGN.md §27): the key's clause, the kind of join, its
 * flags, the sort operator and collation of distinct keys, and - added at
 * plan time, once the child plan exists - the key's column of the child.
 */
static void
lion_priv_decode_join(List *priv, LionPrivStage stage, LionCountPriv *out)
{
	List	   *join = (List *) list_nth(priv, LION_PRIV_JOIN);

	if (join == NIL)
		return;
	if (list_length(join) != (stage == LION_PRIV_STAGE_PLAN ? 6 : 5))
		elog(ERROR, "LionCount: malformed join");
	out->hasjoin = true;
	out->join.clause = linitial_int(join);
	out->join.type = lsecond_int(join);
	out->join.flags = lthird_int(join);
	out->join.sortop = (Oid) list_nth_int(join, 3);
	out->join.sortcoll = (Oid) list_nth_int(join, 4);
	out->join.keyresno = (stage == LION_PRIV_STAGE_PLAN) ?
		(AttrNumber) list_nth_int(join, 5) : 0;
	if (out->join.clause < 0 || out->join.clause >= out->nclause ||
		(out->join.type != LION_JOIN_INNER &&
		 out->join.type != LION_JOIN_SEMI &&
		 out->join.type != LION_JOIN_ANTI) ||
		(out->join.flags & ~LION_JOINFLAG_ALL) != 0 ||
		(stage == LION_PRIV_STAGE_PLAN && out->join.keyresno <= 0))
		elog(ERROR, "LionCount: malformed join");
}

/*
 * The functions the node replaces, for the EXECUTE checks (DESIGN.md §9,
 * "Privileges"): three lists, each of them empty or an OidList.
 */
static void
lion_priv_decode_execute(List *priv, LionCountPriv *out)
{
	List	   *exec = (List *) list_nth(priv, LION_PRIV_EXECUTE);

	if (list_length(exec) != 3 ||
		!lion_priv_is(linitial(exec), T_OidList) ||
		!lion_priv_is(lsecond(exec), T_OidList) ||
		!lion_priv_is(lthird(exec), T_OidList))
		elog(ERROR, "LionCount: malformed EXECUTE list");
	out->exec_aggs = (List *) linitial(exec);
	out->exec_funcs = (List *) lsecond(exec);
	out->exec_groupfuncs = (List *) lthird(exec);
}

/* GROUP BY coalesce(g, c) (DESIGN.md §10): c and its equality */
static void
lion_priv_decode_coalesce(List *priv, LionCountPriv *out)
{
	List	   *coal = (List *) list_nth(priv, LION_PRIV_COALESCE);
	List	   *ops;

	if (coal == NIL)
		return;
	if (list_length(coal) != 2 || linitial(coal) == NULL ||
		!IsA(linitial(coal), Const) ||
		lsecond(coal) == NULL || !IsA(lsecond(coal), OidList) ||
		list_length((List *) lsecond(coal)) != 2)
		elog(ERROR, "LionCount: malformed coalesce group");
	out->coalconst = (Const *) linitial(coal);
	ops = (List *) lsecond(coal);
	out->coaleqop = linitial_oid(ops);
	out->coalcoll = lsecond_oid(ops);
	if (out->coalconst->constisnull || !OidIsValid(out->coaleqop))
		elog(ERROR, "LionCount: malformed coalesce group");
}

/*
 * An FK-side join grouped by a fact column (DESIGN.md §27, "Grouped by a fact
 * column"): the column, and per relation counted - the table, or each
 * partition in PARTS' order - the index of its groups, or InvalidOid and the
 * value the partition's bounds give it.  A plain table always has the index.
 */
static void
lion_priv_decode_fact_group(List *priv, LionCountPriv *out)
{
	List	   *fg = (List *) list_nth(priv, LION_PRIV_FACTGROUP);
	List	   *fgoids;
	List	   *fgconsts;
	int			nrel = Max(out->npart, 1);
	int			i;

	if (fg == NIL)
		return;
	if (list_length(fg) != 3 ||
		linitial(fg) == NULL || !IsA(linitial(fg), IntList) ||
		list_length((List *) linitial(fg)) != 1 ||
		lsecond(fg) == NULL || !IsA(lsecond(fg), OidList) ||
		lthird(fg) == NULL || !IsA(lthird(fg), List))
		elog(ERROR, "LionCount: malformed fact group");
	out->fgattno = (AttrNumber) linitial_int((List *) linitial(fg));
	fgoids = (List *) lsecond(fg);
	fgconsts = (List *) lthird(fg);
	if (out->fgattno <= 0 ||
		list_length(fgoids) != nrel || list_length(fgconsts) != nrel)
		elog(ERROR, "LionCount: malformed fact group");

	out->fgidxoid = (Oid *) palloc0(sizeof(Oid) * nrel);
	out->fgconst = (Const **) palloc0(sizeof(Const *) * nrel);
	for (i = 0; i < nrel; i++)
	{
		out->fgidxoid[i] = list_nth_oid(fgoids, i);
		out->fgconst[i] = (Const *) list_nth(fgconsts, i);
		if (out->fgconst[i] == NULL || !IsA(out->fgconst[i], Const) ||
			(out->npart == 0 && !OidIsValid(out->fgidxoid[i])))
			elog(ERROR, "LionCount: malformed fact group");
	}
}

/*
 * The columns of the decoded walk (DESIGN.md §34): an IntList of their
 * attnums and an OidList of their indexes, two to LION_MAX_GROUPCOLS.
 */
static void
lion_priv_decode_groupn(List *priv, LionCountPriv *out)
{
	List	   *gn = (List *) list_nth(priv, LION_PRIV_GROUPN);
	List	   *attnos;
	List	   *oids;
	int			c;

	if (gn == NIL)
		return;
	if (list_length(gn) != 2 ||
		linitial(gn) == NULL || !IsA(linitial(gn), IntList) ||
		lsecond(gn) == NULL || !IsA(lsecond(gn), OidList))
		elog(ERROR, "LionCount: malformed decoded walk");
	attnos = (List *) linitial(gn);
	oids = (List *) lsecond(gn);
	if (list_length(attnos) < 2 || list_length(attnos) > LION_MAX_GROUPCOLS ||
		list_length(oids) != list_length(attnos))
		elog(ERROR, "LionCount: malformed decoded walk");
	out->ngroupn = list_length(attnos);
	for (c = 0; c < out->ngroupn; c++)
	{
		out->groupn_attno[c] = (AttrNumber) list_nth_int(attnos, c);
		out->groupn_idx[c] = list_nth_oid(oids, c);
		if (out->groupn_attno[c] <= 0 || !OidIsValid(out->groupn_idx[c]))
			elog(ERROR, "LionCount: malformed decoded walk");
	}
}

/*
 * The aggregates over lion columns' entries (DESIGN.md §37): three lists of
 * one element per column, and at plan time a fourth of one IntList per
 * aggregate - {kind, column, argument width, aggregate, collation} - which
 * is empty when the target list prints only the columns' keys.
 */
static void
lion_priv_decode_wagg(List *priv, LionPrivStage stage, LionCountPriv *out)
{
	List	   *w = (List *) list_nth(priv, LION_PRIV_WAGG);
	List	   *attnos;
	List	   *oids;
	List	   *cols;
	List	   *specs;
	int			i;

	if (w == NIL)
		return;
	if (list_length(w) != (stage == LION_PRIV_STAGE_PLAN ? 4 : 3))
		elog(ERROR, "LionCount: malformed aggregates over keys");
	attnos = (List *) linitial(w);
	oids = (List *) lsecond(w);
	cols = (List *) lthird(w);
	if (attnos == NIL || !IsA(attnos, IntList) ||
		!lion_priv_is(oids, T_OidList) || !lion_priv_is(cols, T_IntList) ||
		list_length(oids) != list_length(attnos) ||
		list_length(cols) != list_length(attnos))
		elog(ERROR, "LionCount: malformed aggregates over keys");

	out->nwcol = list_length(attnos);
	out->wcol = (LionCountPrivWCol *)
		palloc0(sizeof(LionCountPrivWCol) * out->nwcol);
	for (i = 0; i < out->nwcol; i++)
	{
		out->wcol[i].attno = (AttrNumber) list_nth_int(attnos, i);
		out->wcol[i].idxoid = list_nth_oid(oids, i);
		out->wcol[i].idxcol = (AttrNumber) list_nth_int(cols, i);
	}

	if (stage != LION_PRIV_STAGE_PLAN)
		return;
	specs = (List *) lfourth(w);
	if (!lion_priv_is(specs, T_List))
		elog(ERROR, "LionCount: malformed aggregates over keys");
	out->nwagg = list_length(specs);
	out->wagg = (LionCountPrivWAgg *)
		palloc0(sizeof(LionCountPrivWAgg) * Max(out->nwagg, 1));
	for (i = 0; i < out->nwagg; i++)
	{
		List	   *spec = (List *) list_nth(specs, i);
		LionCountPrivWAgg *a = &out->wagg[i];

		if (spec == NIL || !IsA(spec, IntList) || list_length(spec) != 5)
			elog(ERROR, "LionCount: malformed aggregates over keys");
		a->kind = list_nth_int(spec, 0);
		a->col = list_nth_int(spec, 1);
		a->argwidth = list_nth_int(spec, 2);
		a->aggfnoid = (Oid) list_nth_int(spec, 3);
		a->collation = (Oid) list_nth_int(spec, 4);
		if (a->col < 0 || a->col >= out->nwcol ||
			a->kind <= LION_WAGG_NONE || a->kind > LION_WAGG_EXTREME)
			elog(ERROR, "LionCount: malformed aggregates over keys");
	}
}

/*
 * The kinds of custom_scan_tlist's columns (LION_TL_*), each one of the
 * kinds there are: a child's column, a group key or count, a clause's
 * stored key, or an aggregate over keys or its key.  Which of them the plan
 * may have is lion_count_priv_check()'s.
 */
static void
lion_priv_decode_tlkinds(List *priv, LionCountPriv *out)
{
	List	   *kinds = (List *) list_nth(priv, LION_PRIV_TLKINDS);
	int			i;

	out->ntl = list_length(kinds);
	out->tlkind = (int *) palloc(sizeof(int) * Max(out->ntl, 1));
	for (i = 0; i < out->ntl; i++)
	{
		int			kind = list_nth_int(kinds, i);

		if (!LION_TL_IS_CHILDCOL(kind) &&
			!(kind >= 0 && kind < LION_TL_WHEREKEY + out->nclause) &&
			!LION_TL_IS_WAGG(kind) && !LION_TL_IS_WKEY(kind))
			elog(ERROR, "LionCount: unknown target list kind %d", kind);
		out->tlkind[i] = kind;
	}
}

/*
 * Take custom_private apart into *out, checking as it goes that it is the
 * list this build writes at `stage` (lion_plan_private.h): the shape marker
 * and the number of members before anything else, then every member's node
 * type, and then each member's own shape - its length against the number of
 * clauses or of partitions, and its kinds against the kinds there are.  The
 * rules between members are lion_count_priv_check()'s.
 */
void
lion_count_priv_decode(List *priv, LionPrivStage stage, LionCountPriv *out)
{
	int			nmembers = (stage == LION_PRIV_STAGE_PLAN) ?
		LION_PRIV_NMEMBERS : LION_PRIV_NMEMBERS - 1;
	List	   *shape;
	List	   *m;
	ListCell   *lc;
	int			nlen;
	int			n;

	memset(out, 0, sizeof(LionCountPriv));

	/*
	 * custom_private is read positionally, so check that it is the list this
	 * build writes before reading a single offset of it.  A mismatch means
	 * the planner half and the executor half have drifted apart (or a plan
	 * from another build has been handed to us); saying so is far better
	 * than decoding Oids out of the wrong member.  The PATH's list is one
	 * short of the number its marker gives: the target-list kinds are added
	 * at plan time.
	 */
	nlen = (priv != NIL && IsA(priv, List)) ? list_length(priv) : 0;
	shape = (nlen == nmembers) ?
		(List *) list_nth(priv, LION_PRIV_VERSION) : NIL;
	if (shape == NIL || !IsA(shape, IntList) || list_length(shape) != 2 ||
		linitial_int(shape) != LION_PRIV_MAGIC ||
		lsecond_int(shape) != LION_PRIV_NMEMBERS)
		elog(ERROR, "LionCount: unrecognized custom_private shape (%d members)",
			 nlen);

	for (n = 0; n < nmembers; n++)
	{
		if (!lion_priv_is(list_nth(priv, n), lion_priv_tag[n]))
			elog(ERROR, "LionCount: malformed %s member of custom_private",
				 lion_priv_name[n]);
	}

	lion_priv_decode_relation(priv, out);

	/*
	 * The clause values and the HAVING are the PATH's: the plan has them in
	 * custom_exprs and plan.qual (lion_plan_custom_path()).
	 */
	m = (List *) list_nth(priv, LION_PRIV_CONSTS);
	if (stage == LION_PRIV_STAGE_PLAN)
		lion_priv_expect_length(m, LION_PRIV_CONSTS, 0);
	else
	{
		lion_priv_expect_length(m, LION_PRIV_CONSTS, out->nclause);
		out->consts = m;
	}
	m = (List *) list_nth(priv, LION_PRIV_HAVING);
	if (stage == LION_PRIV_STAGE_PLAN)
		lion_priv_expect_length(m, LION_PRIV_HAVING, 0);
	else
		out->having = m;

	lion_priv_decode_parts(priv, out);
	lion_priv_decode_ors(priv, out);

	/* count(DISTINCT k) (DESIGN.md §26): k */
	m = (List *) list_nth(priv, LION_PRIV_DISTINCT);
	if (m != NIL)
	{
		lion_priv_expect_length(m, LION_PRIV_DISTINCT, 1);
		out->distattno = (AttrNumber) linitial_int(m);
		if (out->distattno <= 0)
			elog(ERROR, "LionCount: malformed DISTINCT member of custom_private");
	}

	lion_priv_decode_join(priv, stage, out);
	lion_priv_decode_execute(priv, out);
	lion_priv_decode_coalesce(priv, out);

	/* the clauses every partition's bounds imply, for EXPLAIN (§16) */
	out->implied = (List *) list_nth(priv, LION_PRIV_IMPLIED);
	foreach(lc, out->implied)
	{
		if (lfirst(lc) == NULL || !IsA(lfirst(lc), String))
			elog(ERROR, "LionCount: malformed IMPLIED member of custom_private");
	}

	lion_priv_decode_fact_group(priv, out);
	lion_priv_decode_groupn(priv, out);

	/* the column a sum over every row drives by (DESIGN.md §35) */
	m = (List *) list_nth(priv, LION_PRIV_ALLROWS);
	if (m != NIL)
	{
		lion_priv_expect_length(m, LION_PRIV_ALLROWS, 1);
		out->allattno = (AttrNumber) linitial_int(m);
		if (out->allattno <= 0)
			elog(ERROR, "LionCount: malformed ALLROWS member of custom_private");
	}

	/* the top k by count (DESIGN.md §36): k, the candidates, the tie rule */
	m = (List *) list_nth(priv, LION_PRIV_TOPK);
	if (m != NIL)
	{
		if (list_length(m) != 3 || linitial_int(m) <= 0 ||
			lsecond_int(m) < linitial_int(m))
			elog(ERROR, "LionCount: a top k of another shape");
		out->topkn = linitial_int(m);
		out->topkcand = lsecond_int(m);
		out->topkstrict = (lthird_int(m) != 0);
	}

	lion_priv_decode_wagg(priv, stage, out);
	if (stage == LION_PRIV_STAGE_PLAN)
		lion_priv_decode_tlkinds(priv, out);
}

/*
 * The members lion_count_priv_decode() reads, written back from *p in the
 * same positions and as the same nodes: a decoded list encoded at the stage
 * it was decoded at is equal() to it.  A member the struct leaves empty is
 * NIL.  Lists and nodes the struct points to are shared, not copied.
 */
static List *
lion_priv_encode(const LionCountPriv *p, LionPrivStage stage)
{
	List	   *priv;
	List	   *oids;
	List	   *ints;
	List	   *ckinds = NIL;
	List	   *ops = NIL;
	List	   *parts = NIL;
	List	   *ors = NIL;
	List	   *m;
	int			i;
	int			j;

	Assert(stage == LION_PRIV_STAGE_PLAN ||
		   (p->join.keyresno == 0 && p->nwagg == 0 && p->ntl == 0));
	Assert(stage == LION_PRIV_STAGE_PATH ||
		   (p->consts == NIL && p->having == NIL));

	oids = list_make3_oid(p->heapoid, p->groupidxoid, p->groupidxoid2);
	ints = list_make4_int((int) p->scanrelid, (int) p->groupattno,
						  (int) p->groupattno2, p->flags);
	for (i = 0; i < p->nclause; i++)
	{
		oids = lappend_oid(oids, p->clause[i].idxoid);
		ints = lappend_int(ints, (int) p->clause[i].attno);
		ckinds = lappend_int(ckinds, p->clause[i].kind);
		ops = lappend_oid(ops, p->clause[i].opno);
	}
	for (i = 0; i < p->npart; i++)
	{
		const LionCountPrivPart *part = &p->part[i];
		List	   *one = list_make3_oid(part->heapoid, part->groupidxoid,
										 part->groupidxoid2);

		for (j = 0; j < p->nclause; j++)
			one = lappend_oid(one, part->clauseidxoid[j]);
		parts = lappend(parts, one);
	}
	for (i = 0; i < p->nor; i++)
	{
		const LionOrState *o = &p->ors[i];
		List	   *one = list_make2_int(o->first, o->narms);

		for (j = 0; j < o->narms; j++)
			one = lappend_int(one, o->armlen[j]);
		ors = lappend(ors, one);
	}

	priv = list_make1(list_make2_int(LION_PRIV_MAGIC, LION_PRIV_NMEMBERS));
	priv = lappend(priv, oids);
	priv = lappend(priv, ints);
	priv = lappend(priv, p->consts);
	priv = lappend(priv, ckinds);
	priv = lappend(priv, parts);
	priv = lappend(priv, ops);
	priv = lappend(priv, ors);
	priv = lappend(priv, p->having);
	priv = lappend(priv, (p->distattno != 0) ?
				   list_make1_int((int) p->distattno) : NIL);

	/* the operator and collation are OIDs in an IntList, cast both ways */
	m = NIL;
	if (p->hasjoin)
	{
		m = list_make5_int(p->join.clause, p->join.type, p->join.flags,
						   (int) p->join.sortop, (int) p->join.sortcoll);
		if (stage == LION_PRIV_STAGE_PLAN)
			m = lappend_int(m, (int) p->join.keyresno);
	}
	priv = lappend(priv, m);

	priv = lappend(priv, list_make3(p->exec_aggs, p->exec_funcs,
									p->exec_groupfuncs));
	priv = lappend(priv, (p->coalconst != NULL) ?
				   list_make2(p->coalconst,
							  list_make2_oid(p->coaleqop, p->coalcoll)) :
				   NIL);
	priv = lappend(priv, p->implied);

	m = NIL;
	if (p->fgattno != 0)
	{
		List	   *fgoids = NIL;
		List	   *fgconsts = NIL;

		for (i = 0; i < Max(p->npart, 1); i++)
		{
			fgoids = lappend_oid(fgoids, p->fgidxoid[i]);
			fgconsts = lappend(fgconsts, p->fgconst[i]);
		}
		m = list_make3(list_make1_int((int) p->fgattno), fgoids, fgconsts);
	}
	priv = lappend(priv, m);

	m = NIL;
	if (p->ngroupn > 0)
	{
		List	   *attnos = NIL;
		List	   *idxoids = NIL;

		for (i = 0; i < p->ngroupn; i++)
		{
			attnos = lappend_int(attnos, (int) p->groupn_attno[i]);
			idxoids = lappend_oid(idxoids, p->groupn_idx[i]);
		}
		m = list_make2(attnos, idxoids);
	}
	priv = lappend(priv, m);

	priv = lappend(priv, (p->allattno != 0) ?
				   list_make1_int((int) p->allattno) : NIL);
	priv = lappend(priv, (p->topkn > 0) ?
				   list_make3_int((int) p->topkn, p->topkcand,
								  p->topkstrict ? 1 : 0) : NIL);

	/* the aggregates and their OIDs are IntLists too, at the PLAN stage */
	m = NIL;
	if (p->nwcol > 0)
	{
		List	   *attnos = NIL;
		List	   *idxoids = NIL;
		List	   *cols = NIL;

		for (i = 0; i < p->nwcol; i++)
		{
			attnos = lappend_int(attnos, (int) p->wcol[i].attno);
			idxoids = lappend_oid(idxoids, p->wcol[i].idxoid);
			cols = lappend_int(cols, (int) p->wcol[i].idxcol);
		}
		m = list_make3(attnos, idxoids, cols);
		if (stage == LION_PRIV_STAGE_PLAN)
		{
			List	   *specs = NIL;

			for (i = 0; i < p->nwagg; i++)
			{
				const LionCountPrivWAgg *a = &p->wagg[i];

				specs = lappend(specs,
								list_make5_int(a->kind, a->col, a->argwidth,
											   (int) a->aggfnoid,
											   (int) a->collation));
			}
			m = lappend(m, specs);
		}
	}
	priv = lappend(priv, m);

	if (stage == LION_PRIV_STAGE_PLAN)
	{
		m = NIL;
		for (i = 0; i < p->ntl; i++)
			m = lappend_int(m, p->tlkind[i]);
		priv = lappend(priv, m);
	}
	return priv;
}

/*
 * Write *p as the custom_private of a LionCount node at `stage`: the list
 * lion_count_priv_decode() reads (lion_plan_private.h), positional, behind
 * the shape marker.  At the PLAN stage the clause values and the HAVING have
 * to have left the struct already: they are custom_exprs' and plan.qual's.
 *
 * An assert-enabled build decodes what it wrote, writes that again and
 * checks the two are equal(), so that any member the two halves of the codec
 * disagree on fails on the first plan that has it.
 */
List *
lion_count_priv_encode(const LionCountPriv *p, LionPrivStage stage)
{
	List	   *priv = lion_priv_encode(p, stage);

#ifdef USE_ASSERT_CHECKING
	{
		LionCountPriv again;

		lion_count_priv_decode(priv, stage, &again);
		Assert(equal(priv, lion_priv_encode(&again, stage)));
	}
#endif
	return priv;
}

/*
 * The WHERE clauses of a path being built, from the planner's parallel lists
 * of them (one element per clause each) and its OR restrictions, one IntList
 * {first, narms, armlen...} each: the clauses' kinds, columns and operators,
 * a copy of each value for CONSTS, and the index of each clause in
 * `whereidx` (IndexOptInfo, one per clause) - or InvalidOid throughout when
 * it is NIL, as for a partitioned table, whose indexes are its partitions'.
 */
void
lion_count_priv_set_where(LionCountPriv *p, List *whereidx, List *whereattnos,
						  List *whereconsts, List *wherekinds,
						  List *whereopnos, List *ors)
{
	ListCell   *lc;
	int			i;
	int			k;

	Assert(list_length(whereconsts) == list_length(whereattnos) &&
		   list_length(wherekinds) == list_length(whereattnos) &&
		   list_length(whereopnos) == list_length(whereattnos) &&
		   (whereidx == NIL ||
			list_length(whereidx) == list_length(whereattnos)));

	p->nclause = list_length(whereattnos);
	p->clause = (LionCountPrivClause *)
		palloc0(sizeof(LionCountPrivClause) * Max(p->nclause, 1));
	p->consts = NIL;
	for (i = 0; i < p->nclause; i++)
	{
		LionCountPrivClause *cl = &p->clause[i];

		cl->kind = list_nth_int(wherekinds, i);
		cl->idxoid = (whereidx != NIL) ?
			((IndexOptInfo *) list_nth(whereidx, i))->indexoid : InvalidOid;
		cl->attno = (AttrNumber) list_nth_int(whereattnos, i);
		cl->opno = list_nth_oid(whereopnos, i);
		p->consts = lappend(p->consts,
							copyObject((Node *) list_nth(whereconsts, i)));
	}

	p->nor = list_length(ors);
	p->ors = (p->nor > 0) ?
		(LionOrState *) palloc0(sizeof(LionOrState) * p->nor) : NULL;
	i = 0;
	foreach(lc, ors)
	{
		List	   *one = (List *) lfirst(lc);
		LionOrState *o = &p->ors[i++];

		o->first = linitial_int(one);
		o->narms = lsecond_int(one);
		o->armlen = (int *) palloc0(sizeof(int) * Max(o->narms, 1));
		o->nleaves = 0;
		for (k = 0; k < o->narms; k++)
		{
			o->armlen[k] = list_nth_int(one, 2 + k);
			o->nleaves += o->armlen[k];
		}
	}
}

/*
 * One PARTS target per relation of a partitioned table's `targets`
 * (LionCountTarget, DESIGN.md §16): its heap, its driving indexes when
 * `groupidx` - InvalidOid where it has none, and always for an FK-side join,
 * whose fact column grouped by is FACTGROUP's - and its index for each of
 * the clauses lion_count_priv_set_where() has set, InvalidOid for a clause
 * the partition's bounds imply.
 */
void
lion_count_priv_set_parts(LionCountPriv *p, List *targets, bool groupidx)
{
	ListCell   *lc;
	int			i = 0;
	int			j;

	p->npart = list_length(targets);
	p->part = (p->npart > 0) ?
		(LionCountPrivPart *) palloc0(sizeof(LionCountPrivPart) * p->npart) :
		NULL;
	foreach(lc, targets)
	{
		LionCountTarget *t = (LionCountTarget *) lfirst(lc);
		LionCountPrivPart *part = &p->part[i++];

		Assert(list_length(t->whereidx) == p->nclause);
		part->heapoid = t->heapoid;
		part->groupidxoid = (groupidx && t->driveidx[0] != NULL) ?
			t->driveidx[0]->indexoid : InvalidOid;
		part->groupidxoid2 = (groupidx && t->driveidx[1] != NULL) ?
			t->driveidx[1]->indexoid : InvalidOid;
		part->clauseidxoid = (Oid *) palloc0(sizeof(Oid) *
											 Max(p->nclause, 1));
		for (j = 0; j < p->nclause; j++)
		{
			IndexOptInfo *idx = (IndexOptInfo *) list_nth(t->whereidx, j);

			part->clauseidxoid[j] = (idx != NULL) ? idx->indexoid :
				InvalidOid;
		}
	}
}

/*
 * The heap column the driving index's entries belong to (DESIGN.md §24 needs
 * it to name that index's KEY COLUMN), or 0 when no index drives.  With a
 * GROUP BY it is the outer group column; a sum-over-all (§14) has none, and
 * its driver is the index of the FIRST `IS NOT NULL` clause - which is
 * exactly the clause the planner took its driving column from (`notnullvar`,
 * set at the first such leaf of the same list this array was built from, and
 * an OR leaf can never be one).
 */
AttrNumber
lion_count_priv_drive_attno(const LionCountPriv *p)
{
	AttrNumber	driveattno = p->groupattno;
	int			i;

	if ((p->flags & LION_FLAG_SUMALL) != 0)
	{
		/*
		 * ... or, when the plan has RANGE clauses (DESIGN.md §28), the column
		 * they bound, which is the one the planner drove the sum from: they
		 * all name it.
		 */
		bool		hasrange = (p->flags & LION_FLAG_RANGE) != 0;
		int			drivekind = hasrange ? LION_CLAUSE_RANGE :
			LION_CLAUSE_NOTNULL;

		driveattno = 0;
		if (p->allattno != 0 && !hasrange)
			driveattno = p->allattno;	/* the plan's (DESIGN.md §35) */
		for (i = 0; i < p->nclause && driveattno == 0; i++)
		{
			if (p->clause[i].kind == drivekind)
			{
				driveattno = p->clause[i].attno;
				break;
			}
		}
		if (driveattno == 0)
			elog(ERROR, "LionCount: sum-over-all without an IS NOT NULL or range clause");
	}

	/*
	 * count(DISTINCT k) (DESIGN.md §26): without a GROUP BY k's own entries
	 * drive the scan.
	 */
	if (p->distattno != 0 && p->groupattno == 0)
		driveattno = p->distattno;
	return driveattno;
}

/*
 * The heap column of the inner side of a nested loop, or 0: a second
 * grouping column's (DESIGN.md §20), or beside a GROUP BY the k of a
 * count(DISTINCT k), whose index is that inner side too (§26).
 */
AttrNumber
lion_count_priv_inner_attno(const LionCountPriv *p)
{
	if (p->groupattno2 != 0)
		return p->groupattno2;
	if (p->distattno != 0 && p->groupattno != 0)
		return p->distattno;
	return 0;
}

/*
 * The rules between the members of a decoded plan, which every plan this
 * build makes keeps: which shapes go together, and what each needs of the
 * others.  `parallel_aware` and `ncustom_plans` are the CustomScan's; at the
 * PATH stage, which has no target-list kinds, the rules about them hold
 * trivially.  Any of them broken is planner drift, said here rather than
 * counted wrong.
 */
void
lion_count_priv_check(const LionCountPriv *p, bool parallel_aware,
					  int ncustom_plans)
{
	bool		singlegroup = (p->flags & LION_FLAG_SINGLEGROUP) != 0;
	bool		sumall = (p->flags & LION_FLAG_SUMALL) != 0;
	bool		hasgroupidx = (p->flags & LION_FLAG_GROUPIDX) != 0;
	bool		hasrange = (p->flags & LION_FLAG_RANGE) != 0;
	bool		decode = (p->flags & LION_FLAG_DECODE) != 0;
	int			jflags = p->hasjoin ? p->join.flags : 0;
	bool		joinunique = (jflags & LION_JOINFLAG_UNIQUE) != 0;
	bool		joinrows = (jflags & LION_JOINFLAG_ROWS) != 0;
	bool		joinsum = (jflags & LION_JOINFLAG_SUM) != 0;
	bool		joincounts = (jflags & LION_JOINFLAG_COUNTS) != 0;
	bool		joinouter = (jflags & LION_JOINFLAG_OUTER) != 0;
	bool		joinordered = (jflags & LION_JOINFLAG_ORDERED) != 0;
	AttrNumber	driveattno;
	int			i;
	int			j;

	/* a sum over every row (DESIGN.md §35) is a sum */
	if (p->allattno != 0 && !sumall)
		elog(ERROR, "LionCount: a column for every row without a sum");

	/*
	 * The top k by count (DESIGN.md §36) is made of one grouping column
	 * walked in one table, counted a group at a time - neither the decoded
	 * walk nor the ranges of a parallel GROUP BY, which the planner never
	 * offers it with either (lion_count_path_topk() declines a decoded
	 * walk, and the parallel path a top k).
	 */
	if (p->topkn > 0 &&
		(p->groupattno == 0 || p->groupattno2 != 0 || p->distattno != 0 ||
		 sumall || p->npart > 0 || p->coalconst != NULL || decode ||
		 parallel_aware))
		elog(ERROR, "LionCount: a top k of another shape");

	/* GROUP BY coalesce(g, c) (DESIGN.md §10) is a one-column walk's */
	if (p->coalconst != NULL &&
		(p->groupattno == 0 || p->groupattno2 != 0 || p->distattno != 0 ||
		 !hasgroupidx))
		elog(ERROR, "LionCount: malformed coalesce group");

	/*
	 * The FK-side join (DESIGN.md §27): one child, and flags that go
	 * together - distinct keys only for an inner join with their operator;
	 * a sum is not rows, counts are an inner join's rows, and the outer
	 * side's rows (a join path) are a semi or anti join's rows alone.
	 */
	if (p->hasjoin &&
		(ncustom_plans != 1 ||
		 (joinunique &&
		  (p->join.type != LION_JOIN_INNER || !OidIsValid(p->join.sortop))) ||
		 (joinsum && joinrows) ||
		 (joincounts && (!joinrows || p->join.type != LION_JOIN_INNER)) ||
		 (joinouter &&
		  (!joinrows || p->join.type == LION_JOIN_INNER ||
		   joinunique || joincounts)) ||
		 (joinordered && !joinouter)))
		elog(ERROR, "LionCount: malformed join");

	/* a column of the child's rows needs a child */
	for (i = 0; i < p->ntl; i++)
	{
		if (LION_TL_IS_CHILDCOL(p->tlkind[i]) && !p->hasjoin)
			elog(ERROR, "LionCount: a child column without a join");
	}

	/*
	 * The aggregates over keys (DESIGN.md §37) are a sum over the entries
	 * with no GROUP BY, and the target list's keys and aggregates are among
	 * the plan's.
	 */
	if (p->nwcol > 0)
	{
		if (!sumall || p->groupattno != 0)
			elog(ERROR, "LionCount: malformed aggregates over keys");
#ifndef HAVE_INT128
		for (i = 0; i < p->nwagg; i++)
		{
			if (p->wagg[i].kind != LION_WAGG_EXTREME)
				elog(ERROR, "LionCount: a sum over keys without 128-bit integers");
		}
#endif
		for (i = 0; i < p->ntl; i++)
		{
			if ((LION_TL_IS_WKEY(p->tlkind[i]) &&
				 LION_TL_WKEY_COL(p->tlkind[i]) >= p->nwcol) ||
				(LION_TL_IS_WAGG(p->tlkind[i]) &&
				 LION_TL_WAGG_NO(p->tlkind[i]) >= p->nwagg))
				elog(ERROR, "LionCount: malformed aggregates over keys");
		}
	}

	/* a sum has a driver (lion_count_priv_drive_attno() says so) ... */
	driveattno = lion_count_priv_drive_attno(p);

	/* ... and so does a count(DISTINCT k): k's index (DESIGN.md §26) */
	if (p->distattno != 0 && !hasgroupidx)
		elog(ERROR, "LionCount: count(DISTINCT) without its index");

	/*
	 * RANGE clauses bound the driving walk (DESIGN.md §28), so there has to
	 * be one, and they have to be on its column.
	 */
	for (i = 0; i < p->nclause; i++)
	{
		if (p->clause[i].kind != LION_CLAUSE_RANGE)
			continue;
		if (!hasrange || !hasgroupidx || p->clause[i].attno != driveattno)
			elog(ERROR, "LionCount: a range clause that does not bound the driving walk");
	}

	/*
	 * A clause a partition leaves out has to be one the executor can do
	 * without there (lion_leaf_drops()): never the join's key, nor a range
	 * that bounds the driving walk.
	 */
	for (i = 0; i < p->npart; i++)
	{
		for (j = 0; j < p->nclause; j++)
		{
			if (!OidIsValid(p->part[i].clauseidxoid[j]) &&
				((p->hasjoin && j == p->join.clause) ||
				 p->clause[j].kind == LION_CLAUSE_RANGE))
				elog(ERROR, "LionCount: a partition leaves out a clause it needs");
		}
	}

	/* a fact column's groups (DESIGN.md §27) are an inner join's counts */
	if (p->fgattno != 0 &&
		(!p->hasjoin || p->join.type != LION_JOIN_INNER ||
		 joinrows || joinsum || joinouter))
		elog(ERROR, "LionCount: malformed fact group");

	/*
	 * A parallel-aware node that is no join is a parallel GROUP BY (DESIGN.md
	 * §10, "A GROUP BY in parallel"), which the planner offers for one shape
	 * alone: one column's entries walked whole, over one table.
	 */
	if (parallel_aware && !p->hasjoin &&
		(!hasgroupidx || p->groupattno == 0 || p->groupattno2 != 0 ||
		 sumall || singlegroup || p->coalconst != NULL ||
		 p->distattno != 0 || hasrange || p->npart > 0))
		elog(ERROR, "LionCount: malformed parallel GROUP BY");

	/*
	 * The decoded walk (DESIGN.md §34) has its columns, the first the one
	 * INTS names, and is the GROUP BY of one table alone.
	 */
	if (decode != (p->ngroupn > 0))
		elog(ERROR, "LionCount: malformed decoded walk");
	if (decode &&
		(p->groupattno != p->groupn_attno[0] || p->groupattno2 != 0 ||
		 p->distattno != 0 || p->coalconst != NULL || sumall ||
		 !hasgroupidx || hasrange || p->npart > 0 || p->hasjoin))
		elog(ERROR, "LionCount: malformed decoded walk");

	/*
	 * Only the key comes out of the sort of a forward semi join's distinct
	 * keys (DESIGN.md §27), so the key is the only column of the child the
	 * target list may read: the dimension of a forward semi join is not
	 * visible to the query above it, and what the join rows of a
	 * count(DISTINCT) carry is the key (lion_plan_fkjoin_path()).
	 */
	for (i = 0; joinunique && i < p->ntl; i++)
	{
		if (LION_TL_IS_CHILDCOL(p->tlkind[i]) &&
			LION_TL_CHILDRESNO(p->tlkind[i]) != p->join.keyresno)
			elog(ERROR, "LionCount: a distinct-key join reads a column other than its key");
	}
}
