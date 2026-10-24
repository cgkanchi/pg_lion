/*-------------------------------------------------------------------------
 *
 * lion_ordered.c
 *		LionOrdered: a CustomScan for
 *
 *			SELECT ... FROM t WHERE <clauses lion indexes answer> [AND ...]
 *			ORDER BY <columns an ordered index (a btree) provides> [LIMIT n]
 *
 *		that builds lion's exact TID set for the WHERE once, walks the btree
 *		in order reading only index tuples, tests each TID against the set
 *		BEFORE it touches the heap, and fetches, rechecks and returns only
 *		the members, until whatever is above it stops pulling.  DESIGN.md
 *		section 30 is the specification.
 *
 *		The ordered side may also be a walk of an ordered scalar column of a
 *		lion index itself, in its key order either way, for an ORDER BY of a
 *		column no btree orders (DESIGN.md §30.11); the lion side is then the
 *		other clauses' set, or nothing at all.
 *
 *		The ordered side may also be a lionvec index's order by distance from
 *		a vector (ORDER BY v <-> q, lion_vec.c, DESIGN.md §44.6): the set is
 *		built, and handed to the vector scan, which then reads and scores
 *		only its members.
 *
 *		When the btree returns every column the node must produce, the row's
 *		values come from the index tuple and the heap is visited only for
 *		pages the visibility map does not call all-visible, as core's Index
 *		Only Scan does (DESIGN.md §40); and such a walk is offered without an
 *		ORDER BY too, under the name LionBtreeScan, since it pays without one.
 *
 * Nothing here re-implements what core already decides.  The ORDERED side is
 * one of core's own ordered IndexPaths from the relation's path list, so its
 * pathkeys, direction and index quals are core's.  The LION side is one of
 * core's own lion index accesses - a bitmap path's bitmapqual tree, or a
 * plain lion IndexPath - built by running create_index_paths() once more on a
 * scratch copy of the relation that sees only its lion indexes, so exactly
 * what a lion bitmap scan would answer is answered here.  At run time each
 * lion leaf of that tree is a lion_source (lion_scan.c, DESIGN.md §29.3/§29.8)
 * copied into a sorted array of containers, and the tree's ANDs and ORs are
 * container-wise ANDs and ORs of those arrays.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <math.h>

#include "access/genam.h"
#include "access/itup.h"
#include "access/parallel.h"
#include "access/relscan.h"
#include "access/table.h"
#include "access/tableam.h"
#if PG_VERSION_NUM >= 200000
#include "access/tableam_indexscan.h"
#endif
#include "access/stratnum.h"
#include "access/visibilitymap.h"
#include "catalog/pg_am.h"
#include "catalog/pg_class.h"
#include "catalog/pg_type.h"
#include "commands/explain.h"
#include "common/hashfn.h"
#if PG_VERSION_NUM >= 180000
#include "commands/explain_format.h"
#endif
#include "executor/executor.h"
#include "executor/instrument.h"
#include "executor/nodeIndexscan.h"
#include "lib/stringinfo.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "nodes/extensible.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "nodes/pathnodes.h"
#include "nodes/plannodes.h"
#include "optimizer/clauses.h"
#include "optimizer/cost.h"
#include "optimizer/optimizer.h"
#include "optimizer/pathnode.h"
#include "optimizer/paths.h"
#include "optimizer/restrictinfo.h"
#include "parser/parsetree.h"
#include "pgstat.h"
#include "storage/bufmgr.h"
#include "storage/condition_variable.h"
#include "storage/fd.h"
#include "storage/predicate.h"
#include "storage/spin.h"
#include "utils/builtins.h"
#include "utils/dsa.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/ruleutils.h"
#include "utils/selfuncs.h"
#include "utils/snapmgr.h"
#include "utils/spccache.h"
#include "utils/tuplesort.h"
#include "utils/typcache.h"
#include "utils/wait_event.h"

#include "lion.h"
#include "lion_compat.h"
#include "lion_count.h"
#include "lion_costs.h"
#include "lion_customscan.h"
#include "lion_vec.h"
#include "lion_arena.h"

/* GUCs, and the hook this file chains (all installed by lion_ordered_init). */
bool		lion_enable_ordered_scan = true;
bool		lion_enable_btree_scan = true;
bool		lion_enable_lazy_set = true;
static bool lion_enable_shared_set = true;
static double lion_ordered_switch_ratio = 32.0;
static set_rel_pathlist_hook_type lion_prev_set_rel_pathlist_hook = NULL;

/*
 * custom_private of the plan is POSITIONAL, behind a shape marker (as §10's):
 *
 *	LO_PRIV_SHAPE	IntList (LO_PRIV_MAGIC, LO_PRIV_NMEMBERS)
 *	LO_PRIV_ORD		OidList (the ordered index: a btree, or the lion index
 *					whose column is walked, LO_FLAG_LIONWALK)
 *	LO_PRIV_INTS	IntList (scan direction, flags, where a lion column's walk
 *					puts its NULL entry - LION_ORDER_NULLS_* - and the
 *					column it walks, 1-based; 0 and 0 for a btree)
 *	LO_PRIV_ORDCOLS	IntList: the index column (0-based) of each ordered
 *					index qual, in custom_exprs' order
 *	LO_PRIV_TREE	List of IntList, the lion tree in preorder: an inner node
 *					is (LO_NODE_AND or LO_NODE_OR, number of children), a leaf
 *					(LO_NODE_LEAF, number of quals, their index columns ...);
 *					NIL when a lion column's walk has no set and every row it
 *					meets is fetched
 *	LO_PRIV_LEAVES	OidList: each leaf's lion index, in preorder
 *	LO_PRIV_SORT	the path's pathkeys as sort keys over the relation's own
 *					columns, for the fetch-and-sort switch (§30.4): a List of
 *					IntList (attnos), OidList (sort operators), OidList
 *					(collations), IntList (nulls first); NIL when a pathkey
 *					is not a plain column, and then the walk never switches
 *	LO_PRIV_IOCOLS	IntList, index-only mode (LO_FLAG_INDEXONLY, §40): for
 *					each column of the btree, the relation attno it returns,
 *					0 for one it does not (an expression); NIL in heap mode
 *	LO_PRIV_EXPECTED a float8 Const: the entries the planner expects the walk
 *					to visit (the ordered index's selectivity, or the walked
 *					range's, times the relation's tuples), for the switch's
 *					density gate (§40.4)
 *	LO_PRIV_LIMIT	an int8 Const: a lionvec order's (LO_FLAG_KNN), the rows
 *					its query level's LIMIT takes, OFFSET included, which the
 *					vector scan pauses by (lion_vec_scan_limit(), DESIGN.md
 *					§44.4, §44.6), or -1 when the planner did not know them;
 *					a btree's or a lion column's walk, the same rows when
 *					they bound what the node returns (lo_limit()), which
 *					bound the fetch-and-sort switch's sort (§30.4), or -1
 *
 * and custom_exprs holds six lists:
 *
 *	LO_EXPR_LIONQUALS	every leaf's index quals, key on the left, in leaf
 *						order (heap Vars; the executor substitutes INDEX_VAR)
 *	LO_EXPR_ORDQUALS	the ordered index's index quals, likewise
 *	LO_EXPR_LIONQUAL	the lion side's ORIGINAL qual (implicit AND), the
 *						recheck of an inexact set
 *	LO_EXPR_ORDORIG		the ordered index's original clauses (implicit AND),
 *						the recheck when the index sets xs_recheck
 *	LO_EXPR_VALUEQUAL	a lion column's walk: the restriction clauses on the
 *						walked column alone that the first visible row of an
 *						entry decides for the whole entry (implicit AND; §30.11,
 *						"A filter on the walked column"); NIL otherwise
 *	LO_EXPR_ORDERBYS	a lionvec index's order (LO_FLAG_KNN): its one ORDER BY
 *						clause, key on the left, likewise; NIL otherwise
 */
#define LO_PRIV_MAGIC		0x4c4f5244	/* "LORD" */
#define LO_PRIV_SHAPE		0
#define LO_PRIV_ORD			1
#define LO_PRIV_INTS		2
#define LO_PRIV_ORDCOLS		3
#define LO_PRIV_TREE		4
#define LO_PRIV_LEAVES		5
#define LO_PRIV_SORT		6
#define LO_PRIV_IOCOLS		7
#define LO_PRIV_EXPECTED	8
#define LO_PRIV_LIMIT		9
#define LO_PRIV_NMEMBERS	10

/*
 * The fetch-and-sort switch (DESIGN.md §30.4, "When the walk is not paying"):
 * once the walk of this scan has met LO_SWITCH_RATIO index entries per member
 * it has not met yet - and at least LO_SWITCH_MIN_WALK entries - the members
 * left are fetched and sorted instead.  The ratio is what one heap fetch of a
 * member costs in index entries walked, measured (§30.10: 0.04 us an entry,
 * 1-2 us a fetch, warm); pg_lion.ordered_switch_ratio sets it.  In
 * index-only mode (§40.4) the switch would read heap pages the walk never
 * touches, so there it is considered only while the walk has met markedly
 * fewer members than a uniform spread of them over the entries it is expected
 * to visit would have given it: LO_SWITCH_DENSITY of that count.
 */
#define LO_SWITCH_RATIO		32
#define LO_SWITCH_MIN_WALK	10000
#define LO_SWITCH_DENSITY	0.5

/*
 * What the walk spends on each btree entry it meets (DESIGN.md §30.3,
 * §40.3): the btree step, the set probe (a binary search over the container
 * keys and a membership test) and the met-before mark, in cpu_tuple_costs
 * over the one cpu_operator_cost indextotalcost does not hold.  Measured on
 * the quick benchmark's 5M-row table (§40.3): a full covering walk takes
 * 670 to 890 ms, 170 ns an entry, against 85 ns a row for a sequential scan
 * with its filter, which the planner charges cpu_tuple_cost +
 * cpu_operator_cost; one cpu_operator_cost for the probe was a fifth of that
 * for twice the work, and chose the walk over plans it ran three to four
 * times slower than.
 */
#define LO_PROBE_TUPLES		2

/*
 * The set evaluated lazily (DESIGN.md §30.4, "The set, lazily"), until its
 * probes have cost what building it would: a probe of a stream is a unit of
 * work for each of its posting sets, starting a stream again behind where it
 * stands LO_LAZY_RESTART_WORK more for each, and the build reads each of the
 * sets' containers once - the budget, never below LO_LAZY_MIN_WORK.  A walk
 * that has met LO_LAZY_WALK_WORK entries for each unit of the budget - and
 * at least LO_SWITCH_MIN_WALK - is built for too, so that the early stop and
 * the fetch-and-sort switch, which need the set's size, can act on it once
 * the walk has cost about what the build will: an entry walked costs 30 to
 * 70 ns, a unit of the build 350 (§30.4: 8.5 ms for a budget of 24,040);
 * as is one whose memo has taken half of hash_mem.
 */
#define LO_LAZY_MIN_WORK		64
#define LO_LAZY_RESTART_WORK	4
#define LO_LAZY_WALK_WORK		8

/*
 * The set is started lazily only when the planner expects the walk to stop
 * short: the node alone in its query under a LIMIT (and OFFSET) of fewer
 * than LO_LAZY_MAX_SHARE of the rows it would return, or a walk expected to
 * end before LO_SWITCH_MIN_WALK entries anyway.  A walk that goes on further
 * meets most of the set's container keys - one in heap order passes over
 * them once for each entry it walks - and a probe of a key costs more than
 * the build's read of it, so the set is built at once.
 */
#define LO_LAZY_MAX_SHARE		0.2

/*
 * ... and a walk that does not go in heap order is built for at once: once
 * LO_LAZY_ORDER_KEYS keys have been evaluated, if one in LO_LAZY_BACK_SHARE
 * of them or more lay behind the key evaluated before it.  A btree walked in
 * an order the heap does not follow - a score, a random key - hands out TIDs
 * at random container keys, half of them behind the last, and each of those
 * seeks every set of the tree again by a descent: a probe of a key cost as
 * much as eight containers of the build, and the probes of 2,012 keys of a
 * 5M-row table (a 0.67% filter, ORDER BY a random score LIMIT 100) came to
 * 29 ms where the build and the walk took 10 (2026-09-30).  A lion column's
 * walk goes back only between entries, and a btree correlated with the heap
 * seldom does.
 */
#define LO_LAZY_ORDER_KEYS		64
#define LO_LAZY_BACK_SHARE		4

/* Why a lazy set was built, for EXPLAIN ANALYZE: lo_lazy_why()'s answers. */
typedef enum LoLazyWhy
{
	LO_LAZY_HEAP_ORDER,			/* the walk does not go in heap order */
	LO_LAZY_BUDGET,				/* the probes cost what the build would */
	LO_LAZY_SWITCH,				/* the switch could be due */
	LO_LAZY_LONG_WALK,			/* LO_LAZY_WALK_WORK entries a unit met */
	LO_LAZY_MEMO,				/* the memo outgrew half of hash_mem */
	LO_LAZY_NWHY
} LoLazyWhy;

static const char *const lo_lazy_why_names[LO_LAZY_NWHY] = {
	"heap order", "probe budget", "switch due", "long walk", "memo size"
};

#define LO_EXPR_LIONQUALS	0
#define LO_EXPR_ORDQUALS	1
#define LO_EXPR_LIONQUAL	2
#define LO_EXPR_ORDORIG		3
#define LO_EXPR_VALUEQUAL	4
#define LO_EXPR_ORDERBYS	5
#define LO_EXPR_NLISTS		6

#define LO_NODE_LEAF		0
#define LO_NODE_AND			1
#define LO_NODE_OR			2

#define LO_FLAG_LOSSY		0x0001	/* a lion index clause was lossy */
#define LO_FLAG_LIONWALK	0x0002	/* the order is a lion column's walk */
#define LO_FLAG_INDEXONLY	0x0004	/* the values come from the btree (§40) */
#define LO_FLAG_HEAPRECHECK	0x0008	/* the lion recheck reads the heap tuple */
#define LO_FLAG_BUILD		0x0010	/* the set built at once, never lazily */
#define LO_FLAG_KNN			0x0020	/* the order is a lionvec index's (§44.6) */
#define LO_FLAG_PROBE		0x0040	/* a nested loop's probe (§40.10) */
#define LO_FLAG_SIDE		0x0080	/* a side of a LionOrderedJoin (§47) */
#define LO_FLAG_KEYED		0x0100	/* ... its dimension, keyed by the join */

/*
 * A side of a LionOrderedJoin (DESIGN.md §47) is a LionOrdered path built
 * for it and never added to its relation (lion_ordered_side_path()): its
 * custom_private is a btree walk's three members and a fourth, the side's -
 * an IntList (LO_SIDE_MAGIC, LO_SIDE_FACT or LO_SIDE_DIM), a float8 Const
 * (the fact's: the share of its walk the join's LIMIT is expected to take;
 * the dimension's: the descents the join is expected to make) and, for the
 * dimension, the qual its unique btree is descended by, `pk = NULL`, the
 * NULL the key each descent is handed (lion_ordered_probe_key()).
 */
#define LO_SIDE_MAGIC		0x4c4f534a	/* "LOSJ" */
#define LO_SIDE_FACT		1
#define LO_SIDE_DIM			2

/*
 * The plan's custom_private as a C struct: lo_priv_decode() is its one
 * reader and lo_priv_encode() its one writer, the members above in their
 * positions.  Lists point into the plan; the sort keys' arrays are palloc'd
 * in the caller's context.  The lion tree stays a List: lo_decode_tree()
 * checks it against the leaves and the quals, which are custom_exprs'.
 */
typedef struct LoPriv
{
	Oid			ordoid;			/* ORD */
	int			dir;			/* INTS: a ScanDirection */
	int			flags;			/* ... LO_FLAG_* */
	int			nulls;			/* ... LION_ORDER_NULLS_* */
	AttrNumber	walkattno;		/* ... 0 for a btree */
	List	   *ordcols;		/* ORDCOLS: IntList */
	List	   *tree;			/* TREE: NIL when there is no set */
	List	   *leaves;			/* LEAVES: OidList */
	bool		hassort;		/* SORT: false for NIL */
	int			nsort;
	AttrNumber *sortattno;
	Oid		   *sortop;
	Oid		   *sortcoll;
	bool	   *nullsfirst;
	List	   *iocols;			/* IOCOLS: IntList, NIL in heap mode */
	double		expected;		/* EXPECTED */
	int64		limit;			/* LIMIT: positive, or -1 */
} LoPriv;

/*
 * The flags of a LionOrdered plan, or 0 if it does not look like one: for
 * lo_create_state(), which has to fix the slot's type before lo_begin()
 * decodes the rest and errors out on a malformed list.
 */
static int
lo_priv_flags(List *priv)
{
	List	   *ints;

	if (list_length(priv) != LO_PRIV_NMEMBERS)
		return 0;
	ints = (List *) list_nth(priv, LO_PRIV_INTS);
	if (ints == NIL || !IsA(ints, IntList) || list_length(ints) != 4)
		return 0;
	return lsecond_int(ints);
}

/* A member that has to be NIL or a list of node type tag */
static List *
lo_priv_list(List *priv, int member, NodeTag tag)
{
	List	   *l = (List *) list_nth(priv, member);

	if (l != NIL && nodeTag(l) != tag)
		elog(ERROR, "LionOrdered: malformed custom_private member %d", member);
	return l;
}

/*
 * Read a LionOrdered plan's custom_private into *out, checking the shape
 * marker, every member's node type and the sort keys' lengths.  A list this
 * build did not write is an ERROR, not a misread Oid.
 */
static void
lo_priv_decode(List *priv, LoPriv *out)
{
	List	   *shape;
	List	   *ord;
	List	   *ints;
	List	   *sk;
	Const	   *c;
	int			i;

	memset(out, 0, sizeof(LoPriv));
	shape = (list_length(priv) == LO_PRIV_NMEMBERS) ?
		(List *) list_nth(priv, LO_PRIV_SHAPE) : NIL;
	if (shape == NIL || !IsA(shape, IntList) || list_length(shape) != 2 ||
		linitial_int(shape) != LO_PRIV_MAGIC ||
		lsecond_int(shape) != LO_PRIV_NMEMBERS)
		elog(ERROR, "LionOrdered: unrecognized custom_private shape (%d members)",
			 list_length(priv));

	ord = lo_priv_list(priv, LO_PRIV_ORD, T_OidList);
	ints = lo_priv_list(priv, LO_PRIV_INTS, T_IntList);
	if (list_length(ord) != 1 || list_length(ints) != 4)
		elog(ERROR, "LionOrdered: malformed custom_private");
	out->ordoid = linitial_oid(ord);
	out->dir = linitial_int(ints);
	out->flags = lsecond_int(ints);
	out->nulls = lthird_int(ints);
	out->walkattno = (AttrNumber) lfourth_int(ints);

	out->ordcols = lo_priv_list(priv, LO_PRIV_ORDCOLS, T_IntList);
	out->tree = lo_priv_list(priv, LO_PRIV_TREE, T_List);
	out->leaves = lo_priv_list(priv, LO_PRIV_LEAVES, T_OidList);
	out->iocols = lo_priv_list(priv, LO_PRIV_IOCOLS, T_IntList);

	/* the sort keys: four lists of one element per key each */
	sk = lo_priv_list(priv, LO_PRIV_SORT, T_List);
	if (sk != NIL)
	{
		List	   *attnos;
		List	   *ops;
		List	   *colls;
		List	   *nulls;

		if (list_length(sk) != 4)
			elog(ERROR, "LionOrdered: malformed sort keys");
		attnos = (List *) linitial(sk);
		ops = (List *) lsecond(sk);
		colls = (List *) lthird(sk);
		nulls = (List *) lfourth(sk);
		out->nsort = list_length(attnos);
		if ((attnos != NIL && !IsA(attnos, IntList)) ||
			(ops != NIL && !IsA(ops, OidList)) ||
			(colls != NIL && !IsA(colls, OidList)) ||
			(nulls != NIL && !IsA(nulls, IntList)) ||
			list_length(ops) != out->nsort ||
			list_length(colls) != out->nsort ||
			list_length(nulls) != out->nsort)
			elog(ERROR, "LionOrdered: malformed sort keys");
		out->hassort = true;
		out->sortattno = (AttrNumber *)
			palloc(sizeof(AttrNumber) * Max(out->nsort, 1));
		out->sortop = (Oid *) palloc(sizeof(Oid) * Max(out->nsort, 1));
		out->sortcoll = (Oid *) palloc(sizeof(Oid) * Max(out->nsort, 1));
		out->nullsfirst = (bool *) palloc(sizeof(bool) * Max(out->nsort, 1));
		for (i = 0; i < out->nsort; i++)
		{
			out->sortattno[i] = (AttrNumber) list_nth_int(attnos, i);
			out->sortop[i] = list_nth_oid(ops, i);
			out->sortcoll[i] = list_nth_oid(colls, i);
			out->nullsfirst[i] = list_nth_int(nulls, i) != 0;
		}
	}

	c = (Const *) list_nth(priv, LO_PRIV_EXPECTED);
	if (c == NULL || !IsA(c, Const) || c->consttype != FLOAT8OID ||
		c->constisnull)
		elog(ERROR, "LionOrdered: malformed expected walk");
	out->expected = DatumGetFloat8(c->constvalue);

	c = (Const *) list_nth(priv, LO_PRIV_LIMIT);
	if (c == NULL || !IsA(c, Const) || c->consttype != INT8OID ||
		c->constisnull)
		elog(ERROR, "LionOrdered: malformed LIMIT");
	out->limit = DatumGetInt64(c->constvalue);
	if (out->limit != -1 && out->limit <= 0)
		elog(ERROR, "LionOrdered: malformed LIMIT " INT64_FORMAT, out->limit);
}

/* lo_priv_encode() without its check */
static List *
lo_priv_build(const LoPriv *p)
{
	List	   *sk = NIL;
	List	   *priv;

	if (p->hassort)
	{
		List	   *attnos = NIL;
		List	   *ops = NIL;
		List	   *colls = NIL;
		List	   *nulls = NIL;
		int			i;

		for (i = 0; i < p->nsort; i++)
		{
			attnos = lappend_int(attnos, p->sortattno[i]);
			ops = lappend_oid(ops, p->sortop[i]);
			colls = lappend_oid(colls, p->sortcoll[i]);
			nulls = lappend_int(nulls, p->nullsfirst[i] ? 1 : 0);
		}
		sk = list_make4(attnos, ops, colls, nulls);
	}

	priv = list_make5(list_make2_int(LO_PRIV_MAGIC, LO_PRIV_NMEMBERS),
					  list_make1_oid(p->ordoid),
					  list_make4_int(p->dir, p->flags, p->nulls,
									 (int) p->walkattno),
					  p->ordcols,
					  p->tree);
	priv = lappend(priv, p->leaves);
	priv = lappend(priv, sk);
	priv = lappend(priv, p->iocols);
	priv = lappend(priv, makeConst(FLOAT8OID, -1, InvalidOid, sizeof(float8),
								   Float8GetDatum(p->expected),
								   false, FLOAT8PASSBYVAL));
	priv = lappend(priv, makeConst(INT8OID, -1, InvalidOid, sizeof(int64),
								   Int64GetDatum(p->limit),
								   false, FLOAT8PASSBYVAL));
	return priv;
}

/*
 * Write *p as a LionOrdered plan's custom_private.  An assert-enabled build
 * decodes what it wrote, writes that again and checks the two are equal(),
 * as lion_count_priv_encode() does.
 */
static List *
lo_priv_encode(const LoPriv *p)
{
	List	   *priv = lo_priv_build(p);

#ifdef USE_ASSERT_CHECKING
	{
		LoPriv		again;

		lo_priv_decode(priv, &again);
		Assert(equal(priv, lo_priv_build(&again)));
	}
#endif
	return priv;
}

/* ---------------------------------------------------------------------
 * The TID set
 * --------------------------------------------------------------------- */

/*
 * A set of heap TIDs as containers (§3) sorted by container key, one a key,
 * none of them NULL.  Its memory is lo_set_alloc()'s: within hash_mem, or
 * spilled (DESIGN.md §30.4, "Spilling").
 */
typedef struct LionTidSet
{
	int			n;
	int			cap;
	uint32	   *keys;			/* strictly ascending */
	LionContainer **conts;
} LionTidSet;

/*
 * Bytes of directory the planner counts per container key, on top of the
 * container (§30.3): keys[] and conts[] grow by doubling, so about a third
 * over their entries; and the key's first place (LionOrderedState.places).
 */
#define LO_ENTRY_BYTES		((Size) (sizeof(uint32) + sizeof(LionContainer *) + 4 + \
									 sizeof(uint64)))

/*
 * The walk's visited bits (DESIGN.md §30.4, "Stopping early"), a bit a place
 * of the set's members (lion_container_places()), come in chunks of this
 * many, each made when the walk first meets a member of its places: a
 * LIMIT walk that meets a few members clears a few chunks, not a bit for
 * every member of the set.
 */
#define LO_VISITED_BITS		((uint64) LION_CONTAINER_RANGE)
#define LO_VISITED_BYTES	((Size) (LO_VISITED_BITS / 8))

/*
 * A flush of a key's gathered members (lo_gather_flush()) merges them
 * member by member, not through a bitset image, while the key has this
 * many or fewer together: LION_WIDTH_ARRAY_CARD(1), 256 at 8K, an ARRAY as
 * large as the narrowest NARROW, so that what the merge leaves is always an
 * ARRAY or a RUN.  Below it the merge won at every shape measured, 2 to 64
 * pieces of members in any block at any offset: 0.9 us against the image's
 * 1.6 for 4 pieces of 256 together, at the worst, and from 30 times faster
 * for a few members.  Past it a union the image makes a NARROW of had to
 * become one from the merge's ARRAY, which took 2.5 to 10 us against the
 * image's 1 to 3 (2026-10-17).
 */
#define LO_MERGE_DIRECT_MAX	((uint32) LION_WIDTH_ARRAY_CARD(1))

/*
 * A leaf's pieces gathered by container key as its source hands them out
 * (lo_build_leaf(); DESIGN.md §30.4, "Gathering the pieces").  A WALK (a
 * range) or a LIST (a long IN list) source hands out a container key once
 * per entry or per batch (§29.3), so a range over a unique column brings
 * about a piece a member, in entry order: 5M pieces of one member each for
 * the 260,417 keys of a 10% range over 50M rows with summaries.  A
 * container key is a heap block / LION_BLOCKS_PER_CONTAINER, a small
 * integer, so the gather is a table of the keys, in chunks of
 * LO_GATHER_CHUNK_KEYS slots made when a key of theirs is first met, a
 * cache line a key: the key's container so far, in the set's memory
 * context, and room for LO_GATHER_INLINE members not in it yet.  A piece of
 * a few members goes into that room, and when the room is full, or a piece
 * brings more, the room and the piece go into the container
 * (lo_gather_flush()): merged member by member while the key has at most
 * LO_MERGE_DIRECT_MAX members (lion_container_or_merge()), through a bitset
 * image past that, and straight into the container's bits once the key has
 * more members than an ARRAY holds, when the container is kept as a BITSET
 * and optimized at the end.  When the source is done the keys are taken into
 * the set in order, which the table gives for nothing (lo_gather_finish()).
 * So a piece costs its slot's cache line and the copy of its members, a key
 * whose members come one at a time costs a flush per LO_GATHER_INLINE of
 * them, and the gather holds about what the set will: a chunk per 64 keys
 * met, and a container a key.
 *
 * Until 2026-10-19 every piece was kept as it came, a copy and a directory
 * entry each, and the directory was sorted by key and its pieces ORed
 * whenever it filled, which rewrote every key a piece had come for since
 * the last time - nearly every key, for the pieces of a range: on 2M rows
 * of ten a heap page (3,125 keys) a 10% range over a unique column with
 * summaries of 64 rows, 198,000 pieces of one member, took 245 ns a piece
 * before the node's first row, 70 of them the source's, 40 the copy, 64 the
 * sort and 70 the four merges, each of nearly every key; at 50M rows the
 * same filter's 5M pieces took 845 ms, 170 ns each.  Gathered, the 2M
 * table's 198,000 pieces take 105 ns each, 70 of them the source's: the
 * 10% range's set in 21 ms against 55, the 3% range's in 5 against 12 and
 * the 30% range's, 594,000 pieces, in 81 against 155; with summaries of
 * 4,096 rows 17 ms against 29 at 10%, and without summaries, an entry a
 * value, 74 against 113 (2026-10-19, the least of 7 runs each).
 */
#define LO_GATHER_CHUNK_KEYS	64
#define LO_GATHER_INLINE		((64 - sizeof(LionContainer *) - sizeof(uint16)) / sizeof(uint16))

typedef struct LoGatherSlot
{
	LionContainer *c;			/* the key's members so far (the set's), or NULL */
	uint16		n;				/* members in vals[] not in c yet */
	uint16		vals[LO_GATHER_INLINE];
} LoGatherSlot;

StaticAssertDecl(sizeof(LoGatherSlot) == 64, "a gather slot is a cache line");

#define LO_GATHER_CHUNK_BYTES	(sizeof(LoGatherSlot) * LO_GATHER_CHUNK_KEYS)

typedef struct LoGather
{
	LoGatherSlot **chunks;		/* chunk k holds keys 64k .. 64k + 63; NULL until one is met */
	uint64	   *touched;		/* a bit a key met, by chunk */
	uint32		nchunks;		/* the room in chunks[] and touched[] */
	uint32		maxchunk;		/* one past the highest chunk a key was met in */
	uint64	   *img;			/* scratch: a bitset image of one key */
	LionContainer *out;			/* scratch: LION_CONTAINER_MAX_SIZE bytes */
	LionContainer *pend;		/* scratch: a slot's pending members as an ARRAY */
} LoGather;

/* ---------------------------------------------------------------------
 * Executor state
 * --------------------------------------------------------------------- */

/*
 * One scan key of a leaf, for the lazy set: its posting sets, located once
 * and let go of (no pin is held between rows, §30.5), the tree over them
 * (lion_scankey_sets()), and a stream of that tree's containers which is only
 * ever sought forward - started again when a probe is behind it.  A NEGATED
 * key is `col IS NOT NULL`: its set is the column's NULL entry, taken away
 * from what the leaf's other keys select (DESIGN.md §14, §30.4).
 */
typedef struct LoProbeKey
{
	int			nsets;
	LionPostingSet *sets;
	LionKeyNode *tree;			/* NULL: the key selects nothing - or, when
								 * negated, takes nothing away */
	bool		negated;
	MemoryContext cxt;			/* the stream's */
	LionSetStream *stream;		/* NULL until the first probe */
	uint32		last;			/* the container key last sought */
} LoProbeKey;

typedef struct LoLeaf
{
	Oid			indexoid;
	Relation	index;
	List	   *quals;			/* fixed: INDEX_VAR on the left */
	ScanKey		keys;
	int			nkeys;
	IndexRuntimeKeyInfo *rtkeys;
	int			nrtkeys;
	LoProbeKey *pkeys;			/* [nkeys] while the set is lazy, else NULL */
} LoLeaf;

typedef struct LoNode
{
	int			kind;
	int			nchild;
	struct LoNode **child;
	LoLeaf	   *leaf;
} LoNode;

/*
 * What the lazy set is at one container key the walk met: its members there,
 * and the ones this scan's walk has met.
 */
typedef struct LoMemoEnt
{
	uint32		ckey;
	char		status;
	LionContainer *c;			/* NULL: no member at this key */
	uint64	   *visited;		/* in scancxt, NULL until a member is met */
} LoMemoEnt;

#define SH_PREFIX		lo_memo
#define SH_ELEMENT_TYPE	LoMemoEnt
#define SH_KEY_TYPE		uint32
#define SH_KEY			ckey
#define SH_HASH_KEY(tb, key)	murmurhash32(key)
#define SH_EQUAL(tb, a, b)		((a) == (b))
#define SH_SCOPE		static inline
#define SH_DECLARE
#define SH_DEFINE
#include "lib/simplehash.h"

/*
 * A parallel walk (DESIGN.md §40.9).  The participants of a parallel-aware
 * plan share the btree's parallel scan, which hands each of them leaf pages
 * of its own, and the lion set, which the first of them to need it builds
 * while the others wait: into the query's DSA when it fits the room a
 * Parallel Hash's table would have - a hash table's memory for each
 * participant planned - and each reads it there (LO_SHSET_SHARED); and
 * otherwise each builds its own, as the serial node does (LO_SHSET_OWN).
 * EXPLAIN ANALYZE's counters are summed here too.  The struct lives in the
 * Gather's dynamic shared memory, the btree's parallel scan descriptor
 * after it.
 */
#define LO_SHSET_NONE		0	/* nobody has started the set */
#define LO_SHSET_BUILDING	1	/* a participant is building it */
#define LO_SHSET_SHARED		2	/* ... and has put it in the DSA */
#define LO_SHSET_OWN		3	/* ... and kept it: each builds its own */

/* What EXPLAIN ANALYZE adds up over a parallel walk's participants */
typedef struct LoCounts
{
	uint64		walked;
	uint64		hits;
	uint64		fetched;
	uint64		removed;
	uint64		builds;
	uint64		published;		/* builds put into the DSA for all */
	uint64		earlystops;
} LoCounts;

typedef struct LoShared
{
	slock_t		mutex;
	LoCounts	sums;			/* the workers', each added as it shuts down */

	/* the set, and what its last builder saw of it */
	int			setstate;		/* LO_SHSET_* */
	ConditionVariable setcv;	/* broadcast as setstate leaves BUILDING */
	dsa_pointer setimage;		/* LO_SHSET_SHARED: a LoSetImage */
	Size		setroom;		/* the most of the DSA it may take */
	int			ncont;
	bool		exact;
	Size		spillbytes;

	Size		pscanoff;		/* where the btree's parallel scan is */
} LoShared;

/*
 * The set in the DSA, one allocation (lo_set_publish()): its container keys,
 * where each container is from the image's start, and the containers, each
 * at a MAXALIGNed offset.
 */
typedef struct LoSetImage
{
	int			n;
	bool		exact;
} LoSetImage;

#define LO_IMAGE_KEYS(img) \
	((uint32 *) ((char *) (img) + MAXALIGN(sizeof(LoSetImage))))
#define LO_IMAGE_OFFS(img) \
	((Size *) ((char *) LO_IMAGE_KEYS(img) + \
			   MAXALIGN(sizeof(uint32) * (Size) (img)->n)))
#define LO_IMAGE_HEAD(n) \
	(MAXALIGN(sizeof(LoSetImage)) + MAXALIGN(sizeof(uint32) * (Size) (n)) + \
	 MAXALIGN(sizeof(Size) * (Size) (n)))

typedef struct LionOrderedState
{
	CustomScanState css;

	/* the ordered index */
	Oid			ordoid;
	Relation	ordidx;
	ScanDirection dir;
	ScanKey		okeys;
	int			nokeys;
	IndexRuntimeKeyInfo *ortkeys;
	int			nortkeys;
	ExprContext *ortcxt;		/* its run-time keys' values live here */
	IndexScanDesc scan;
	bool		started;		/* the walk is positioned for this scan */
	bool		done;

	/*
	 * ... or a lionvec index's order by distance (DESIGN.md §44.6): its
	 * ORDER BY key, whose run-time value is among ortkeys, and the set
	 * handed to its scan after every rescan.
	 */
	bool		knn;
	ScanKey		obkeys;
	int			nobkeys;
	int64		knnlimit;		/* the rows its LIMIT takes, or -1 */

	/*
	 * ... or a nested loop's probe (DESIGN.md §40.10): a btree's walk
	 * parameterized by the outer row, a scan a descent for its keys, over the
	 * set the loop's scans share; no scan of it counts what it has met.
	 */
	bool		probe;

	/*
	 * ... or a side of a LionOrderedJoin (DESIGN.md §47): a btree's walk that
	 * may have no set, its rows those of the relation's other quals - and its
	 * dimension, a probe whose one index qual is the key the join hands it
	 * before each descent (lion_ordered_probe_key()).
	 */
	bool		side;
	bool		keyed;

	/*
	 * ... narrowed, for a LionOrderedSplit's arm (DESIGN.md §48), to the rows
	 * of one entry of the split's column: that entry's TIDs ANDed with the
	 * set a container at a time as the split hands them in
	 * (lion_ordered_narrow_add()), which a TID a descent finds is tested
	 * against in place of the set.  In memory of its own: a lazy set's build
	 * (lo_lazy_convert()) resets the set's.
	 */
	bool		narrowing;
	LionTidSet *narrow;
	MemoryContext narrowcxt;	/* the narrowed set; reset per arm */
	LionContainer *narrowbuf;	/* the entry's TIDs of one container key */
	LionContainer *narrowand;	/* ... ANDed with the set's */
	bool		narrowopen;		/* narrowbuf holds the key's TIDs so far */
	uint32		narrowlast;		/* the key narrowbuf was opened for last */
	uint64		narrowmembers;

	/*
	 * ... or a lion column's walk (DESIGN.md §30.11): the column, where its
	 * NULL entry goes, the walk of this scan, and the fetch of what it meets
	 * (the TID it hands out is a HOT chain's root, as a btree's is).
	 */
	bool		lionwalk;
	AttrNumber	walkattno;
	int			walknulls;
	LionOrderWalk *owalk;
	MemoryContext walkcxt;
	ItemPointerData walktid;
#if PG_VERSION_NUM < 200000
	struct IndexFetchTableData *fetch;
#endif
	uint64		walkkeys;		/* EXPLAIN ANALYZE: keys the walks read */

	/*
	 * ... and its filter on the walked column (§30.11, "A filter on the
	 * walked column"): the entry whose first visible row has decided for it,
	 * and how many entries were passed over for failing.
	 */
	ExprState  *valuequal;
	int64		valueentry;
	uint64		valueskips;

	/*
	 * Index-only mode (DESIGN.md §40): the row's values come from the btree's
	 * index tuple, through the mapping of its columns to the relation's, and
	 * the heap is visited - into a slot of its own, for visibility alone - on
	 * pages the visibility map does not call all-visible.  A name column the
	 * btree stores as a cstring is copied into a Name of the node's.
	 */
	bool		indexonly;
	bool		lionheaprecheck;	/* the btree lacks the lion qual's columns */
	int			niocols;		/* the btree's columns */
	AttrNumber *iocols;			/* [niocols] the attno each returns, or 0 */
	bool	   *ionames;		/* [niocols] stored as cstring, returned as name */
	NameData   *ionamebuf;		/* [niocols] where those are copied to */
	Datum	   *iovals;			/* [niocols] the index tuple, deformed */
	bool	   *ionulls;
	TupleTableSlot *tabslot;	/* the heap tuple, for its visibility */
	Buffer		vmbuffer;		/* the visibility map page last read */

	/* the lion side; noset: none, every row the walk meets is fetched */
	bool		noset;
	LoNode	   *tree;
	LoLeaf	   *leaves;
	int			nleaves;
	ExprContext *lrtcxt;		/* the leaves' run-time keys' values */
	bool		lossyqual;
	Bitmapset  *lionparams;		/* Param ids the lion quals use */
	ExprState  *lionrecheck;
	ExprState  *ordrecheck;

	/* the set, and what building it costs */
	MemoryContext setcxt;		/* the set; reset per build */
	MemoryContext buildcxt;		/* sources while building */
	LionTidSet *set;			/* NULL: not built for this scan yet */
	List	   *live;			/* sets alive during a build */
	Size		bytes;			/* the set's memory, in setcxt and buildcxt */
	Size		limit;			/* ... within which it stays: hash_mem */
	bool		spilled;		/* this build's set outgrew it: in the arena */
	LionArena  *arena;			/* the set past hash_mem (§30.4, "Spilling") */
	File		arenafile;		/* its temporary file, once needed */
	MemoryContextCallback arenacb;	/* unmaps it with the query's memory */
	Size		spillbytes;		/* EXPLAIN: arena bytes the last build and its
								 * walks' visited bits took */
	bool		exact;
	uint64		members;		/* members of the set */
	uint64	   *places;			/* [set->n + 1]: the places before each key */

	/*
	 * ... or the set evaluated only at the container keys the walk meets
	 * (DESIGN.md §30.4, "The set, lazily"), and what it is at each of them
	 * remembered; built as above once the probes have cost what that would.
	 */
	bool		lazyok;			/* every leaf's keys are sets to seek in */
	bool		lazy;			/* the set is lazy now */
	bool		lazyempty;		/* ... and selects nothing at all */
	MemoryContext lazycxt;		/* the leaves' sets and the memo */
	MemoryContext probecxt;		/* one key's evaluation */
	lo_memo_hash *memo;
	Size		memobytes;
	double		lazywork;
	double		lazybudget;
	double		lazymembers;	/* at most this many members: the sets' hints */
	uint64		lazyevals;		/* keys evaluated since the set was started */
	uint64		lazyback;		/* ... that lay behind the key before them */
	uint32		lazylast;		/* the key evaluated last */
	uint64		lazykeys;		/* EXPLAIN ANALYZE: keys evaluated */
	uint64		lazywhy[LO_LAZY_NWHY];	/* ... and why it was built, per build */

	/*
	 * This scan's walk (DESIGN.md §30.4): which members it has met, a bit a
	 * place (places[] above), so that a member counts once however often the
	 * walk meets its TID - the bits in chunks of LO_VISITED_BITS,
	 * lo_set_alloc()'s as the set's memory is, within hash_mem and past it in
	 * the arena; and the fetch-and-sort switch.
	 */
	MemoryContext scancxt;		/* reset per scan */
	uint64	  **visited;		/* [nvisited], NULL until a member is met */
	uint64		nvisited;		/* chunks */
	uint64		distinct;		/* members met in this scan */
	uint64		scanwalked;		/* entries walked in this scan */
	uint64		scanreturned;	/* rows this scan returned */
	double		switchratio;	/* entries walked a member's fetch is worth */
	double		expectwalk;		/* entries the planner expects the walk to visit */

	/*
	 * The switch's sort (lo_switch()): a tuplesort of the members' rows, each
	 * its sort keys - the relation's columns sortattnos[], by the pathkeys'
	 * operators, collations and NULLS placement - and then its heap tuple's
	 * bytes, which come out as they went in.  At most `bound` rows a scan,
	 * the walk's included, when the plan knows that is all it can be asked
	 * for (LO_PRIV_LIMIT); -1 otherwise.
	 */
	int			nsort;			/* sort keys; 0: the walk never switches */
	AttrNumber *sortattnos;		/* [nsort] */
	AttrNumber *sortcols;		/* [nsort] the sort tuple's: 1 .. nsort */
	Oid		   *sortops;
	Oid		   *sortcolls;
	bool	   *sortnullsfirst;
	int64		bound;
	TupleDesc	sortdesc;		/* the keys, then the row (text) */
	TupleTableSlot *sortin;		/* a row into the sort (virtual) */
	TupleTableSlot *sortout;	/* ... and out of it (minimal) */
	bool		sorting;		/* switched: returning the sort's rows */
	MemoryContext sortcxt;		/* the sort and its scratch, under scancxt */
	Tuplesortstate *sortstate;
	char	   *sortitem;		/* scratch: a row's bytes, into the sort */
	Size		sortitemcap;
	HeapTuple	sortrow;		/* scratch: a row out of it */
	Size		sortrowcap;

	/*
	 * A parallel walk (DESIGN.md §40.9): the shared struct, once the Gather
	 * has set it up; whether the set is a view of its shared copy; and - in
	 * the leader - the workers' counters, taken from the sums as it shuts
	 * down, with how much of the sums it has taken already, and the set as
	 * its builder saw it, for a leader that never had one.
	 */
	bool		parallel;		/* the plan is parallel-aware */
	LoShared   *shared;
	bool		setview;
	bool		counted;		/* a worker's counters are in the sums */
	LoCounts	wsums;
	LoCounts	wseen;
	int			wncont;
	bool		wexact;
	Size		wspill;
	uint64		published;		/* sets this participant shared */
	uint64		attaches;		/* views this participant took of one */

	/*
	 * EXPLAIN ANALYZE.  `ran`: this process ran the node.  A serial node that
	 * did not, while its instrumentation says it ran, ran in a worker
	 * (§40.9), and its counters stayed there.
	 */
	bool		ran;
	uint64		walked;
	uint64		hits;
	uint64		fetched;
	uint64		removed;
	uint64		builds;
	int			ncont;
	uint64		scans;			/* walks started */
	uint64		switches;		/* scans that switched */
	uint64		sortfetched;	/* members fetched by the switch */
	bool		sorted;			/* sortstats are the last switch's */
	TuplesortInstrumentation sortstats;
	uint64		earlystops;		/* scans the early stop ended */
} LionOrderedState;

static Plan *lo_plan_path(PlannerInfo *root, RelOptInfo *rel,
						  CustomPath *best_path, List *tlist,
						  List *clauses, List *custom_plans);
static List *lo_reparameterize(PlannerInfo *root, List *custom_private,
							   RelOptInfo *child_rel);
static Node *lo_create_state(CustomScan *cscan);
static void lo_begin(CustomScanState *node, EState *estate, int eflags);
static TupleTableSlot *lo_exec(CustomScanState *node);
static void lo_end(CustomScanState *node);
static void lo_rescan(CustomScanState *node);
static void lo_scan_reset(LionOrderedState *st);
static bool lo_mark_place(LionOrderedState *st, int idx, uint32 place);
static void lo_explain(CustomScanState *node, List *ancestors,
					   ExplainState *es);
static Size lo_estimate_dsm(CustomScanState *node, ParallelContext *pcxt);
static void lo_initialize_dsm(CustomScanState *node, ParallelContext *pcxt,
							  void *coordinate);
static void lo_reinitialize_dsm(CustomScanState *node, ParallelContext *pcxt,
								void *coordinate);
static void lo_initialize_worker(CustomScanState *node, shm_toc *toc,
								 void *coordinate);
static void lo_shutdown(CustomScanState *node);

const CustomPathMethods lion_ordered_path_methods = {
	.CustomName = "LionOrdered",
	.PlanCustomPath = lo_plan_path,
};

static const CustomScanMethods lo_scan_methods = {
	.CustomName = "LionOrdered",
	.CreateCustomScanState = lo_create_state,
};

/*
 * A btree's walk that claims no order (DESIGN.md §40): the same plan and the
 * same executor under a name that does not promise one.  It alone is
 * parameterized (§40.10), and a partitionwise join translates its
 * parameterization from the outer side's parent to the partition joined
 * (lo_reparameterize()).
 */
const CustomPathMethods lion_btree_path_methods = {
	.CustomName = "LionBtreeScan",
	.PlanCustomPath = lo_plan_path,
	.ReparameterizeCustomPathByChild = lo_reparameterize,
};

static const CustomScanMethods lo_btree_scan_methods = {
	.CustomName = "LionBtreeScan",
	.CreateCustomScanState = lo_create_state,
};

static const CustomExecMethods lo_exec_methods = {
	.CustomName = "LionOrdered",
	.BeginCustomScan = lo_begin,
	.ExecCustomScan = lo_exec,
	.EndCustomScan = lo_end,
	.ReScanCustomScan = lo_rescan,
	.EstimateDSMCustomScan = lo_estimate_dsm,
	.InitializeDSMCustomScan = lo_initialize_dsm,
	.ReInitializeDSMCustomScan = lo_reinitialize_dsm,
	.InitializeWorkerCustomScan = lo_initialize_worker,
	.ShutdownCustomScan = lo_shutdown,
	.ExplainCustomScan = lo_explain,
};

/* ---------------------------------------------------------------------
 * Planner
 * --------------------------------------------------------------------- */

/*
 * Can the executor turn this index qual into a scan key by substituting an
 * INDEX_VAR Var for its key operand (lo_fix_qual())?  Core's index quals are
 * `key op value`, `key op ANY (array)`, `key IS [NOT] NULL` or a row
 * comparison; the last is not taken in v1 (DESIGN.md §30.8).
 */
static bool
lo_qual_ok(Node *clause)
{
	if (IsA(clause, OpExpr))
		return list_length(((OpExpr *) clause)->args) == 2;
	if (IsA(clause, ScalarArrayOpExpr))
		return true;
	if (IsA(clause, NullTest))
		return true;
	return false;
}

/* Every index qual of an IndexPath is one lo_qual_ok() takes. */
static bool
lo_indexpath_ok(IndexPath *ipath)
{
	ListCell   *lc;

	foreach(lc, ipath->indexclauses)
	{
		IndexClause *iclause = lfirst_node(IndexClause, lc);
		ListCell   *lc2;

		if (iclause->indexcols != NIL)
			return false;
		foreach(lc2, iclause->indexquals)
		{
			RestrictInfo *rinfo = lfirst_node(RestrictInfo, lc2);

			if (!lo_qual_ok((Node *) rinfo->clause))
				return false;
		}
	}
	return true;
}

/*
 * A lion access the node can build a set from: a tree of BitmapAnd/BitmapOr
 * over lion IndexPaths, each with index clauses the executor can turn into
 * scan keys - or none at all, for a partial index whose predicate the query
 * implies (the source then walks the whole index, §29.3).
 */
static bool
lo_lion_tree_ok(Path *path, Oid lionam)
{
	ListCell   *lc;

	if (IsA(path, IndexPath))
	{
		IndexPath  *ipath = (IndexPath *) path;

		if (ipath->indexinfo->relam != lionam || ipath->indexinfo->hypothetical)
			return false;
		if (ipath->path.param_info != NULL || ipath->indexorderbys != NIL)
			return false;
		if (ipath->indexclauses == NIL && ipath->indexinfo->indpred == NIL)
			return false;
		return lo_indexpath_ok(ipath);
	}
	if (IsA(path, BitmapAndPath))
	{
		foreach(lc, ((BitmapAndPath *) path)->bitmapquals)
			if (!lo_lion_tree_ok((Path *) lfirst(lc), lionam))
				return false;
		return true;
	}
	if (IsA(path, BitmapOrPath))
	{
		foreach(lc, ((BitmapOrPath *) path)->bitmapquals)
			if (!lo_lion_tree_ok((Path *) lfirst(lc), lionam))
				return false;
		return true;
	}
	return false;
}

/*
 * The lion side's ORIGINAL qual, as create_bitmap_subplan() computes its
 * `qual`: an IndexPath's clauses, plus its index predicate where they do not
 * imply it; a BitmapAnd's children's, concatenated; a BitmapOr's as one OR
 * (or nothing, when an arm has none).  Also collects the RestrictInfos of the
 * index clauses of the leaves reached from the root through ANDs only - the
 * clauses the lion qual implies by construction; never an OR arm's.
 */
static List *
lo_lion_qual(Path *path, List **rinfos, bool *lossy)
{
	List	   *qual = NIL;
	ListCell   *lc;

	if (IsA(path, IndexPath))
	{
		IndexPath  *ipath = (IndexPath *) path;

		foreach(lc, ipath->indexclauses)
		{
			IndexClause *iclause = lfirst_node(IndexClause, lc);

			qual = lappend(qual, iclause->rinfo->clause);
			*rinfos = lappend(*rinfos, iclause->rinfo);
			if (iclause->lossy)
				*lossy = true;
		}
		foreach(lc, ipath->indexinfo->indpred)
		{
			Expr	   *pred = (Expr *) lfirst(lc);

			if (!predicate_implied_by(list_make1(pred), qual, false))
				qual = lappend(qual, pred);
		}
		return qual;
	}
	if (IsA(path, BitmapAndPath))
	{
		foreach(lc, ((BitmapAndPath *) path)->bitmapquals)
			qual = list_concat_unique(qual,
									  lo_lion_qual((Path *) lfirst(lc),
												   rinfos, lossy));
		return qual;
	}
	if (IsA(path, BitmapOrPath))
	{
		List	   *arms = NIL;
		bool		consttrue = false;

		foreach(lc, ((BitmapOrPath *) path)->bitmapquals)
		{
			/*
			 * An arm's own clauses are NOT implied by the OR: core builds each
			 * arm with the other top-level clauses at hand, so an arm may
			 * reuse the RestrictInfo of `a = 3` from `a = 3 AND (b = 5 OR
			 * c = 7)`, and dropping `a = 3` from the filter because of it
			 * returned rows with a <> 3 (2026-09-25 review).  Only the
			 * leaves on AND paths from the root are collected; anything else
			 * leaves the filter only when predicate_implied_by() the whole
			 * lion qual, as in create_bitmap_scan_plan().
			 */
			List	   *armrinfos = NIL;
			List	   *sub = lo_lion_qual((Path *) lfirst(lc), &armrinfos, lossy);

			if (sub == NIL)
				consttrue = true;
			else
				arms = lappend(arms, make_ands_explicit(sub));
		}
		if (consttrue || arms == NIL)
			return NIL;
		if (list_length(arms) == 1)
			return list_make1(linitial(arms));
		return list_make1(make_orclause(arms));
	}
	elog(ERROR, "LionOrdered: unexpected lion path type %d", (int) nodeTag(path));
	return NIL;					/* keep compiler quiet */
}

/*
 * The lion tree for custom_private, in preorder, with every leaf's index and
 * its index quals (key on the left) and their index columns.
 */
static void
lo_lion_tree(Path *path, List **tree, List **leaves, List **quals)
{
	ListCell   *lc;

	if (IsA(path, IndexPath))
	{
		IndexPath  *ipath = (IndexPath *) path;
		List	   *node = list_make2_int(LO_NODE_LEAF, 0);
		int			n = 0;

		foreach(lc, ipath->indexclauses)
		{
			IndexClause *iclause = lfirst_node(IndexClause, lc);
			ListCell   *lc2;

			foreach(lc2, iclause->indexquals)
			{
				RestrictInfo *rinfo = lfirst_node(RestrictInfo, lc2);

				*quals = lappend(*quals, rinfo->clause);
				node = lappend_int(node, iclause->indexcol);
				n++;
			}
		}
		lsecond_int(node) = n;
		*tree = lappend(*tree, node);
		*leaves = lappend_oid(*leaves, ipath->indexinfo->indexoid);
		return;
	}
	else
	{
		List	   *children = IsA(path, BitmapAndPath) ?
			((BitmapAndPath *) path)->bitmapquals :
			castNode(BitmapOrPath, path)->bitmapquals;

		*tree = lappend(*tree,
						list_make2_int(IsA(path, BitmapAndPath) ?
									   LO_NODE_AND : LO_NODE_OR,
									   list_length(children)));
		foreach(lc, children)
			lo_lion_tree((Path *) lfirst(lc), tree, leaves, quals);
	}
}

/* The ordered index's non-lossy index clauses: what its walk answers. */
static List *
lo_ord_rinfos(IndexPath *ord)
{
	List	   *ordrinfos = NIL;
	ListCell   *lc;

	foreach(lc, ord->indexclauses)
	{
		IndexClause *iclause = lfirst_node(IndexClause, lc);

		if (!iclause->lossy)
			ordrinfos = lappend(ordrinfos, iclause->rinfo);
	}
	return ordrinfos;
}

/*
 * The same restriction clause?  The RestrictInfo itself, or one spelling the
 * same clause: 18's matching of `k = 1 OR k = 7` to an index makes a fresh
 * `k = ANY ('{1,7}')` RestrictInfo for EACH index it matches, so the btree's
 * and the lion index's clause for one OR are two objects (DESIGN.md §40.2).
 */
static bool
lo_same_clause(RestrictInfo *a, RestrictInfo *b)
{
	return a == b || equal(a->clause, b->clause);
}

/*
 * Does the ordered walk answer rinfo: is it one of the clauses the walk
 * answers - the RestrictInfo itself, one spelling the same clause, or a join
 * clause derived from the same equivalence class as one of them, which
 * create_indexscan_plan() takes as answered too
 * (is_redundant_with_indexclauses()): a nested loop's probe (DESIGN.md
 * §40.10) is handed its parameterization's join clauses beside the btree's
 * own, and an equivalence class makes the two separately.
 */
static bool
lo_ord_answers(RestrictInfo *rinfo, List *ordrinfos)
{
	ListCell   *lc;

	foreach(lc, ordrinfos)
	{
		RestrictInfo *other = lfirst_node(RestrictInfo, lc);

		if (lo_same_clause(rinfo, other) ||
			(rinfo->parent_ec != NULL && rinfo->parent_ec == other->parent_ec))
			return true;
	}
	return false;
}

/*
 * Which restriction clauses the node must still evaluate on a fetched row
 * (DESIGN.md §30.2, step 4): not the ones the ordered walk answers (the
 * ordered index's non-lossy index clauses, or the range a lion column's walk
 * is bounded by: lo_ord_answers()), not the lion leaves' index clauses, not
 * what the lion qual implies.  A nested loop's probe passes its
 * parameterization's join clauses among rinfos (§40.10).
 */
static List *
lo_residual(List *rinfos, List *ordrinfos, List *lionrinfos, List *lionqual)
{
	List	   *result = NIL;
	ListCell   *lc;

	foreach(lc, rinfos)
	{
		RestrictInfo *rinfo = lfirst_node(RestrictInfo, lc);

		if (rinfo->pseudoconstant)
			continue;			/* core's gating Result evaluates it */
		if (lo_ord_answers(rinfo, ordrinfos) ||
			list_member_ptr(lionrinfos, rinfo))
			continue;
		if (lionqual != NIL &&
			predicate_implied_by(list_make1(rinfo->clause), lionqual, false))
			continue;
		result = lappend(result, rinfo);
	}
	return result;
}

/*
 * The clauses of a lion access's qual that a partial index's predicate
 * implies - core's own test for which restriction clauses a partial index
 * answers by itself (check_index_predicates()), asked of the lion side: an
 * index clause of a lion leaf, or a partial lion index's own predicate.  A
 * walk of that index meets only rows that satisfy them, so among the rows it
 * walks they select nothing.
 */
static List *
lo_pred_implied(IndexOptInfo *idx, List *lionqual)
{
	List	   *result = NIL;
	ListCell   *lc;

	if (idx->indpred == NIL)
		return NIL;
	foreach(lc, lionqual)
	{
		Expr	   *clause = (Expr *) lfirst(lc);

		if (!contain_mutable_functions((Node *) clause) &&
			predicate_implied_by(list_make1(clause), idx->indpred, false))
			result = lappend(result, clause);
	}
	return result;
}

/*
 * The columns the node must produce for one (ordered path, lion access) pair
 * (DESIGN.md §40.1), offset as pull_varattnos() offsets them: every Var of
 * rel in its target; in the residual filter; in the ordered index's own
 * clauses (the recheck under xs_recheck, EvalPlanQual's); and in the lion
 * qual when a lion index clause is lossy at plan time - every member is then
 * rechecked, and a walk that read the heap for each would be heap mode under
 * another name.  A lion qual that is not lossy is rechecked only when its set
 * turns out inexact at run time, and then from the heap tuple
 * where the btree lacks its columns (§40.4), so its Vars are not required.  A
 * system column (ctid, which a row mark adds) or a whole-row Var is among
 * the Vars as one no index returns.
 */
static Bitmapset *
lo_needed_attrs(RelOptInfo *rel, List *residual, List *ordclauses,
				List *lionqual, bool lossy)
{
	Bitmapset  *attrs = NULL;
	ListCell   *lc;

	pull_varattnos((Node *) rel->reltarget->exprs, rel->relid, &attrs);
	foreach(lc, residual)
		pull_varattnos((Node *) lfirst_node(RestrictInfo, lc)->clause,
					   rel->relid, &attrs);
	foreach(lc, ordclauses)
		pull_varattnos((Node *) lfirst_node(IndexClause, lc)->rinfo->clause,
					   rel->relid, &attrs);
	if (lossy)
		pull_varattnos((Node *) lionqual, rel->relid, &attrs);
	return attrs;
}

/*
 * The relation columns a btree returns (DESIGN.md §40.1), offset as
 * pull_varattnos() offsets them: a plain column the AM can return; an
 * expression column matches no Var (it may still give the order, as today,
 * but no value).  *cols is the relation attno each index column returns, 0
 * for none - the mapping the executor fills the scan tuple through.
 */
static Bitmapset *
lo_returned_attrs(IndexOptInfo *idx, List **cols)
{
	Bitmapset  *returned = NULL;
	int			i;

	*cols = NIL;
	for (i = 0; i < idx->ncolumns; i++)
	{
		int			attno = idx->indexkeys[i];

		if (attno > 0 && idx->canreturn[i])
			returned = bms_add_member(returned,
									  attno - FirstLowInvalidHeapAttributeNumber);
		else
			attno = 0;
		*cols = lappend_int(*cols, attno);
	}
	return returned;
}

/*
 * Does the btree return every column in `needed` (DESIGN.md §40.1)?  When it
 * does, *iocols is the mapping of lo_returned_attrs().
 */
static bool
lo_index_only(IndexOptInfo *idx, Bitmapset *needed, List **iocols)
{
	List	   *cols;

	if (!enable_indexonlyscan)
		return false;
	if (!bms_is_subset(needed, lo_returned_attrs(idx, &cols)))
		return false;
	*iocols = cols;
	return true;
}

/*
 * Does the lion access answer a clause the btree's own index clauses do not
 * (DESIGN.md §40.2)?  One whose every leaf's clauses - an OR arm's too - are
 * among the btree's adds nothing to its walk: every entry walked is a member.
 * A leaf with no clauses is a partial lion index walked whole, whose
 * predicate is what it selects by: it adds.
 */
static bool
lo_lion_adds(Path *lion, List *ordclauses)
{
	ListCell   *lc;

	if (IsA(lion, IndexPath))
	{
		if (((IndexPath *) lion)->indexclauses == NIL)
			return true;
		foreach(lc, ((IndexPath *) lion)->indexclauses)
		{
			IndexClause *iclause = lfirst_node(IndexClause, lc);
			ListCell   *lc2;
			bool		found = false;

			foreach(lc2, ordclauses)
				if (lo_same_clause(lfirst_node(IndexClause, lc2)->rinfo,
								   iclause->rinfo))
					found = true;
			if (!found)
				return true;
		}
		return false;
	}
	foreach(lc, IsA(lion, BitmapAndPath) ?
			((BitmapAndPath *) lion)->bitmapquals :
			castNode(BitmapOrPath, lion)->bitmapquals)
	{
		if (lo_lion_adds((Path *) lfirst(lc), ordclauses))
			return true;
	}
	return false;
}

/* The entries the leaves of a lion access read (lion_cost_path_entries()). */
static double
lo_lion_entries(PlannerInfo *root, Path *lion)
{
	double		entries = 0.0;
	ListCell   *lc;

	if (IsA(lion, IndexPath))
		return lion_cost_path_entries(root, (IndexPath *) lion);
	foreach(lc, IsA(lion, BitmapAndPath) ?
			((BitmapAndPath *) lion)->bitmapquals :
			castNode(BitmapOrPath, lion)->bitmapquals)
		entries += lo_lion_entries(root, (Path *) lfirst(lc));
	return entries;
}

/*
 * What taking the lion side's answer into the set costs, beyond its
 * lookups and a cpu_operator_cost a container key (DESIGN.md §30.3, §44.7):
 * each leaf's source hands the set a container for each entry it reads at
 * each container key that entry's rows reach, and each is gathered into its
 * key's (lo_gather_push()).  A range over a unique column brings about a
 * piece a member: its summaries' buckets each hold rows from nearly every
 * key of the heap.  So the pieces are min(members, entries x keys) - a 10%
 * equality on a column the heap is ordered by brings a piece a key, the
 * same filter as a range over a unique column one a member - each at
 * LO_PIECE_TUPLES cpu_tuple_costs.  Before 2026-10-18 nothing was charged
 * for them, and a 10% range on a unique double with summaries at 50M rows,
 * whose set of 5M pieces took 845 ms, was priced as its 1,220 summary
 * entries and 260,417 keys alone: 650 for 845 ms.  Then LO_PROBE_TUPLES,
 * the walk's 170 ns an entry, which those pieces came to (at 5M rows the
 * same filter's 500,000 members, 1.36 a piece, took 44 to 48 ms, 90 ns a
 * member); gathered (2026-10-19, LoGather), the 2M emulation's pieces went
 * from 278 ns each to 105, so a piece is now one cpu_tuple_cost, 85 ns on
 * the walk's scale, a little under the measurement, which is where a tie
 * should fall (the set's answer does not move with the plan).  To be
 * measured again at 50M rows.
 */
#define LO_PIECE_TUPLES		1

static Cost
lo_set_pieces(PlannerInfo *root, Path *lion, double members, double ncont)
{
	double		pieces = Min(members, lo_lion_entries(root, lion) * ncont);

	return pieces * LO_PIECE_TUPLES * cpu_tuple_cost;
}

/*
 * The bytes the set is expected to take (DESIGN.md §30.3): a directory
 * entry and a container header a key, and two bytes a member up to a
 * BITSET a key; and the walk's visited bits, a bit a place, counted as a
 * bit a member - an ARRAY's or a RUN's places are its members, a bitmap's
 * its bits, as many bytes as the bitmap itself takes.
 */
static double
lo_set_bytes(double members, double ncont)
{
	return ncont * (LION_CONTAINER_HDRSZ + LO_ENTRY_BYTES) +
		Min(members * sizeof(uint16), ncont * LION_BITSET_BYTES) +
		members / 8.0;
}

/*
 * What a set past hash_mem (get_hash_memory_limit()) costs beyond its build
 * (§30.4, "Spilling"): the bytes past it go to a temporary file, each page
 * of it written as zeros when the file is extended, faulted in and written
 * again as the set's bytes land in it.  Measured on the 2M emulation at a
 * 64 kB hash_mem against the default (2026-10-19, the least of 10 runs,
 * twice): the 10% range's set, 1.3 MB spilled, 20.9 and 21.0 ms against
 * 20.1 and 20.6; the 30% range's, 3.1 MB, 84.3 and 89.5 against 82.9 and
 * 86.9; the 90% range's, 6.2 MB, 259.7 and 244.6 against 253.0 and 248.7 -
 * about 5 us a page of BLCKSZ where the runs differ at all, 60
 * cpu_tuple_cost on the walk's scale (LO_PIECE_TUPLES), and that is the
 * price: 0.00015 a member, next to the piece's 0.01.
 */
#define LO_SPILL_PAGE_TUPLES	60

static Cost
lo_spill_cost(double setbytes)
{
	double		limit = (double) get_hash_memory_limit();

	if (setbytes <= limit)
		return 0.0;
	return ceil((setbytes - limit) / BLCKSZ) * LO_SPILL_PAGE_TUPLES *
		cpu_tuple_cost;
}

/*
 * The quals of a lion access its set leaves to the recheck: what the plain
 * scan's source of each of its IndexPaths leaves (lion_path_rechecked_quals(),
 * lion_amcost.c), through a BitmapAnd.  A BitmapOr's arms are not followed:
 * what one of them leaves is no share of the OR's rows.
 */
static List *
lo_rechecked_quals(PlannerInfo *root, Path *lion)
{
	List	   *quals = NIL;
	ListCell   *lc;

	if (IsA(lion, IndexPath))
		return lion_path_rechecked_quals(root, (IndexPath *) lion);
	if (IsA(lion, BitmapAndPath))
	{
		foreach(lc, ((BitmapAndPath *) lion)->bitmapquals)
			quals = list_concat(quals,
								lo_rechecked_quals(root, (Path *) lfirst(lc)));
	}
	return quals;
}

/*
 * What §30.3's price of the lookups charges a nested loop's probe for beyond
 * what its set's build does (DESIGN.md §40.10, "Cost"): in TIDs, each a
 * cpu_index_tuple_cost and the bitmap's tenth of a cpu_operator_cost.
 *
 * The lion access is priced as the bitmap paths it is (cost_bitmap_tree_node(),
 * lioncostestimate()), a TID each, which is what a lion index's scan hands a
 * bitmap.  The set's build hands over nothing a TID at a time: each leaf's
 * posting sets are read and combined a container at a time (lo_build_leaf(),
 * lo_set_combine()), a dense container's members costing nothing apiece.
 * The probe's start-up is that build alone, so its price decides the plan:
 * a set of four dense filters' leaves on a 6M-row dimension of 4,688
 * containers, priced 88,000, built in 57 ms; on 50M rows of 12,808
 * containers, priced 715,000, in 80 ms - 1.5 and 0.8 us a container of a
 * posting set, against the 10 ns a TID stands for.  So a leaf is charged
 * at most LO_BUILD_PIECE_TIDS TIDs a container of each entry it reads -
 * 0.8 us a piece at the reference's 500 units a millisecond, the lower of
 * the two, where a tie should fall - and its TIDs past that are taken back
 * out.  A sparse leaf keeps its price.  The other walks keep §30.3's price:
 * their walk, not the build, is most of theirs - but for a LionOrderedJoin's
 * sides (§47.3), under a LIMIT that scales their walks and not their builds.
 */
#define LO_BUILD_PIECE_TIDS		80

static double
lo_build_excess(PlannerInfo *root, Path *lion, double tuples, double pages)
{
	double		excess = 0.0;
	ListCell   *lc;

	if (IsA(lion, IndexPath))
	{
		IndexPath  *ipath = (IndexPath *) lion;
		double		tids = clamp_row_est(ipath->indexselectivity * tuples);
		double		pieces = lion_cost_path_entries(root, ipath) *
			Min(ceil(pages / LION_BLOCKS_PER_CONTAINER), tids);

		return Max(tids - LO_BUILD_PIECE_TIDS * pieces, 0.0);
	}
	foreach(lc, IsA(lion, BitmapAndPath) ?
			((BitmapAndPath *) lion)->bitmapquals :
			castNode(BitmapOrPath, lion)->bitmapquals)
		excess += lo_build_excess(root, (Path *) lfirst(lc), tuples, pages);
	return excess;
}

/*
 * What the lion access's lookups cost less, a call, when a correlated
 * subquery's walk rebuilds its set at every call (DESIGN.md §40.11): its
 * pages spread over the `loops` calls, as lioncostestimate() spreads a
 * parameterized scan's.  A leaf whose quals hold the outer query's Param was
 * priced so already (lioncostestimate()); the others are priced again at the
 * loop count, and the difference, at most nothing, is returned.  The CPU of
 * the lookups stays the call's.
 */
static Cost
lo_lion_amortized(PlannerInfo *root, Path *lion, double loops)
{
	Cost		less = 0.0;
	ListCell   *lc;

	if (IsA(lion, IndexPath))
	{
		IndexPath  *ipath = (IndexPath *) lion;
		Cost		startup;
		Cost		total;
		Selectivity sel;
		double		corr;
		double		pages;

		if (lion_outer_param_loops(root,
								   (Node *) ipath->indexclauses) > 1.0)
			return 0.0;
		ipath->indexinfo->amcostestimate(root, ipath, loops, &startup, &total,
										 &sel, &corr, &pages);
		return Min(total - ipath->indextotalcost, 0.0);
	}
	foreach(lc, IsA(lion, BitmapAndPath) ?
			((BitmapAndPath *) lion)->bitmapquals :
			castNode(BitmapOrPath, lion)->bitmapquals)
		less += lo_lion_amortized(root, (Path *) lfirst(lc), loops);
	return less;
}

/*
 * The pages a lion access's set reads where it is built: each leaf index's
 * selectivity's share of its pages, a page at least - a lion index's posting
 * sets hold their rows in proportion - as the competing bitmap heap scan's
 * bitmap reads the same leaves (lion_bitmap_cold(), DESIGN.md §39).  None
 * with no lion access, which builds no set (lo_cost()).
 */
static double
lo_lion_pages(Path *lion)
{
	double		pages = 0.0;
	List	   *quals = NIL;
	ListCell   *lc;

	check_stack_depth();
	if (lion == NULL)
		return 0.0;
	if (IsA(lion, IndexPath))
	{
		IndexPath  *ip = (IndexPath *) lion;
		double		ipages = Max((double) ip->indexinfo->pages, 1.0);

		return Min(Max(ceil(ip->indexselectivity * ipages), 1.0), ipages);
	}
	if (IsA(lion, BitmapAndPath))
		quals = ((BitmapAndPath *) lion)->bitmapquals;
	else if (IsA(lion, BitmapOrPath))
		quals = ((BitmapOrPath *) lion)->bitmapquals;
	foreach(lc, quals)
		pages += lo_lion_pages((Path *) lfirst(lc));
	return pages;
}

/*
 * The price of one (ordered path, lion access) pair (DESIGN.md §30.3), and the
 * size its set is expected to have.  rows is what the node returns, as lion's
 * estimates see it (lion_probe_rel_rows()).  In index-only mode (§40.3) the
 * members' heap I/O is only the share of pages not all-visible.
 *
 * A qual the set leaves to the recheck (lo_rechecked_quals()) makes it hold
 * more than the members: the CANDIDATES, the rows of the quals it answers,
 * each of which the walk meets is rechecked - from the heap tuple in heap
 * mode, where every one is fetched, and in index-only mode when the btree
 * does not return a column the lion qual reads (lo_plan_path()'s
 * LO_FLAG_HEAPRECHECK), where every one is fetched too, all-visible or not:
 * the recheck reads the tuple before the visibility map is asked.  Priced as
 * the members it was, such a set chose an index-only walk that fetched a
 * million heap rows to throw half of them away (§40.8).
 *
 * With `workers` not NULL the price is a PARTIAL path's (DESIGN.md §40.9),
 * as cost_index() prices a parallel index scan: *workers is what
 * compute_parallel_worker() gives the btree's pages - and, in heap mode, the
 * heap pages the members' fetches read, as core counts them for a parallel
 * Index Scan; an index-only walk's are left out, as core leaves an Index Only
 * Scan's out - and the per-row CPU, the probe of each entry walked among it,
 * is divided among the participants.  The lion lookups and the set, built
 * once for all of them, the btree's own cost and the heap's pages are each
 * participant's whole, as core leaves the index and the heap undivided.
 * Returns false when no worker would be given.
 *
 * With `loops` above 1 the price is a nested loop's probe of rel (DESIGN.md
 * §40.10), ord parameterized by the outer side: one scan of `loops`, whose
 * walk, probes and rows are its own (ord's selectivity and rows are a
 * scan's), whose heap pages are amortized over the loops as cost_index()
 * amortizes a parameterized scan's, and which pays its share of the set,
 * built once for every scan the loop makes: the build divided by `scans`.
 *
 * In a correlated subquery, whose quals hold an outer query's Param, the price
 * is a call's of the loop the subquery is (DESIGN.md §40.11), over the calls
 * lion_outer_param_loops() assumes: the walk's descents and the B-tree's pages
 * spread over them as core spreads a parameterized scan's, and the set's build
 * too where no Param reaches the lion quals - it is built once and kept for
 * every call (lo_rescan()) - or, where one does, its lookups' pages alone,
 * the set being built again at every call.  The heap's pages are a call's, as
 * core prices the subquery's own scans.
 *
 * With `probebuild` the set's lookups are priced as its build reads them,
 * lo_build_excess() taken off: a nested loop's probe's (§40.10), whose
 * callers pass ord's param_info test, and a LionOrderedJoin's side's (§47.3),
 * whose LIMIT scales the walk but not the build.  A correlated subquery's
 * calls (more than one, as above) are priced so too: lo_cost() finds them
 * itself, as it does for the rest of their price.  Otherwise §30.3's price of
 * a walk, the lookups a bitmap scan's.
 *
 * With `lion` NULL there is no set: each entry walked is only tested in the
 * visibility map, and the heap read for the share of pages not all-visible -
 * a LionProbeJoin's descent where the dimension's WHERE is empty (§46).
 *
 * With `reads_p` not NULL, the pages the walk reads go there, which a bitmap
 * heap scan's cold reads are weighed against (DESIGN.md §39, "A bitmap heap
 * scan's cold reads"): the btree's, as its amcostestimate() counts them, the
 * set's build reads (lo_lion_pages()), and the heap pages its fetches read,
 * as priced below.  With `fetches_p` not NULL, the heap fetches of the whole
 * walk go there, each a page to read: every candidate in heap mode or where
 * the recheck reads the tuple, otherwise the members on pages the visibility
 * map does not call all-visible (a LionOrderedJoin's cold reads, §47.3).
 */
static bool
lo_cost(PlannerInfo *root, RelOptInfo *rel, IndexPath *ord, Path *lion,
		List *lionrinfos, List *lionqual, List *residual, double rows,
		bool indexonly, double loops, double scans, bool probebuild,
		int *workers, Cost *startup_p, Cost *total_p, double *reads_p,
		double *fetches_p)
{
	Cost		lioncost;
	Selectivity sel;
	Selectivity selwalk;
	List	   *shared = NIL;
	ListCell   *lc;
	double		tuples = Max(rel->tuples, 1.0);
	double		pages = Max((double) rel->pages, 1.0);
	double		members;
	double		ncont;
	double		walked;
	double		fetched;
	double		candidates;
	double		heapfetched;
	Selectivity candsel;
	List	   *rechecked;
	QualCost	rqcost;
	bool		heaprecheck = false;
	double		spc_random;
	double		spc_seq;
	double		max_io;
	double		min_io;
	double		pages_corr;
	double		corr = 0.0;
	double		ipages = 0.0;
	double		calls;
	bool		rebuilt = false;
	double		skew;
	double		skewscale = 1.0;
	QualCost	qcost;
	Cost		startup;
	Cost		run;
	Cost		cpu;
	Cost		ordtotal = ord->indextotalcost;

	/*
	 * No lion access at all is LionProbeJoin's descent alone (DESIGN.md
	 * §46): every entry walked a member, and no set to build.
	 */
	lioncost = 0;
	sel = 1.0;
	if (lion != NULL)
		cost_bitmap_tree_node(lion, &lioncost, &sel);

	/* the calls of a correlated subquery, each walk a probe (§40.11) */
	calls = lion_outer_param_loops(root,
								   (Node *) list_make3(ord->indexclauses,
													   lionqual, residual));
	if (calls > 1.0)
	{
		rebuilt = lion_has_outer_param(root, (Node *) lionqual);
		if (rebuilt)
			lioncost = Max(lioncost + lo_lion_amortized(root, lion, calls),
						   0.0);
	}

	/*
	 * A probe's, a join's side's or a correlated subquery's call's: its
	 * lookups as its set's build reads them.
	 */
	if (lion != NULL && (probebuild || calls > 1.0))
		lioncost = Max(lioncost -
					   lo_build_excess(root, lion, tuples, pages) *
					   (cpu_index_tuple_cost + 0.1 * cpu_operator_cost),
					   0.0);

	/* the set's members: the candidates, when it leaves quals to the recheck */
	candsel = sel;
	rechecked = (lion != NULL) ? lo_rechecked_quals(root, lion) : NIL;
	rqcost.startup = 0;
	rqcost.per_tuple = 0;
	if (rechecked != NIL)
	{
		Selectivity rsel = clauselist_selectivity(root, rechecked, rel->relid,
												  JOIN_INNER, NULL);

		if (rsel > 0)
			candsel = Min(sel / rsel, 1.0);
		cost_qual_eval(&rqcost, lionqual, root);
		if (indexonly)
		{
			Bitmapset  *lionattrs = NULL;
			List	   *cols;

			pull_varattnos((Node *) lionqual, rel->relid, &lionattrs);
			heaprecheck = !bms_is_subset(lionattrs,
										 lo_returned_attrs(ord->indexinfo,
														   &cols));
		}
	}
	members = clamp_row_est(candsel * tuples);
	ncont = Min(ceil(pages / LION_BLOCKS_PER_CONTAINER), members);

	/*
	 * the lookups, the copy of their answer into the set, and its spill - a
	 * probe's share of them (§40.10)
	 */
	startup = (lion == NULL) ? 0.0 :
		lioncost + ncont * cpu_operator_cost +
		lo_set_pieces(root, lion, members, ncont) +
		lo_spill_cost(lo_set_bytes(members, ncont));
	if (calls > 1.0 && !rebuilt)
		scans *= calls;
	if (scans > 1.0)
		startup /= scans;

	/*
	 * the walk: the ordered index's own cost, and the probe of each entry -
	 * CPU, which a parallel walk divides
	 */
	walked = clamp_row_est(ord->indexselectivity * tuples);

	/*
	 * A key the plan does not know - the outer row's, a Param - on a skewed
	 * key column: the walk meets the rows a probed key meets (DESIGN.md §33,
	 * "The rows of a probed key"), and so do its members' tests and fetches
	 * below, where its competitor - the cheapest of core's paths of rel with
	 * the same parameterization - meets them too and is priced at them
	 * (lion_skew_rel_scale(), DESIGN.md §39, "A skewed key's probes"); the
	 * price is put back into core's units at the end by the competitor's two
	 * prices.  A competitor no key's rows change, a sequential scan, leaves
	 * the walk at core's rows too.
	 */
	skew = lion_skew_clauses_factor(root, rel, ord->indexclauses);
	if (skew > 1.0)
	{
		skewscale = lion_skew_rel_scale(root, rel,
										PATH_REQ_OUTER(&ord->path),
										(ord->path.param_info != NULL) ?
										loops : 1.0);
		if (skewscale < 1.0)
			walked = Min(clamp_row_est(walked * skew), tuples);
	}
	cpu = walked * (LO_PROBE_TUPLES * cpu_tuple_cost + cpu_operator_cost);

	/*
	 * The members' heap fetches, priced as cost_index() prices them: of the
	 * entries walked, the share the lion access selects BEYOND what the
	 * ordered index's own quals already did.  A clause both of them answer -
	 * `g = 5` over a btree on (g, k) and a lion index on g - was counted
	 * twice: the walk meets only g = 5 entries, every one a member, yet the
	 * fetches were priced at 1% of them, and the node (a plain index scan
	 * plus the lion lookups) beat a bitmap scan and Sort that the planner's
	 * own price for that index scan had rejected, 721 against 13,909 on 1M
	 * rows (2026-09-25 second review).  So is a clause the ordered index's
	 * PREDICATE implies: a partial btree on (k) WHERE s <> '' walks only
	 * rows with s <> '', and pricing the fetches at s <> ''s share of them
	 * chose the node over a bitmap scan it ran twice as slow as.
	 */
	foreach(lc, ord->indexclauses)
	{
		IndexClause *iclause = lfirst_node(IndexClause, lc);
		ListCell   *lc2;

		foreach(lc2, lionrinfos)
		{
			if (lo_same_clause(lfirst_node(RestrictInfo, lc2), iclause->rinfo))
			{
				shared = lappend(shared, iclause->rinfo);
				break;
			}
		}
	}
	foreach(lc, lo_pred_implied(ord->indexinfo, lionqual))
	{
		Expr	   *clause = (Expr *) lfirst(lc);
		ListCell   *lc2;
		bool		dup = false;

		foreach(lc2, shared)
		{
			Node	   *c = (Node *) lfirst(lc2);

			/* an index clause's RestrictInfo, or an implied bare clause */
			if (IsA(c, RestrictInfo))
				c = (Node *) ((RestrictInfo *) c)->clause;
			if (equal(c, clause))
				dup = true;
		}
		if (!dup)
			shared = lappend(shared, clause);
	}
	selwalk = sel;
	if (shared != NIL)
	{
		Selectivity both = clauselist_selectivity(root, shared, rel->relid,
												  JOIN_INNER, NULL);

		if (both > 0)
			selwalk = Min(sel / both, 1.0);
	}
	fetched = clamp_row_est(walked * selwalk);
	candidates = clamp_row_est(walked * Min(selwalk * candsel / sel, 1.0));
	heapfetched = (!indexonly || heaprecheck) ? candidates : fetched;
	get_tablespace_page_costs(rel->reltablespace, &spc_random, &spc_seq);
	{
		Cost		s;
		Cost		t;
		Selectivity isel;

		/*
		 * The B-tree's correlation and pages; and in a correlated subquery
		 * its own cost again, its pages spread over the calls as well as
		 * over a nested loop's scans (§40.11).
		 */
		ord->indexinfo->amcostestimate(root, ord,
									   (calls > 1.0) ?
									   calls * (ord->path.param_info != NULL ?
												loops : 1.0) : 1.0,
									   &s, &t, &isel, &corr, &ipages);
		if (calls > 1.0)
			ordtotal = Min(t, ordtotal);
	}
	run = ordtotal;
	pages_corr = ceil(Min(heapfetched, walked / tuples * pages));
	if (loops > 1.0)
	{
		/*
		 * A nested loop's probe (§40.10): the pages every scan of the loop
		 * fetches, through the Mackert and Lohman formula, as cost_index()
		 * counts a parameterized scan's - for the uncorrelated order and,
		 * scan by scan, for the correlated one - pro-rated by the loops
		 * below.
		 */
		max_io = index_pages_fetched(heapfetched * loops, rel->pages,
									 (double) ord->indexinfo->pages, root);
		pages_corr = index_pages_fetched(pages_corr * loops, rel->pages,
										 (double) ord->indexinfo->pages, root);
	}
	else
		max_io = index_pages_fetched(heapfetched, rel->pages,
									 (double) ord->indexinfo->pages, root);
	if (indexonly && !heaprecheck)
	{
		/*
		 * Only the pages the visibility map does not call all-visible are
		 * visited, as cost_index() prices an index-only scan's; each member
		 * pays the map's test (DESIGN.md §40.3).
		 */
		max_io = ceil(max_io * (1.0 - rel->allvisfrac));
		pages_corr = ceil(pages_corr * (1.0 - rel->allvisfrac));
	}

	/*
	 * A partial path's workers, as cost_index() gives a parallel index
	 * scan's: from the btree's pages, and the heap pages the fetches read
	 * at random unless the walk is index-only.
	 */
	if (workers != NULL)
	{
		*workers = compute_parallel_worker(rel,
										   (indexonly && !heaprecheck) ?
										   -1 : max_io,
										   ipages,
										   max_parallel_workers_per_gather);
		if (*workers <= 0)
			return false;
	}
	if (indexonly)
		cpu += fetched * cpu_operator_cost;
	if (reads_p != NULL)
		*reads_p = ipages + lo_lion_pages(lion) +
			Max(max_io + corr * corr * (pages_corr - max_io), 0.0);
	if (fetches_p != NULL)
		*fetches_p = (indexonly && !heaprecheck) ?
			heapfetched * (1.0 - rel->allvisfrac) : heapfetched;
	max_io *= spc_random;
	if (loops > 1.0)
	{
		/* every fetch random, as cost_index() takes a loop's, per scan */
		min_io = pages_corr * spc_random / loops;
		max_io /= loops;
	}
	else
		min_io = (pages_corr > 0) ?
			spc_random + (pages_corr - 1) * spc_seq : 0;
	run += max_io + corr * corr * (min_io - max_io);

	cost_qual_eval(&qcost, residual, root);
	startup += qcost.startup + rqcost.startup + rel->reltarget->cost.startup;
	cpu += heapfetched * cpu_tuple_cost +
		candidates * rqcost.per_tuple +
		fetched * qcost.per_tuple +
		rows * rel->reltarget->cost.per_tuple;
	if (workers != NULL)
		cpu /= lion_parallel_divisor(*workers);
	run += cpu;

	/*
	 * Priced at the rows a probed key meets, the walk is put back into core's
	 * units by what its competitor costs at core's rows over what it costs at
	 * those: the two compare as they would at the rows both meet, and the
	 * loop around them stays in core's units.
	 */
	startup *= skewscale;
	run *= skewscale;

	*startup_p = startup;
	*total_p = startup + run;
	return true;
}

/*
 * Is this a lionvec index's order by distance (DESIGN.md §44.6), which the
 * node can walk over a lion set: one ORDER BY clause, no index clause (the
 * index has no search operator), forward - the one direction its scan has.
 */
static bool
lo_knn_path(IndexPath *ipath)
{
	return lion_vec_index(ipath->indexinfo) &&
		list_length(ipath->indexorderbys) == 1 &&
		list_length(ipath->indexorderbycols) == 1 &&
		ipath->indexclauses == NIL &&
		!ScanDirectionIsBackward(ipath->indexscandir);
}

/*
 * The cost of the node over a lionvec index's order (DESIGN.md §44.6): the
 * set, built as lo_cost() prices it; the vector scan restricted to its
 * members, which before its first row reads plane 1 of the CODE pages that
 * hold one, or computes every member's distance (lion_vec_scan_cost()); and
 * then, for every member it returns, at worst a refine and the heap fetch
 * of its exact distance - or the exact path's check of a row it read by
 * place, or nothing for one it read from the heap, whose page it read
 * before the first row (`fetch`) - and the node's own fetch of that row,
 * from the page the scan has just read, its set probe and its quals.
 */
static void
lo_cost_knn(PlannerInfo *root, RelOptInfo *rel, IndexPath *ord, Path *lion,
			List *residual, double rows, Cost *startup_p, Cost *total_p)
{
	Cost		lioncost;
	Selectivity sel;
	double		tuples = Max(rel->tuples, 1.0);
	double		pages = Max((double) rel->pages, 1.0);
	double		members;
	double		ncont;
	Cost		fetch;
	QualCost	qcost;
	Cost		startup;
	Cost		run;

	cost_bitmap_tree_node(lion, &lioncost, &sel);
	members = clamp_row_est(sel * tuples);
	ncont = Min(ceil(pages / LION_BLOCKS_PER_CONTAINER), members);

	startup = lioncost + ncont * cpu_operator_cost +
		lo_set_pieces(root, lion, members, ncont) +
		lo_spill_cost(lo_set_bytes(members, ncont)) +
		lion_vec_scan_cost(root, ord, members, &fetch);

	run = members * (fetch + cpu_index_tuple_cost + cpu_operator_cost +
					 cpu_tuple_cost);

	cost_qual_eval(&qcost, residual, root);
	startup += qcost.startup + rel->reltarget->cost.startup;
	run += members * qcost.per_tuple + rows * rel->reltarget->cost.per_tuple;

	*startup_p = startup;
	*total_p = startup + run;
}

/*
 * Is rel one the node may scan at all (DESIGN.md §30.1)?  A plain table, or a
 * leaf partition reached through its parent (§30.11): the parent's Append or
 * MergeAppend is core's, over whatever path each partition has.
 */
static bool
lo_rel_ok(PlannerInfo *root, RelOptInfo *rel, RangeTblEntry *rte)
{
	ListCell   *lc;

	if ((rel->reloptkind != RELOPT_BASEREL &&
		 rel->reloptkind != RELOPT_OTHER_MEMBER_REL) ||
		rte->rtekind != RTE_RELATION)
		return false;
	if (rte->inh || rte->tablesample != NULL)
		return false;
	if (lion_rel_is_unexpanded_parent(rte))
		return false;
	if (rte->relkind != RELKIND_RELATION && rte->relkind != RELKIND_MATVIEW)
		return false;
	if (rte->securityQuals != NIL)
		return false;

	/*
	 * Proven empty already (a constant-false or NULL restriction, or
	 * constraint exclusion): core costs no index path for it, and neither
	 * does the node - the planner leaves such a relation's pages out of
	 * root->total_table_pages, which cost_index() asserts they are in.
	 */
	if (IS_DUMMY_REL(rel))
		return false;

	/*
	 * Nothing it needs comes from outside it: a relation with LATERAL
	 * references - a PlaceHolderVar it evaluates for a LATERAL subquery
	 * pulled up from under an outer join, `la LEFT JOIN LATERAL (SELECT
	 * lb.id, la.x FROM lb ...) ON true` - is scanned with a value of
	 * another relation each time, so every path of it is parameterized by
	 * at least those relations (get_baserel_parampathinfo() asserts it),
	 * and every path the node builds - its own, and the btree paths it
	 * builds as its ordered side - is unparameterized (§30.2).
	 */
	if (!bms_is_empty(rel->lateral_relids))
		return false;

	/*
	 * Not the target of an UPDATE, DELETE or MERGE (reached through a merge
	 * join's ordered input), nor a partition of one: v1 leaves those scans to
	 * core (DESIGN.md §30.8).
	 */
	if (root->parse->resultRelation == (int) rel->relid ||
		bms_is_member((int) rel->relid, root->all_result_relids))
		return false;
	foreach(lc, rel->baserestrictinfo)
	{
		RestrictInfo *rinfo = lfirst_node(RestrictInfo, lc);

		if (rinfo->security_level > 0)
			return false;
	}
	if (rel->indexlist == NIL)
		return false;
	return true;
}

/*
 * A scratch copy of rel that sees only `indexes`, with `exclude` taken out of
 * its restriction clauses, no paths, no join clauses and no parallelism, so
 * that create_index_paths() run on it builds only unparameterized, non-partial
 * paths - in the copy's own path list - and leaves rel untouched (DESIGN.md
 * §30.2).  Each index is copied too when there is an `exclude`, with it taken
 * out of the clauses the index may match, which is how a lion column's walk
 * leaves the range it is bounded by out of the set (§30.11).
 *
 * With `joins` the copy keeps rel's join clauses, so that the index paths
 * core would make for a nested loop's inner side are made too: a btree's,
 * for the probe of §40.10.  (An index that is not copied belongs to rel,
 * whose equivalence classes core matches it to whatever the copy says -
 * match_eclass_clauses_to_index() asks index->rel - so its paths
 * parameterized by those come out either way; every caller but that probe
 * passes over them.)
 */
static RelOptInfo *
lo_scratch_rel(RelOptInfo *rel, List *indexes, List *exclude, bool joins)
{
	RelOptInfo *scratch;
	ListCell   *lc;

	scratch = makeNode(RelOptInfo);
	memcpy(scratch, rel, sizeof(RelOptInfo));
	scratch->indexlist = NIL;
	foreach(lc, indexes)
	{
		IndexOptInfo *idx = (IndexOptInfo *) lfirst(lc);

		if (exclude != NIL)
		{
			IndexOptInfo *copy = makeNode(IndexOptInfo);

			memcpy(copy, idx, sizeof(IndexOptInfo));
			copy->rel = scratch;
			copy->indrestrictinfo = list_difference_ptr(idx->indrestrictinfo,
														exclude);
			idx = copy;
		}
		scratch->indexlist = lappend(scratch->indexlist, idx);
	}
	scratch->baserestrictinfo = list_difference_ptr(rel->baserestrictinfo,
													exclude);
	scratch->pathlist = NIL;
	scratch->ppilist = NIL;
	scratch->partial_pathlist = NIL;
	scratch->cheapest_startup_path = NULL;
	scratch->cheapest_total_path = NULL;
	scratch->cheapest_parameterized_paths = NIL;
	if (!joins)
	{
		scratch->joininfo = NIL;
		scratch->has_eclass_joins = false;
	}
	scratch->consider_parallel = false;
	return scratch;
}

/*
 * The lion accesses core builds for rel's restriction clauses less `exclude`:
 * create_index_paths() run on a scratch copy of rel that sees only its lion
 * indexes (DESIGN.md §30.2).
 */
static List *
lo_lion_accesses(PlannerInfo *root, RelOptInfo *rel, List *lionidx,
				 List *exclude, Oid lionam)
{
	RelOptInfo *scratch = lo_scratch_rel(rel, lionidx, exclude, false);
	List	   *cands = NIL;
	ListCell   *lc;

	if (scratch->baserestrictinfo == NIL)
		return NIL;
	create_index_paths(root, scratch);

#if PG_VERSION_NUM < 180000

	/*
	 * ... and the paths 18's matching builds for an OR of equalities, as the
	 * IN list it spells (lion_or_list_paths(), DESIGN.md §29.11): the lion
	 * side of `k = 1 OR k = 7` is then the lion side of `k IN (1, 7)`.
	 */
	scratch->pathlist = list_concat(scratch->pathlist,
									lion_or_list_paths(root, scratch,
													   scratch->indexlist));
#endif

	foreach(lc, scratch->pathlist)
	{
		Path	   *p = (Path *) lfirst(lc);
		Path	   *q = NULL;

		if (p->param_info != NULL)
			continue;
		if (IsA(p, BitmapHeapPath))
			q = ((BitmapHeapPath *) p)->bitmapqual;
		else if (IsA(p, IndexPath))
			q = p;
		if (q != NULL && lo_lion_tree_ok(q, lionam))
			cands = lappend(cands, q);
	}
	return cands;
}

/*
 * Do two lists of IndexClauses from one create_index_paths() run name the
 * same clauses at the same columns?  Its paths share the RestrictInfos;
 * equal() has no support for an IndexClause.
 */
static bool
lo_same_indexclauses(List *a, List *b)
{
	ListCell   *la;
	ListCell   *lb;

	if (list_length(a) != list_length(b))
		return false;
	forboth(la, a, lb, b)
	{
		IndexClause *ia = lfirst_node(IndexClause, la);
		IndexClause *ib = lfirst_node(IndexClause, lb);

		if (ia->rinfo != ib->rinfo || ia->indexcol != ib->indexcol)
			return false;
	}
	return true;
}

/*
 * The IndexPaths core builds for one btree of rel over the relation's
 * restriction clauses, whether or not it finds their order useful (DESIGN.md
 * §40.2): create_index_paths() on a scratch copy of rel that sees that index
 * alone, so that its paths compete with nothing else's - in the relation
 * itself the full walk of a covering index with no qual, which core builds as
 * an index-only scan, loses to the sequential scan and never reaches the
 * path list.  One add_path() still dropped for a bitmap scan of the same
 * index is taken from that scan's one-leaf bitmapqual.
 *
 * And the walk of the whole index when core built no unparameterized path of
 * it at all: it builds one without index clauses only when its order is
 * useful or the index alone answers the query - every column of the
 * restriction clauses among its own (check_index_only()) - and a btree that
 * lacks the lion filter's columns, a primary key under a filter on others, is
 * walked whole by the node all the same, the filter answered by the set and
 * the columns the node must produce decided per pair (§40.1).  A partial
 * btree only where the query implies its predicate, as core takes one.  Not
 * when core built a path of it that is none of these - a BitmapOr of its
 * scans for an OR of its column's equalities, on 16 and 17 - which reads its
 * matches alone.  (Until 2026-10-23 "no path at all": the paths core makes
 * for the key's equivalence class's joins, which the scratch copy never
 * stopped, then kept a primary key's whole walk out of every join query.)
 *
 * *params gets the btree's paths PARAMETERIZED by the outer side of a nested
 * loop (§40.10): the scratch copy keeps rel's join clauses for them, so they
 * are the ones core makes for the btree as an inner side - its join clauses
 * as index clauses beside the restriction clauses it matches - forward only
 * (the backward twin of a parameterized path walks the same entries), one a
 * parameterization and set of index clauses.
 */
static List *
lo_btree_paths(PlannerInfo *root, RelOptInfo *rel, IndexOptInfo *idx,
			   List **params)
{
	RelOptInfo *scratch = lo_scratch_rel(rel, list_make1(idx), NIL, true);
	List	   *result = NIL;
	bool		unparam = false;
	ListCell   *lc;

	create_index_paths(root, scratch);
	foreach(lc, scratch->pathlist)
	{
		Path	   *p = (Path *) lfirst(lc);
		IndexPath  *ipath;

		if (p->param_info == NULL)
			unparam = true;
		if (IsA(p, BitmapHeapPath))
			p = ((BitmapHeapPath *) p)->bitmapqual;
		if (!IsA(p, IndexPath))
			continue;
		ipath = (IndexPath *) p;
		if (ipath->indexinfo != idx || ipath->indexorderbys != NIL ||
			!lo_indexpath_ok(ipath))
			continue;
		if (p->param_info == NULL)
		{
			if (!list_member_ptr(result, ipath))
				result = lappend(result, ipath);
		}
		else if (!ScanDirectionIsBackward(ipath->indexscandir))
		{
			ListCell   *lc2;
			bool		dup = false;

			foreach(lc2, *params)
			{
				IndexPath  *other = (IndexPath *) lfirst(lc2);

				if (other == ipath ||
					(other->indexinfo == ipath->indexinfo &&
					 bms_equal(PATH_REQ_OUTER(&other->path),
							   PATH_REQ_OUTER(&ipath->path)) &&
					 lo_same_indexclauses(other->indexclauses,
										  ipath->indexclauses)))
					dup = true;
			}
			if (!dup)
				*params = lappend(*params, ipath);
		}
	}
	if (!unparam && (idx->indpred == NIL || idx->predOK))
		result = lappend(result,
						 create_index_path(root, idx, NIL, NIL, NIL, NIL,
										   ForwardScanDirection, true, NULL,
										   1.0, false));
	return result;
}

/*
 * A walk of one ordered scalar key column of a lion index in the column's
 * order, as the ordered side of the node (DESIGN.md §30.11).
 */
typedef struct LoWalk
{
	IndexOptInfo *index;
	int			indexcol;		/* 0-based */
	Var		   *var;			/* the column, as rel's Var */
	PathKey    *pathkey;		/* the order it gives: the query's first */
	bool		backward;
	int			nulls;			/* LION_ORDER_NULLS_* */
	List	   *rinfos;			/* the restriction clauses it answers */
	List	   *quals;			/* ... as index quals, the key on the left */
	double		nlist;			/* the values of a list among them, or 0 */
	List	   *vrinfos;		/* the clauses an entry's first visible row
								 * decides for it (lo_walk_value_rinfos()) */
} LoWalk;

/* The walk's custom_private marker (the btree kind's first member is a Path) */
#define LO_WALK_MAGIC		0x4c57414c	/* "LWAL" */

/*
 * Can the walk of `var` answer rinfo: a range comparison of the column - or a
 * `<>`, the hole the walk steps over (DESIGN.md §35) - with
 * a value that does not depend on the row - a Const, a Param, a stable
 * expression, evaluated when the walk starts as an Index Scan's run-time keys
 * are - under the index column's collation, `IS NOT NULL`, or a list, `col =
 * ANY (array)` of such an array?  *qual is the clause with the column on the
 * left, and *list says it is a list.
 *
 * A list is walked a value at a time, each value descended to (DESIGN.md
 * §30.11, "Lists"), which the walk can do only in the directory's own order:
 * so only under the column's own equality for its own type (the opclass's
 * input type), whose probe is the column's comparison.  Left to the set, a
 * list on the walked column was walked from the column's first entry
 * whatever its values, and `here IN ('H10', 'H11') ... ORDER BY here LIMIT
 * 50` read every entry below 'H10' before its first member - until the
 * fetch-and-sort switch fetched all 1,292 of them instead (2026-09-30).
 *
 * The collation is core's rule for an index clause, IndexCollMatchesExprColl()
 * (as lion_match_index() applies it): the walk compares its bound with the
 * directory's keys in the order they were stored in, the index column's
 * collation's, so a comparison under another collation is no bound of it.
 * Taken as one it left the filter too, and `t < 'a' COLLATE "C"` over an
 * "en-x-icu" column returned no rows at all, where 'A0' is below 'a' in "C"
 * but after it in English (2026-09-29 review).  Such a clause stays in the
 * filter, as any other the walk does not answer.
 */
static bool
lo_walk_clause(RestrictInfo *rinfo, Var *var, Oid opfamily, Oid opcintype,
			   Oid idxcoll, Expr **qual, bool *list)
{
	Expr	   *clause = rinfo->clause;

	*list = false;
	if (rinfo->pseudoconstant)
		return false;
	if (IsA(clause, ScalarArrayOpExpr))
	{
		ScalarArrayOpExpr *saop = (ScalarArrayOpExpr *) clause;
		Node	   *l;
		Node	   *arr;
		int			strategy;
		Oid			lefttype;
		Oid			righttype;

		if (!saop->useOr || list_length(saop->args) != 2)
			return false;
		l = (Node *) linitial(saop->args);
		arr = (Node *) lsecond(saop->args);
		while (IsA(l, RelabelType))
			l = (Node *) ((RelabelType *) l)->arg;
		if (!equal(l, var))
			return false;
		if (contain_var_clause(arr) || contain_volatile_functions(arr) ||
			contain_subplans(arr))
			return false;
		if (OidIsValid(idxcoll) && saop->inputcollid != idxcoll)
			return false;
		if (!op_in_opfamily(saop->opno, opfamily))
			return false;
		get_op_opfamily_properties(saop->opno, opfamily, false, &strategy,
								   &lefttype, &righttype);
		if (strategy != LION_STRAT_EQUAL || lefttype != opcintype ||
			righttype != opcintype)
			return false;
		*qual = clause;
		*list = true;
		return true;
	}
	if (IsA(clause, NullTest))
	{
		NullTest   *nt = (NullTest *) clause;
		Node	   *arg = (Node *) nt->arg;

		while (IsA(arg, RelabelType))
			arg = (Node *) ((RelabelType *) arg)->arg;
		if (nt->nulltesttype != IS_NOT_NULL || nt->argisrow ||
			!equal(arg, var))
			return false;
		*qual = clause;
		return true;
	}
	if (IsA(clause, OpExpr) && list_length(((OpExpr *) clause)->args) == 2)
	{
		OpExpr	   *op = (OpExpr *) clause;
		Node	   *left = (Node *) linitial(op->args);
		Node	   *right = (Node *) lsecond(op->args);
		Node	   *l = left;
		Node	   *r = right;
		Oid			opno = op->opno;
		int			strategy;

		while (IsA(l, RelabelType))
			l = (Node *) ((RelabelType *) l)->arg;
		while (IsA(r, RelabelType))
			r = (Node *) ((RelabelType *) r)->arg;
		if (!equal(l, var))
		{
			if (!equal(r, var))
				return false;
			opno = get_commutator(opno);
			if (!OidIsValid(opno))
				return false;
			right = left;
			left = (Node *) lsecond(op->args);
		}
		if (contain_var_clause(right) || contain_volatile_functions(right) ||
			contain_subplans(right))
			return false;
		if (OidIsValid(idxcoll) && op->inputcollid != idxcoll)
			return false;
		strategy = get_op_opfamily_strategy(opno, opfamily);
		if (!LION_STRAT_IS_WALK(strategy))
			return false;		/* a range, or `<>`'s hole (DESIGN.md §35) */
		if (opno == op->opno)
			*qual = clause;
		else
		{
			OpExpr	   *c = (OpExpr *) copyObject(op);

			c->opno = opno;
			c->opfuncid = InvalidOid;
			set_opfuncid(c);
			c->args = list_make2(left, right);
			*qual = (Expr *) c;
		}
		return true;
	}
	return false;
}

/*
 * The restriction clauses a lion column's walk can decide once per ENTRY
 * rather than once per row (DESIGN.md §30.11, "A filter on the walked
 * column"): those that read no column but the walked one and are immutable,
 * on an index whose stored keys are the rows' own values byte for byte
 * (lion_index_can_emit_value()).  Every row of an entry then holds one
 * value, so a clause is true of all of them or of none, and the first
 * visible row the walk fetches of an entry decides for the entry: one that
 * fails takes the rest of the entry with it, unread.  The clause is only
 * ever evaluated on a visible row, as the ordinary plan evaluates it - never
 * on a stored key alone, which may be a dead row's, and could make a function
 * fail that the query never calls on it (`1 / k` with k = 0 only in a
 * deleted row).  It stays in the node's filter as well.
 *
 * Immutable, not merely non-volatile: a stable function may look at how a
 * row stores its value rather than at the value - pg_column_size(),
 * pg_column_compression(), pg_column_toast_chunk_id() differ between two
 * rows of one value, one compressed or toasted and one not - and the first
 * row would then decide wrongly for the others.  An immutable function sees
 * the value only, and the value is one image for the whole entry.
 */
static List *
lo_walk_value_rinfos(RelOptInfo *rel, IndexOptInfo *idx, int c, Var *var,
					 List *walkrinfos)
{
	List	   *result = NIL;
	ListCell   *lc;

	if (!lion_index_can_emit_value(idx, (AttrNumber) (c + 1)))
		return NIL;
	foreach(lc, rel->baserestrictinfo)
	{
		RestrictInfo *rinfo = lfirst_node(RestrictInfo, lc);
		Bitmapset  *attrs = NULL;

		if (rinfo->pseudoconstant || list_member_ptr(walkrinfos, rinfo) ||
			!bms_equal(rinfo->clause_relids, rel->relids))
			continue;
		if (contain_mutable_functions((Node *) rinfo->clause) ||
			contain_subplans((Node *) rinfo->clause))
			continue;
		pull_varattnos((Node *) rinfo->clause, rel->relid, &attrs);
		if (bms_num_members(attrs) != 1 ||
			!bms_is_member(var->varattno - FirstLowInvalidHeapAttributeNumber,
						   attrs))
			continue;
		result = lappend(result, rinfo);
	}
	return result;
}

/*
 * Does key column c of the lion index idx of rel give the pathkey want in
 * its directory's order (DESIGN.md §30.11): an ordered scalar key column, a
 * plain column of the table under the column's own collation, in its type's
 * own btree order, whose pathkey - the ascending one, as
 * build_expression_pathkey() finds it among the query's equivalence classes
 * (for a partition, through its member of the parent's class) - has want's
 * class and operator family?  *var is the column, as rel's Var.  *indexrel is
 * the index, opened at the first call that needs it, for the caller to close.
 */
static bool
lo_column_gives(PlannerInfo *root, RelOptInfo *rel, RangeTblEntry *rte,
				IndexOptInfo *idx, Relation *indexrel, int c, PathKey *want,
				Var **var)
{
	AttrNumber	attno = idx->indexkeys[c];
	LionState  *state;
	TypeCacheEntry *typentry;
	Oid			type;
	int32		typmod;
	Oid			coll;
	List	   *pks;
	PathKey    *mine;

	if (attno <= 0)
		return false;			/* an expression: no Var to order by */
	get_atttypetypmodcoll(rte->relid, attno, &type, &typmod, &coll);
	if (idx->indexcollations[c] != coll)
		return false;

	if (*indexrel == NULL)
		*indexrel = index_open(idx->indexoid, AccessShareLock);
	state = lion_index_column_state(*indexrel, (AttrNumber) (c + 1));
	if (state->multikey || !state->ordered || state->typid != type)
		return false;

	/*
	 * The directory is in the type's default btree order only when the
	 * column's comparison is that order's (the GROUP BY walk's rule,
	 * lion_index_orders_naturally()); its `<` names the operator family the
	 * pathkey is in.
	 */
	typentry = lookup_type_cache(type, TYPECACHE_CMP_PROC | TYPECACHE_LT_OPR);
	if (!OidIsValid(typentry->cmp_proc) ||
		typentry->cmp_proc != state->cmpproc.fn_oid ||
		!OidIsValid(typentry->lt_opr))
		return false;

	*var = makeVar(rel->relid, attno, type, typmod, coll, 0);
	pks = build_expression_pathkey(root, (Expr *) *var, typentry->lt_opr,
								   rel->relids, false);
	if (pks == NIL)
		return false;
	mine = (PathKey *) linitial(pks);
	return mine->pk_eclass == want->pk_eclass &&
		mine->pk_opfamily == want->pk_opfamily;
}

/*
 * The walks of rel's lion columns that give the query's ORDER BY (DESIGN.md
 * §30.11): an ordered scalar key column of a lion index whose order is the
 * first of the ORDER BY's pathkeys (lo_column_gives()).  The walk then gives
 * that pathkey whichever direction and NULLs placement it asks for: it runs
 * either way, and takes the column's NULL entry before its values or after
 * them.  Only the first pathkey: a lion column orders one column, and core
 * sorts the rest incrementally.  Only the ORDER BY's: a GROUP BY, a DISTINCT
 * or a merge join over a lion column wants every row, which the count
 * pushdown counts from the same entries and a hash or a Sort takes from the
 * heap in its order, where the walk would fetch them in key order.
 */
static List *
lo_lion_walks(PlannerInfo *root, RelOptInfo *rel, RangeTblEntry *rte,
			  List *lionidx)
{
	PathKey    *want;
	List	   *walks = NIL;
	ListCell   *lc;

	if (root->sort_pathkeys == NIL)
		return NIL;
	want = (PathKey *) linitial(root->sort_pathkeys);

	foreach(lc, lionidx)
	{
		IndexOptInfo *idx = (IndexOptInfo *) lfirst(lc);
		Relation	indexrel = NULL;
		int			c;

		/*
		 * A partial index holds the rows its predicate selects, which is all
		 * of them only when the query implies it - as core asks of an ordered
		 * index path.
		 */
		if (idx->indpred != NIL && !idx->predOK)
			continue;

		for (c = 0; c < idx->nkeycolumns; c++)
		{
			Var		   *var;
			LoWalk	   *w;
			ListCell   *lc2;
			bool		strict = false;

			if (!lo_column_gives(root, rel, rte, idx, &indexrel, c, want,
								 &var))
				continue;

			w = (LoWalk *) palloc0(sizeof(LoWalk));
			w->index = idx;
			w->indexcol = c;
			w->var = var;
			w->pathkey = want;
#if PG_VERSION_NUM >= 180000
			w->backward = (want->pk_cmptype == COMPARE_GT);
#else
			w->backward = (want->pk_strategy == BTGreaterStrategyNumber);
#endif
			foreach(lc2, rel->baserestrictinfo)
			{
				RestrictInfo *rinfo = lfirst_node(RestrictInfo, lc2);
				Expr	   *qual;
				bool		list;

				if (lo_walk_clause(rinfo, var, idx->opfamily[c],
								   idx->opcintype[c],
								   idx->indexcollations[c], &qual, &list))
				{
					/* one list a walk: any other stays the set's, or a filter */
					if (list && w->nlist > 0)
						continue;
					if (list)
					{
						Node	   *arr = (Node *) lsecond(((ScalarArrayOpExpr *) qual)->args);

#if PG_VERSION_NUM >= 170000
						w->nlist = Max(1.0, estimate_array_length(root, arr));
#else
						w->nlist = Max(1.0, (double) estimate_array_length(arr));
#endif
					}
					w->rinfos = lappend(w->rinfos, rinfo);
					w->quals = lappend(w->quals, qual);
					strict = true;	/* no NULL satisfies any kind */
				}
			}
			w->nulls = strict ? LION_ORDER_NULLS_NONE :
				want->pk_nulls_first ? LION_ORDER_NULLS_FIRST :
				LION_ORDER_NULLS_LAST;
			w->vrinfos = lo_walk_value_rinfos(rel, idx, c, var, w->rinfos);
			walks = lappend(walks, w);
		}
		if (indexrel != NULL)
			index_close(indexrel, AccessShareLock);
	}
	return walks;
}

/*
 * The price of a lion column's walk with one lion access - or none: every row
 * the walk meets is fetched and filtered - (DESIGN.md §30.11, "Cost"), and
 * the size its set is expected to have.  It is §30.3's with the walk in place
 * of the btree's: the entries and TIDs of the range, read a directory leaf
 * at a time from private copies, instead of the btree's index cost.
 */
static void
lo_cost_walk(PlannerInfo *root, RelOptInfo *rel, LoWalk *w, Path *lion,
			 List *lionqual, List *residual, double rows, Cost *startup_p,
			 Cost *total_p)
{
	double		tuples = Max(rel->tuples, 1.0);
	double		pages = Max((double) rel->pages, 1.0);
	Selectivity swalk = 1.0;
	Selectivity sel = 1.0;
	double		walked;
	double		entries;
	double		ndistinct;
	double		perentry;
	double		entrybytes;
	double		leaves;
	double		fetched;
	double		spc_random;
	double		spc_seq;
	double		max_io;
	double		min_io;
	double		pages_corr;
	double		corr;
	QualCost	qcost;
	Cost		startup;
	Cost		run;
	VariableStatData vardata;
	bool		isdefault;
	int16		keywidth;

	/*
	 * The rows of the range - of a partial index, only the ones its predicate
	 * admits, the predicate's clauses the range does not already imply added
	 * as core's add_predicate_to_index_quals() adds them.
	 */
	{
		List	   *walkclauses = list_copy(w->rinfos);
		ListCell   *lc;

		foreach(lc, w->index->indpred)
		{
			Expr	   *pred = (Expr *) lfirst(lc);

			if (!predicate_implied_by(list_make1(pred), w->quals, false))
				walkclauses = lappend(walkclauses, pred);
		}
		if (walkclauses != NIL)
			swalk = clauselist_selectivity(root, walkclauses, rel->relid,
										   JOIN_INNER, NULL);
	}
	walked = clamp_row_est(tuples * swalk);
	examine_variable(root, (Node *) w->var, rel->relid, &vardata);
	ndistinct = Max(1.0, get_variable_numdistinct(&vardata, &isdefault));
	ReleaseVariableStats(vardata);
	entries = Max(1.0, Min(walked, ndistinct * swalk));
	perentry = walked / entries;

	/* the descent to where the walk starts */
	startup = 2.0 * random_page_cost;
	if (lion != NULL)
	{
		Cost		lioncost;
		double		members;
		double		ncont;

		List	   *shared;

		cost_bitmap_tree_node(lion, &lioncost, &sel);
		members = clamp_row_est(sel * tuples);
		ncont = Min(ceil(pages / LION_BLOCKS_PER_CONTAINER), members);
		startup += lioncost + ncont * cpu_operator_cost +
			lo_spill_cost(lo_set_bytes(members, ncont));

		/*
		 * What the set selects among the rows walked: not what the walked
		 * index's predicate already guarantees (lo_cost()).
		 */
		shared = lo_pred_implied(w->index, lionqual);
		if (shared != NIL)
		{
			Selectivity both = clauselist_selectivity(root, shared, rel->relid,
													  JOIN_INNER, NULL);

			if (both > 0)
				sel = Min(sel / both, 1.0);
		}
	}

	/*
	 * The walk: each entry a small entry of a walk (a copy, a test of its key
	 * against the range, its items read), each TID one more test, and the
	 * directory leaves under them - an entry's key and its INLINE set, two
	 * bytes a row, or, past a page, a posting tree's pages.
	 */
	keywidth = get_typlen(w->var->vartype);
	entrybytes = 24.0 + (keywidth > 0 ? (double) keywidth : 16.0) +
		Min(perentry * 2.0 + 8.0, BLCKSZ / 4.0);
	leaves = ceil(entries * entrybytes / (BLCKSZ * 0.7)) +
		(perentry * 2.0 > BLCKSZ / 4.0 ? ceil(walked * 2.0 / BLCKSZ) : 0.0);
	run = leaves * seq_page_cost +
		entries * lion_range_union_entry_cost * cpu_tuple_cost +
		walked * cpu_operator_cost;

	/*
	 * The rows fetched: every row the walk meets, or only the set's members,
	 * each priced as cost_index() prices a fetch, the column's correlation
	 * with the heap interpolating between rows at random and rows in order.
	 *
	 * A filter on the walked column (lo_walk_value_rinfos()) lets an entry
	 * go after one fetched member: of the entries it rejects, the walk reads
	 * the TIDs up to that member and fetches it, and of the rest everything,
	 * as before.  The share of its clauses the set already selects by is not
	 * counted again.
	 */
	fetched = clamp_row_est(walked * sel);
	if (w->vrinfos != NIL)
	{
		List	   *vclauses = NIL;
		ListCell   *lc;

		foreach(lc, w->vrinfos)
		{
			RestrictInfo *rinfo = lfirst_node(RestrictInfo, lc);

			if (lionqual == NIL ||
				!predicate_implied_by(list_make1(rinfo->clause), lionqual,
									  false))
				vclauses = lappend(vclauses, rinfo);
		}
		if (vclauses != NIL)
		{
			Selectivity vsel = clauselist_selectivity(root, vclauses,
													  rel->relid, JOIN_INNER,
													  NULL);
			double		rejected = entries * (1.0 - vsel);
			double		tidsto = Min(perentry, 1.0 / Max(sel, 1e-9));

			run -= walked * cpu_operator_cost;
			walked = clamp_row_est(walked * vsel + rejected * tidsto);
			run += walked * cpu_operator_cost;
			fetched = clamp_row_est(fetched * vsel +
									rejected * Min(1.0, perentry * sel));
		}
	}
	get_tablespace_page_costs(rel->reltablespace, &spc_random, &spc_seq);
	corr = lion_var_heap_correlation(root, rel->relid, (Node *) w->var);
	max_io = index_pages_fetched(fetched, rel->pages,
								 (double) w->index->pages, root) * spc_random;
	pages_corr = ceil(Min(fetched, walked / tuples * pages));
	min_io = (pages_corr > 0) ? spc_random + (pages_corr - 1) * spc_seq : 0;
	run += max_io + corr * corr * (min_io - max_io);

	cost_qual_eval(&qcost, residual, root);
	startup += qcost.startup + rel->reltarget->cost.startup;
	run += fetched * (cpu_tuple_cost + qcost.per_tuple) +
		rows * rel->reltarget->cost.per_tuple;

	/* a list: one more descent for each of its values after the first */
	if (w->nlist > 1.0)
		run += (w->nlist - 1.0) * 2.0 * random_page_cost;

	*startup_p = startup;
	*total_p = startup + run;
}

/*
 * The margin the paths of the relation lion_ordered_set_rel_pathlist() is
 * adding them to are offered at (lion_units_margin_for()), found before the
 * first is added and can free a scan of core's.
 */
static double lo_margin = 1.0;

/*
 * One LionOrdered path, offered to add_path() (DESIGN.md §30.2, step 3) at
 * pg_lion.pushdown_margin (§39): its price divided by the margin, startup
 * and total alike, so that whatever it is compared with - the btree scan it
 * walks, a Sort over another path, a LIMIT's fraction of either - it is
 * taken only by that margin.  Its competitors are the relation's scans, which
 * are lion's reference units already: it has no rate.
 *
 * A btree's walk may run in a worker when the relation may be read in one
 * (`parallel_safe`, DESIGN.md §40.9): whole, as the inner side of a join a
 * worker runs, say; and with `workers` above zero it is a PARTIAL path, a
 * parallel-aware walk whose participants share the btree's pages, offered
 * to add_partial_path() with its rows a participant's.
 *
 * With `ppi` it is a nested loop's probe of rel, parameterized by the outer
 * relations ppi names (§40.10): never partial, never ordered.
 */
static void
lo_add_path(RelOptInfo *rel, ParamPathInfo *ppi,
			const CustomPathMethods *methods, List *pathkeys,
			List *priv, double rows, Cost startup, Cost total,
			bool parallel_safe, int workers)
{
	CustomPath *cp = makeNode(CustomPath);

	Assert(workers == 0 || parallel_safe);
	Assert(ppi == NULL || (workers == 0 && pathkeys == NIL));
	cp->path.pathtype = T_CustomScan;
	cp->path.parent = rel;
	cp->path.pathtarget = rel->reltarget;
	cp->path.param_info = ppi;
	cp->path.parallel_aware = (workers > 0);
	cp->path.parallel_safe = parallel_safe;
	cp->path.parallel_workers = workers;
	cp->path.rows = (workers > 0) ?
		clamp_row_est(rows / lion_parallel_divisor(workers)) : rows;
	cp->path.startup_cost = startup / lo_margin;
	cp->path.total_cost = total / lo_margin;
	cp->path.pathkeys = pathkeys;
	cp->flags = CUSTOMPATH_SUPPORT_PROJECTION;
	cp->custom_paths = NIL;
	cp->custom_private = priv;
	cp->methods = methods;

	/*
	 * A walk of an index, as an index scan or an index-only scan is: core's
	 * enable_indexscan disables it, and enable_indexonlyscan has taken the
	 * index-only modes away already (lo_index_only()), as indxpath.c takes
	 * core's - a partial walk as a parallel index scan.
	 */
	lion_path_core_switches(&cp->path, LION_CORE_INDEXSCAN);
	if (workers > 0)
		add_partial_path(rel, &cp->path);
	else
		add_path(rel, &cp->path);
}

/*
 * A covering walk's price against the bitmap heap scan it competes with, when
 * that scan's reads are cold (lion_units_scan_cold(), DESIGN.md §39, "A
 * bitmap heap scan's cold reads"): its startup and total times the factor
 * its own reads and the scan's give it - at most 1, and never below 1 /
 * pg_lion.cold_max_discount.  Only an index-only walk: one in heap mode
 * fetches each member's tuple when it meets it, a synchronous read, which
 * the stream's price would not cover.
 */
static void
lo_cold_price(const LionUnits *cold, double reads, Cost *startup, Cost *total)
{
	double		factor = lion_units_bitmap_factor(cold, *total, reads);

	*startup *= factor;
	*total *= factor;
}

/*
 * ... and a partial walk's, whose startup and total are one participant's
 * share with no Gather over it, where the scan's cost it is weighed against
 * (lion_units_scan_cold()) is a whole plan's: the serial scan's, or a
 * partial one's under its Gather.  So the walk is priced under the same
 * Gather - parallel_setup_cost, and a parallel_tuple_cost a row of the
 * relation's `rows`, as cost_gather() and lion_units_scan_cold() charge them
 * - the factor is taken of that, and the Gather's share is taken out again:
 * both prices in one unit, and the Gather's overhead discounted with the
 * walk under it.  Never a startup below 0, nor a total below the startup,
 * which a deep discount of a small partial cost would otherwise give.
 */
static void
lo_cold_price_partial(const LionUnits *cold, double reads, double rows,
					  Cost *startup, Cost *total)
{
	Cost		overhead = parallel_setup_cost +
		parallel_tuple_cost * clamp_row_est(rows);
	Cost		gathered = *total + overhead;
	double		factor = lion_units_bitmap_factor(cold, gathered, reads);

	if (factor >= 1.0)
		return;
	*startup = Max((*startup + parallel_setup_cost) * factor -
				   parallel_setup_cost, 0.0);
	*total = Max(gathered * factor - overhead, *startup);
}

/*
 * May a walk of this btree be divided among the participants of a parallel
 * plan (DESIGN.md §40.9)?  The relation may be read in a worker - its
 * restriction clauses, which the node evaluates, and its target are
 * parallel-safe, and it is not a temporary table - and the btree has a
 * parallel scan, which hands each participant leaf pages of its own.
 */
static bool
lo_parallel_ok(RelOptInfo *rel, IndexPath *ord)
{
	return rel->consider_parallel && ord->indexinfo->amcanparallel &&
		max_parallel_workers_per_gather > 0;
}

/*
 * A nested loop's probe of rel parameterized by the outer relations ppi
 * names (DESIGN.md §40.10): how many times the loop is expected to scan it
 * in one execution of the plan, and over how many scans the set, built once
 * for all of them, is spread.
 *
 * *loops is what core's get_loop_count() makes of the parameterization for
 * its own parameterized index scans, and what their heap pages are amortized
 * over: the fewest rows of the outer relations, each taken as the distinct
 * values of a semi join's inner side where it is one, unique-ified before
 * the loop.
 *
 * The scans returned are the loops, or - while enable_memoize lets core put
 * a Memoize above the probe - the distinct values the outer side hands it,
 * as Memoize counts them (estimate_num_groups() over the outer side of each
 * join clause; where a clause is not one Memoize can key on, the loops): a
 * Memoize rescans its inner side for a value it has not cached alone, and
 * charges a scan's price a miss, so the set spread over the loops would be
 * charged a fraction of it - the misses over the loops - in the plan core
 * then likes best.  Spread over the distinct values, the plan without the
 * Memoize pays the set as often as the outer side repeats its values, and
 * loses to the plan with it, which is right.
 */
static double
lo_probe_scans(PlannerInfo *root, RelOptInfo *rel, ParamPathInfo *ppi,
			   double *loops)
{
	double		n = 0.0;
	double		d;
	List	   *exprs = NIL;
	int			relid = -1;
	ListCell   *lc;

	while ((relid = bms_next_member(ppi->ppi_req_outer, relid)) >= 0)
	{
		RelOptInfo *outer;
		double		rows;

		/* an outer join's relid names no relation */
		if (relid >= root->simple_rel_array_size)
			continue;
		outer = root->simple_rel_array[relid];
		if (outer == NULL || IS_DUMMY_REL(outer))
			continue;
		rows = Max(outer->rows, 1.0);
		foreach(lc, root->join_info_list)
		{
			SpecialJoinInfo *sjinfo = lfirst_node(SpecialJoinInfo, lc);
			double		nraw = 1.0;
			int			rhs = -1;

			if (sjinfo->jointype != JOIN_SEMI ||
				sjinfo->semi_rhs_exprs == NIL ||
				!bms_is_member((int) rel->relid, sjinfo->syn_lefthand) ||
				!bms_is_member(relid, sjinfo->syn_righthand))
				continue;
			/* the product of its relations' rows, as core approximates it */
			while ((rhs = bms_next_member(sjinfo->syn_righthand, rhs)) >= 0)
			{
				RelOptInfo *r = (rhs < root->simple_rel_array_size) ?
					root->simple_rel_array[rhs] : NULL;

				if (r != NULL && !IS_DUMMY_REL(r))
					nraw *= Max(r->rows, 1.0);
			}
			rows = Min(rows, estimate_num_groups(root, sjinfo->semi_rhs_exprs,
												 nraw, NULL, NULL));
		}
		if (n == 0.0 || rows < n)
			n = rows;
	}
	n = Max(n, 1.0);
	*loops = n;
	if (!enable_memoize)
		return n;

	foreach(lc, ppi->ppi_clauses)
	{
		RestrictInfo *rinfo = lfirst_node(RestrictInfo, lc);
		OpExpr	   *op = (OpExpr *) rinfo->clause;

		if (!IsA(op, OpExpr) || list_length(op->args) != 2)
			return n;
		if (bms_is_subset(rinfo->left_relids, ppi->ppi_req_outer) &&
			bms_is_subset(rinfo->right_relids, rel->relids))
			exprs = lappend(exprs, linitial(op->args));
		else if (bms_is_subset(rinfo->right_relids, ppi->ppi_req_outer) &&
				 bms_is_subset(rinfo->left_relids, rel->relids))
			exprs = lappend(exprs, lsecond(op->args));
		else
			return n;
	}
	if (exprs == NIL)
		return n;
	d = estimate_num_groups(root, exprs, n, NULL, NULL);
	return Max(Min(d, n), 1.0);
}

/* ---------------------------------------------------------------------
 * A lionvec order under a filter: pg_lion.vector_postfilter (DESIGN.md
 * §44.6, "Post-filtering")
 *
 * A lionvec scan with a Filter above it is not the restricted scan with the
 * rejected rows left out.  The scan ends a pause, and moves the frontier it
 * skips late rows behind, on the rows it RETURNS (lion_vec_scan.c,
 * lionvecgettuple()); the executor's Filter rejects most of those after the
 * fact, and the AM never learns it.  So at the same margin and patience the
 * two plans find other rows: on 1M rows under a 1% filter at a margin of 8
 * the plain scan paused 17.6 times a query and read 29,075 pages for 0.993
 * of the true ten, LionOrdered paused once and read 3,483 for 0.980; at a
 * margin of 3 and no pause the plain scan's frontier, advanced by rows the
 * Filter rejected, skipped members LionOrdered kept (0.960 against 0.966).
 * The plan must not decide the answer.  So unless the setting is allow, a
 * relation the query restricts has no plain lionvec order among its paths
 * (lo_knn_take()), and its LionOrdered over that order only when lion's set
 * answers every clause - the node's own Filter would post-filter the members
 * the same way.  What is left is, under error (the default), an error at
 * plan time that suggests the subquery that makes the approximation
 * explicit, or under exact an exact plan (a sort of the filter's rows, say).
 * --------------------------------------------------------------------- */

/* Why a lionvec order got no LionOrdered path, for error's message. */
typedef enum LoKnnWhy
{
	LO_KNN_NONE,				/* nothing in the way but the clauses */
	LO_KNN_DISABLED,			/* pg_lion.enable_ordered_scan is off */
	LO_KNN_RELATION				/* not a relation the node scans (§30.1) */
} LoKnnWhy;

/*
 * The relation's lionvec orders lo_knn_take() took out of its path list, and
 * what lo_set_rel_pathlist() offered over them.
 */
typedef struct LoKnn
{
	List	   *taken;			/* IndexPaths, in path-list order */
	int			offered;		/* LionOrdered paths over a lionvec order */
	LoKnnWhy	why;			/* the first obstacle met, when none was */
} LoKnn;

static void
lo_knn_why(LoKnn *knn, LoKnnWhy why)
{
	if (knn->why == LO_KNN_NONE)
		knn->why = why;
}

/*
 * Is the clause one of the lionvec index's own, which reads no column of the
 * relation but those the index's key does - the vector's (`v IS NOT NULL`, a
 * distance bound `v <-> $1 < 0.5`)?  Such a clause may filter the rows the
 * scan returns under every pg_lion.vector_postfilter (DESIGN.md §44.6,
 * "Post-filtering"): it rejects NULL vectors, which the scan returns last, or
 * rows by their distance, which the scan returns in order, so it leaves the
 * rows the scan finds before it as they were.  A clause of another relation's
 * columns too (a parameterized path's join clause) is no such clause.
 */
static bool
lo_knn_own(IndexOptInfo *idx, RestrictInfo *rinfo)
{
	Bitmapset  *keyattrs = NULL;
	Bitmapset  *attrs = NULL;
	int			i;

	if (!bms_equal(rinfo->clause_relids, idx->rel->relids))
		return false;
	for (i = 0; i < idx->nkeycolumns; i++)
		if (idx->indexkeys[i] != 0)
			keyattrs = bms_add_member(keyattrs, idx->indexkeys[i] -
									  FirstLowInvalidHeapAttributeNumber);
	pull_varattnos((Node *) idx->indexprs, idx->rel->relid, &keyattrs);
	pull_varattnos((Node *) rinfo->clause, idx->rel->relid, &attrs);
	return !bms_is_empty(attrs) && bms_is_subset(attrs, keyattrs);
}

/* The clauses of rinfos but the lionvec index's own (lo_knn_own()). */
static List *
lo_knn_others(List *rinfos, IndexOptInfo *idx)
{
	List	   *result = NIL;
	ListCell   *lc;

	foreach(lc, rinfos)
		if (!lo_knn_own(idx, lfirst_node(RestrictInfo, lc)))
			result = lappend(result, lfirst(lc));
	return result;
}

/*
 * The clauses a plain scan of a lionvec order would leave to its Filter, as
 * create_indexscan_plan() makes its qpqual: the restriction clauses the
 * index is given (indrestrictinfo, which for a partial index leaves out the
 * ones its predicate implies, check_index_predicates()) and a parameterized
 * path's join clauses, less the pseudoconstant ones a gating Result tests
 * once and the index's own (lo_knn_own()), which may filter its rows.  The
 * order has no index clauses (lo_knn_path()), so nothing else comes off.
 * NIL: the scan returns every row the query keeps, or every row but those
 * its own clauses reject.
 */
static List *
lo_knn_filter(IndexPath *ipath)
{
	List	   *clauses = ipath->indexinfo->indrestrictinfo;
	List	   *result = NIL;
	ListCell   *lc;

	if (ipath->path.param_info != NULL)
		clauses = list_concat_copy(clauses,
								   ipath->path.param_info->ppi_clauses);
	foreach(lc, clauses)
	{
		RestrictInfo *rinfo = lfirst_node(RestrictInfo, lc);

		if (!rinfo->pseudoconstant && !lo_knn_own(ipath->indexinfo, rinfo))
			result = lappend(result, rinfo);
	}
	return result;
}

/*
 * Take the relation's lionvec orders that a Filter would follow out of its
 * path list, into knn->taken.  This relies on core calling
 * set_rel_pathlist_hook before set_cheapest() (allpaths.c,
 * set_rel_pathlist(), 16 to 19): nothing has chosen among the paths yet,
 * so a path no longer in the list is one no plan can take.  The paths stay
 * valid - add_path() never frees an IndexPath, which a bitmap path may share
 * - and LionOrdered still walks them, from knn->taken.  A hypothetical index
 * (hypopg's) is left, as the node leaves it.
 *
 * A path the lionvec order made add_path() discard is not brought back but
 * for core's sequential scan, offered again as set_plain_rel_pathlist()
 * offers it, so that the relation has an unparameterized path whatever the
 * order had beaten: one cheaper in total, with its pathkeys, dominates a
 * path that has none when no LIMIT makes start-up count.
 *
 * A partial path of a lionvec order (a Parallel Index Scan, DESIGN.md
 * §44.4, "A parallel scan") is the serial one's in parallel, and goes by
 * the same rule: a Filter over it, under Gather Merge, would be the same
 * post-filter.  It goes from the partial path list alone - LionOrdered is
 * serial and walks the serial paths - and needs nothing brought back, a
 * partial sequential scan having no pathkeys it could have lost to.
 */
static void
lo_knn_take(PlannerInfo *root, RelOptInfo *rel, LoKnn *knn)
{
	ListCell   *lc;

	foreach(lc, rel->pathlist)
	{
		Path	   *p = (Path *) lfirst(lc);
		IndexPath  *ipath;

		if (!IsA(p, IndexPath))
			continue;
		ipath = (IndexPath *) p;
		if (lo_knn_path(ipath) && !ipath->indexinfo->hypothetical &&
			lo_knn_filter(ipath) != NIL)
			knn->taken = lappend(knn->taken, ipath);
	}

	foreach(lc, rel->partial_pathlist)
	{
		Path	   *p = (Path *) lfirst(lc);
		IndexPath  *ipath;

		if (!IsA(p, IndexPath))
			continue;
		ipath = (IndexPath *) p;
		if (lo_knn_path(ipath) && !ipath->indexinfo->hypothetical &&
			lo_knn_filter(ipath) != NIL)
			rel->partial_pathlist =
				foreach_delete_current(rel->partial_pathlist, lc);
	}

	if (knn->taken == NIL)
		return;
	foreach(lc, knn->taken)
		rel->pathlist = list_delete_ptr(rel->pathlist, lfirst(lc));
	add_path(rel, create_seqscan_path(root, rel, rel->lateral_relids, 0));
}

/*
 * The partial paths of a lionvec order at their parallel cost
 * (lion_vec_parallel_cost(), which cost_index() could not ask for: it
 * prices the index before it counts the workers), each taken out and
 * offered again as lion_plain_set_rel_pathlist() reprices a plain path,
 * so that add_partial_path() orders and prunes them by the cost they now
 * have; or, unless pg_lion.vector_parallel_scan is on, taken out and not
 * offered again.
 */
static void
lo_knn_partial(PlannerInfo *root, RelOptInfo *rel)
{
	List	   *repriced = NIL;
	ListCell   *lc;

	foreach(lc, rel->partial_pathlist)
	{
		Path	   *p = (Path *) lfirst(lc);

		if (IsA(p, IndexPath) && p->parallel_workers > 0 &&
			lion_vec_index(((IndexPath *) p)->indexinfo))
		{
			repriced = lappend(repriced, p);
			rel->partial_pathlist =
				foreach_delete_current(rel->partial_pathlist, lc);
		}
	}
	foreach(lc, repriced)
	{
		IndexPath  *ipath = (IndexPath *) lfirst(lc);

		if (!lionvec_parallel_scan)
			continue;
		lion_vec_parallel_cost(root, ipath);
		add_partial_path(rel, (Path *) ipath);
	}
	list_free(repriced);
}

/*
 * A lionvec order among the relation's paths, serial or partial, or NULL: a
 * plan could order its rows by that index.
 */
static IndexPath *
lo_knn_first(RelOptInfo *rel)
{
	List	   *lists[2] = {rel->pathlist, rel->partial_pathlist};
	ListCell   *lc;

	for (int i = 0; i < 2; i++)
		foreach(lc, lists[i])
		{
			Path	   *p = (Path *) lfirst(lc);

			if (IsA(p, IndexPath) && lo_knn_path((IndexPath *) p) &&
				!((IndexPath *) p)->indexinfo->hypothetical)
				return (IndexPath *) p;
		}
	return NULL;
}

/*
 * Is a lionvec order no Filter follows still among the relation's paths?
 * The serial ones alone: a partial path no Filter follows has the serial
 * path of the same index and clauses beside it.
 */
static bool
lo_knn_kept(RelOptInfo *rel)
{
	ListCell   *lc;

	foreach(lc, rel->pathlist)
	{
		Path	   *p = (Path *) lfirst(lc);

		if (IsA(p, IndexPath) && lo_knn_path((IndexPath *) p) &&
			!((IndexPath *) p)->indexinfo->hypothetical)
			return true;
	}
	return false;
}

/* The restriction clauses a lion access leaves to the node's Filter. */
static List *
lo_lion_residual(RelOptInfo *rel, Path *lion)
{
	List	   *lionrinfos = NIL;
	bool		lossy = false;
	List	   *lionqual = lo_lion_qual(lion, &lionrinfos, &lossy);

	return lo_residual(rel->baserestrictinfo, NIL, lionrinfos, lionqual);
}

/* The members of a that are in b, by pointer. */
static List *
lo_rinfos_in(List *a, List *b)
{
	List	   *result = NIL;
	ListCell   *lc;

	foreach(lc, a)
		if (list_member_ptr(b, lfirst(lc)))
			result = lappend(result, lfirst(lc));
	return result;
}

/*
 * The accesses a cover is ANDed from (lo_cover_and()): core's, cands, and
 * those core builds for each lion index alone, whose paths the others cannot
 * have pruned; *residuals their residuals (lo_lion_residual()), in order.
 */
static List *
lo_cover_pool(PlannerInfo *root, RelOptInfo *rel, List *cands, List *lionidx,
			  Oid lionam, List **residuals)
{
	List	   *pool = list_copy(cands);
	ListCell   *lc;

	foreach(lc, lionidx)
		pool = list_concat(pool, lo_lion_accesses(root, rel,
												  list_make1(lfirst(lc)),
												  NIL, lionam));
	*residuals = NIL;
	foreach(lc, pool)
		*residuals = lappend(*residuals,
							 lo_lion_residual(rel, (Path *) lfirst(lc)));
	return pool;
}

/*
 * The access of pool that answers every clause of rest, or the BitmapAnd of
 * those that do: the access that leaves the fewest of them and then, while a
 * clause is left, the one that answers most of the ones left.  NULL when no
 * AND of them answers all; *left is then the clauses none answers.
 */
static Path *
lo_cover_and(PlannerInfo *root, RelOptInfo *rel, List *pool,
			 List *residuals, List *rest, List **left)
{
	List	   *chosen = NIL;
	List	   *quals = NIL;
	Path	   *best = NULL;
	ListCell   *lc;
	ListCell   *lc2;

	*left = NIL;
	for (;;)
	{
		int			most = 0;
		List	   *bestres = NIL;

		best = NULL;
		forboth(lc, pool, lc2, residuals)
		{
			List	   *res = (List *) lfirst(lc2);
			int			answers = list_length(rest) -
				list_length(lo_rinfos_in(rest, res));

			if (answers > most && !list_member_ptr(chosen, lfirst(lc)))
			{
				most = answers;
				best = (Path *) lfirst(lc);
				bestres = res;
			}
		}
		if (best == NULL)
			break;
		chosen = lappend(chosen, best);
		rest = lo_rinfos_in(rest, bestres);
		if (rest == NIL)
			break;
	}
	if (rest != NIL || chosen == NIL)
	{
		*left = rest;
		return NULL;
	}
	if (list_length(chosen) == 1)
		return (Path *) linitial(chosen);

	/* one BitmapAnd of the accesses, flattened, as core would make it */
	foreach(lc, chosen)
	{
		Path	   *p = (Path *) lfirst(lc);

		if (IsA(p, BitmapAndPath))
			quals = list_concat(quals, ((BitmapAndPath *) p)->bitmapquals);
		else
			quals = lappend(quals, p);
	}
	return (Path *) create_bitmap_and_path(root, rel, quals);
}

/*
 * The lion side of a lionvec order that answers every restriction clause a
 * lion index can (DESIGN.md §44.6, "Post-filtering"), when none of core's
 * accesses `cands` does: core builds the accesses a bitmap scan would use,
 * and leaves out of a BitmapAnd an index whose bitmap costs more than the
 * rows it saves the heap scan (choose_bitmap_and()) - `tag < 50` beside
 * `cat IN (1, 2)` - since a Filter takes such a clause cheaply.  A lionvec
 * scan's Filter is what may not happen here, so the set is ANDed from the
 * accesses that answer them (lo_cover_and()), among core's and those of
 * each lion index alone (lo_cover_pool()).  The price decides as for any
 * lion side (lo_cost_knn()): a clause that keeps most rows makes a set
 * costly to build.
 *
 * The clauses of the lionvec index vidx's own (lo_knn_own()) need no
 * answer: the node's Filter may apply them.
 *
 * NULL when an access in cands answers every clause already, or when some
 * clause no lion access answers (or, which should not happen, their AND
 * leaves one); *left is then those clauses, which a plan must filter.
 */
static Path *
lo_knn_cover(PlannerInfo *root, RelOptInfo *rel, List *cands, List *lionidx,
			 Oid lionam, IndexOptInfo *vidx, List **left)
{
	List	   *pool;
	List	   *residuals;
	List	   *rest = NIL;
	Path	   *best;
	ListCell   *lc;

	*left = NIL;
	foreach(lc, rel->baserestrictinfo)
		if (!lfirst_node(RestrictInfo, lc)->pseudoconstant)
			rest = lappend(rest, lfirst(lc));
	rest = lo_knn_others(rest, vidx);
	if (rest == NIL)
		return NULL;			/* the scan's own clauses alone */
	foreach(lc, cands)
		if (lo_knn_others(lo_lion_residual(rel, (Path *) lfirst(lc)),
						  vidx) == NIL)
			return NULL;		/* the pairing has it already */

	pool = lo_cover_pool(root, rel, cands, lionidx, lionam, &residuals);
	best = lo_cover_and(root, rel, pool, residuals, rest, left);
	if (best == NULL)
		return NULL;

	/*
	 * Its residual should be none, being no larger than any of theirs: a
	 * clause one access's qual implies, the conjunction of all of theirs
	 * implies too.  Were predicate_implied_by() to prove less of the
	 * conjunction than of its parts, the clauses it leaves are left as a
	 * clause no access answers is: the query loses LionOrdered over the
	 * order, which is better than failing it.
	 */
	rest = lo_knn_others(lo_lion_residual(rel, best), vidx);
	Assert(rest == NIL);
	if (rest != NIL)
	{
		*left = rest;
		return NULL;
	}
	return best;
}

/* Does every access in cands leave one of the clauses want to its Filter? */
static bool
lo_cover_wanted(RelOptInfo *rel, List *cands, List *want)
{
	ListCell   *lc;

	if (want == NIL)
		return false;
	foreach(lc, cands)
		if (lo_rinfos_in(want, lo_lion_residual(rel, (Path *) lfirst(lc))) == NIL)
			return false;
	return true;
}

/*
 * The lion side of a btree walk that answers every restriction clause a lion
 * access answers (DESIGN.md §40.2, "A clause core's AND leaves out"), when
 * none of core's accesses `cands` does.  Core leaves out of a BitmapAnd a
 * clause that keeps most rows (lo_knn_cover()) - `b IN (1, 2)` of three
 * values beside `a IN (1, 2)` of twenty - since its bitmap heap scan's
 * Filter tests the clause on the heap tuple it fetches anyway.  A covering
 * btree's walk fetches none, but a clause its set leaves, of a column the
 * btree does not hold, would make it fetch every member's: no index-only
 * walk is offered, and the query goes to the heap.  So beside core's
 * accesses, the AND of those that answer them all (lo_cover_and()), for the
 * walks it makes index-only; the price decides, as a clause that keeps most
 * rows makes a set costly to build, and a btree holding its column takes it
 * from the index tuple more cheaply.  A walk in heap mode reads the tuple
 * for its values and keeps core's accesses.
 *
 * The clauses wanted are those whose columns a lion index's key has
 * (lo_knn_own()) and some lion access answers: one none answers is the
 * Filter's whatever the set.  NULL when an access in cands answers all of
 * them already.
 */
static Path *
lo_lion_cover(PlannerInfo *root, RelOptInfo *rel, List *cands, List *lionidx,
			  Oid lionam)
{
	List	   *want = NIL;
	List	   *answered = NIL;
	List	   *pool;
	List	   *residuals;
	List	   *left;
	ListCell   *lc;
	ListCell   *lc2;

	foreach(lc, rel->baserestrictinfo)
	{
		RestrictInfo *rinfo = lfirst_node(RestrictInfo, lc);

		if (rinfo->pseudoconstant)
			continue;
		foreach(lc2, lionidx)
		{
			if (lo_knn_own((IndexOptInfo *) lfirst(lc2), rinfo))
			{
				want = lappend(want, rinfo);
				break;
			}
		}
	}
	if (!lo_cover_wanted(rel, cands, want))
		return NULL;

	pool = lo_cover_pool(root, rel, cands, lionidx, lionam, &residuals);
	foreach(lc, want)
	{
		foreach(lc2, residuals)
		{
			if (!list_member_ptr((List *) lfirst(lc2), lfirst(lc)))
			{
				answered = lappend(answered, lfirst(lc));
				break;
			}
		}
	}
	if (!lo_cover_wanted(rel, cands, answered))
		return NULL;
	return lo_cover_and(root, rel, pool, residuals, answered, &left);
}

/*
 * The restriction clauses of rel no lion access answers, for error's
 * message: what lo_knn_cover() cannot cover - every one but the
 * pseudoconstant ones when the relation has no lion index.  Of all of them,
 * not only of the Filter the taken order would have: LionOrdered walks an
 * order only over a set that answers every clause, so one a partial lionvec
 * index's predicate implies - no Filter of that index's scan - still keeps
 * the node from that order when no lion index answers it.  The lionvec index
 * vidx's own clauses (lo_knn_own()) are none of them.
 */
static List *
lo_knn_unanswered(PlannerInfo *root, RelOptInfo *rel, IndexOptInfo *vidx)
{
	Oid			lionam = lion_get_am_oid();
	List	   *lionidx = NIL;
	List	   *rest = NIL;
	List	   *left;
	ListCell   *lc;

	foreach(lc, rel->baserestrictinfo)
		if (!lfirst_node(RestrictInfo, lc)->pseudoconstant)
			rest = lappend(rest, lfirst(lc));
	rest = lo_knn_others(rest, vidx);
	if (!OidIsValid(lionam))
		return rest;
	foreach(lc, rel->indexlist)
	{
		IndexOptInfo *idx = (IndexOptInfo *) lfirst(lc);

		if (idx->relam == lionam && !idx->hypothetical)
			lionidx = lappend(lionidx, idx);
	}
	if (lionidx == NIL)
		return rest;
	(void) lo_knn_cover(root, rel, lo_lion_accesses(root, rel, lionidx, NIL,
													lionam),
						lionidx, lionam, vidx, &left);
	return left;
}

/* At most this many bytes of the clauses in error's message. */
#define LO_KNN_CLAUSES_TEXT		512

/*
 * pg_lion.vector_postfilter = error, the default: the relation's lionvec
 * order would be filtered after its scan, and no LionOrdered path restricts
 * the scan instead.  At plan time, so that EXPLAIN says so too.  The hint is
 * the subquery that filters the first rows of a scan no Filter follows, and
 * the settings that plan the query exactly or filter the scan's rows.
 * (Row-level security, whose policies would filter the subquery's scan too,
 * lionvec refuses before: lionvec_refuse_rls().)
 */
static void
lo_knn_error(PlannerInfo *root, RelOptInfo *rel, RangeTblEntry *rte,
			 LoKnn *knn)
{
	IndexPath  *ipath = linitial_node(IndexPath, knn->taken);
	List	   *unanswered = lo_knn_unanswered(root, rel, ipath->indexinfo);
	const char *index = get_rel_name(ipath->indexinfo->indexoid);
	const char *table = get_rel_name(rte->relid);
	StringInfoData clauses;
	ListCell   *lc;

	if (table == NULL)
		table = rte->eref->aliasname;

	initStringInfo(&clauses);
	foreach(lc, unanswered)
	{
		RestrictInfo *rinfo = lfirst_node(RestrictInfo, lc);

		if (clauses.len > 0)
			appendStringInfoString(&clauses, " AND ");
		appendStringInfoString(&clauses,
							   lion_deparse_rel_clause(root, rel->relid,
													   (Node *) rinfo->clause));
		if (clauses.len > LO_KNN_CLAUSES_TEXT)
		{
			clauses.len = pg_mbcliplen(clauses.data, clauses.len,
									   LO_KNN_CLAUSES_TEXT);
			clauses.data[clauses.len] = '\0';
			appendStringInfoString(&clauses, "...");
			break;
		}
	}

	ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("lionvec index \"%s\" would apply the filter after its nearest-neighbour scan",
					index != NULL ? index : "?"),
			 unanswered != NIL ?
			 errdetail("No lion index of \"%s\" answers %s, so LionOrdered cannot restrict the scan to the rows that pass it.",
					   table, clauses.data) :
			 knn->why == LO_KNN_DISABLED ?
			 errdetail("pg_lion.enable_ordered_scan is off, so LionOrdered cannot restrict the scan to the rows that pass the filter.") :
			 errdetail("LionOrdered, which restricts the scan to the rows that pass a filter, does not scan \"%s\" in this query.",
					   table),
			 errhint("Filter the first rows of a nearest-neighbour subquery instead, which makes the approximation explicit: SELECT ... FROM (SELECT ..., <distance> AS d FROM %s [WHERE <clauses lion indexes answer>] ORDER BY d LIMIT 1000) s WHERE <the other clauses> ORDER BY d LIMIT 10.  For an exact answer, a sort of the rows that pass the filter, set pg_lion.vector_postfilter to exact; to filter the rows the scan returns, set it to allow.",
					 table)));
}

/*
 * The LionOrdered paths of a relation (DESIGN.md §30.2, §40.2, §30.11,
 * §44.6), added to its path list; knn says which lionvec orders
 * lo_knn_take() took out of it, which are walked as if they were there, and
 * gathers what was offered over them.
 */
static void
lo_set_rel_pathlist(PlannerInfo *root, RelOptInfo *rel, RangeTblEntry *rte,
					LoKnn *knn)
{
	Oid			lionam;
	List	   *ordpaths = NIL;
	List	   *btpaths = NIL;
	List	   *probepaths = NIL;
	List	   *lionidx = NIL;
	List	   *cands = NIL;
	List	   *knncands;
	Path	   *lioncover = NULL;
	List	   *walks = NIL;
	Bitmapset  *targetattrs = NULL;
	ListCell   *lc;
	ListCell   *lc2;
	double		rows;
	LionUnits	cold;			/* a bitmap heap scan's cold reads (§39) */

	if (!lion_enable_ordered_scan)
		lo_knn_why(knn, LO_KNN_DISABLED);
	if (!lion_enable_ordered_scan && !lion_enable_btree_scan)
		return;
	/* no lion index is read under 16's old_snapshot_threshold (§9) */
	if (lion_old_snapshot_threshold_active())
		return;
	if (!lo_rel_ok(root, rel, rte))
	{
		lo_knn_why(knn, LO_KNN_RELATION);
		return;
	}

	lionam = lion_get_am_oid();
	if (!OidIsValid(lionam))
		return;

	foreach(lc, rel->indexlist)
	{
		IndexOptInfo *idx = (IndexOptInfo *) lfirst(lc);

		if (idx->relam == lionam && !idx->hypothetical)
			lionidx = lappend(lionidx, idx);
	}
	if (lionidx == NIL)
		return;
	lo_margin = lion_units_margin_for(rel);
	lion_units_scan_cold(root, rel, &cold);
	if (enable_indexonlyscan)
		pull_varattnos((Node *) rel->reltarget->exprs, rel->relid,
					   &targetattrs);

	/* the ordered side: core's ordered btree paths ... */
	if (lion_enable_ordered_scan && rel->baserestrictinfo != NIL)
	{
		foreach(lc, rel->pathlist)
		{
			Path	   *p = (Path *) lfirst(lc);
			IndexPath  *ipath;

			if (!IsA(p, IndexPath) || p->pathkeys == NIL ||
				p->param_info != NULL)
				continue;
			ipath = (IndexPath *) p;

			/*
			 * A btree: the early stop and the fetch-and-sort switch rest on
			 * its returning each heap TID once per scan, in the order its
			 * pathkeys claim (DESIGN.md §30.4).  Or a lionvec index's order
			 * by distance (§44.6), which returns each member once.
			 */
			if (lo_knn_path(ipath) && !ipath->indexinfo->hypothetical)
			{
				ordpaths = lappend(ordpaths, ipath);
				continue;
			}
			if (ipath->indexorderbys != NIL ||
				ipath->indexinfo->relam != BTREE_AM_OID ||
				ipath->indexinfo->hypothetical)
				continue;
			if (!lo_indexpath_ok(ipath))
				continue;
			ordpaths = lappend(ordpaths, ipath);
		}

		/*
		 * ... and the lionvec orders lo_knn_take() took out of the list, a
		 * plain scan of which a Filter would follow: the node still walks
		 * them (§44.6, "Post-filtering").
		 */
		ordpaths = list_concat(ordpaths, knn->taken);
	}

	/* ... and the walks of lion's own ordered columns (§30.11) */
	if (lion_enable_ordered_scan)
		walks = lo_lion_walks(root, rel, rte, lionidx);

	/*
	 * ... and the paths of the relation's btrees that return at least the
	 * target's columns (what every pair below needs, and the rest is the
	 * pair's), whether or not core found their order useful (DESIGN.md
	 * §40.2): with the row's values coming from the index tuple, their walk
	 * pays without an ORDER BY.
	 */
	if (lion_enable_btree_scan && enable_indexonlyscan &&
		rel->baserestrictinfo != NIL)
	{
		foreach(lc, rel->indexlist)
		{
			IndexOptInfo *idx = (IndexOptInfo *) lfirst(lc);
			List	   *iocols;

			if (idx->relam != BTREE_AM_OID || idx->hypothetical ||
				!lo_index_only(idx, targetattrs, &iocols))
				continue;
			btpaths = list_concat(btpaths,
								  lo_btree_paths(root, rel, idx, &probepaths));
		}
	}
	if (ordpaths == NIL && walks == NIL && btpaths == NIL && probepaths == NIL)
		return;

	/* the heap table AM only (§2); ambuild refused every other */
	{
		Relation	relation = table_open(rte->relid, NoLock);
		bool		supported = lion_table_am_supported(relation);

		table_close(relation, NoLock);
		if (!supported)
		{
			lo_knn_why(knn, LO_KNN_RELATION);
			return;
		}
	}

	/*
	 * The rows the node returns, as lion's estimates see them: core's, times
	 * what the intersection probe measured of the set clauses among the
	 * relation's restriction clauses (lion_probe_rel_rows(), DESIGN.md
	 * §29.11, "Correlated sets"), as the lion side's selectivity already is
	 * (lioncostestimate()).  Core's LIMIT planning takes the share of the
	 * walk a LIMIT needs from them (§30.3): priced for the few rows core's
	 * product of the clauses' selectivities says, the node was charged its
	 * whole walk for a page of a correlated filter's thousands of rows, and
	 * lost to the lion scan and Sort that fetch every one of them.
	 */
	rows = lion_probe_rel_rows(root, rel);

	/*
	 * The lion side of a btree walk, as core would build it for every
	 * restriction clause.
	 */
	if (ordpaths != NIL || btpaths != NIL || probepaths != NIL)
		cands = lo_lion_accesses(root, rel, lionidx, NIL, lionam);

	/*
	 * A lionvec order is walked over a set that answers every clause unless
	 * pg_lion.vector_postfilter is allow (below): when none of core's
	 * accesses does, one ANDed to (lo_knn_cover()).
	 */
	knncands = cands;
	if (lionvec_postfilter != LIONVEC_POSTFILTER_ALLOW)
	{
		foreach(lc, ordpaths)
		{
			List	   *left;
			Path	   *cover;

			if (!lo_knn_path((IndexPath *) lfirst(lc)))
				continue;
			cover = lo_knn_cover(root, rel, cands, lionidx, lionam,
								 ((IndexPath *) lfirst(lc))->indexinfo, &left);
			if (cover != NULL)
				knncands = lappend(list_copy(cands), cover);
			break;
		}
	}

	/*
	 * And the AND that answers each clause a lion access answers, when none
	 * of core's accesses does (lo_lion_cover()), for the walks it lets read
	 * no heap tuple: index-only ones.
	 */
	if (cands != NIL)
		lioncover = lo_lion_cover(root, rel, cands, lionidx, lionam);
	if (lioncover != NULL)
		cands = lappend(list_copy(cands), lioncover);

	foreach(lc, ordpaths)
	{
		IndexPath  *ord = (IndexPath *) lfirst(lc);
		List	   *ordrinfos = lo_ord_rinfos(ord);

		foreach(lc2, lo_knn_path(ord) ? knncands : cands)
		{
			Path	   *lion = (Path *) lfirst(lc2);
			List	   *lionrinfos = NIL;
			bool		lossy = false;
			List	   *lionqual = lo_lion_qual(lion, &lionrinfos, &lossy);
			List	   *residual = lo_residual(rel->baserestrictinfo, ordrinfos,
											   lionrinfos, lionqual);
			List	   *iocols = NIL;
			bool		indexonly;
			int			workers;
			Cost		startup;
			Cost		total;
			double		reads;

			if (lo_knn_path(ord))
			{
				/*
				 * Only over a set that answers every clause but the index's
				 * own (lo_knn_own()), unless pg_lion.vector_postfilter is
				 * allow: the node's own Filter would reject members the scan
				 * has returned, which ends its pause and moves its frontier
				 * as the plain scan's Filter does (§44.6, "Post-filtering").
				 */
				if (lo_knn_others(residual, ord->indexinfo) != NIL &&
					lionvec_postfilter != LIONVEC_POSTFILTER_ALLOW)
					continue;
				/* a lionvec index returns no values: heap mode (§44.6) */
				lo_cost_knn(root, rel, ord, lion, residual, rows,
							&startup, &total);
				lo_add_path(rel, NULL, &lion_ordered_path_methods,
							ord->path.pathkeys, list_make3(ord, lion, NIL),
							rows, startup, total, false, 0);
				knn->offered++;
				continue;
			}

			/* the values from the index tuple when the btree has them (§40) */
			indexonly = lo_index_only(ord->indexinfo,
									  lo_needed_attrs(rel, residual,
													  ord->indexclauses,
													  lionqual, lossy),
									  &iocols);
			if (lion == lioncover && !indexonly)
				continue;
			lo_cost(root, rel, ord, lion, lionrinfos, lionqual, residual, rows,
					indexonly, 1.0, 1.0, ord->path.param_info != NULL, NULL,
					&startup, &total, &reads, NULL);
			if (indexonly)
				lo_cold_price(&cold, reads, &startup, &total);
			lo_add_path(rel, NULL, &lion_ordered_path_methods,
						ord->path.pathkeys, list_make3(ord, lion, iocols),
						rows, startup, total, rel->consider_parallel, 0);

			/* ... and its walk divided among a parallel plan's participants */
			if (lo_parallel_ok(rel, ord) &&
				lo_cost(root, rel, ord, lion, lionrinfos, lionqual, residual,
						rows, indexonly, 1.0, 1.0,
						ord->path.param_info != NULL, &workers, &startup,
						&total, &reads, NULL))
			{
				if (indexonly)
					lo_cold_price_partial(&cold, reads, rows, &startup,
										  &total);
				lo_add_path(rel, NULL, &lion_ordered_path_methods,
							ord->path.pathkeys, list_make3(ord, lion, iocols),
							rows, startup, total, true, workers);
			}
		}
	}

	/*
	 * A covering btree's paths (DESIGN.md §40.2), each paired with each lion
	 * access as above - but not with one whose every clause the btree's own
	 * quals answer already, which would add nothing to the walk - priced in
	 * index-only mode, and offered at whatever order the path has: with
	 * pathkeys it is a LionOrdered, without them a LionBtreeScan.  The twin
	 * of an ordered path above - the same index, direction and order - was
	 * offered there.  With the ordered scans off no order is claimed at all.
	 */
	foreach(lc, btpaths)
	{
		IndexPath  *ipath = (IndexPath *) lfirst(lc);
		List	   *ordrinfos = lo_ord_rinfos(ipath);
		List	   *pathkeys = lion_enable_ordered_scan ?
			ipath->path.pathkeys : NIL;
		bool		twin = false;

		foreach(lc2, ordpaths)
		{
			IndexPath  *ord = (IndexPath *) lfirst(lc2);

			if (ord->indexinfo == ipath->indexinfo &&
				ord->indexscandir == ipath->indexscandir &&
				compare_pathkeys(ord->path.pathkeys,
								 ipath->path.pathkeys) == PATHKEYS_EQUAL)
				twin = true;
		}
		if (twin)
			continue;
		foreach(lc2, cands)
		{
			Path	   *lion = (Path *) lfirst(lc2);
			List	   *lionrinfos = NIL;
			bool		lossy = false;
			List	   *lionqual;
			List	   *residual;
			List	   *iocols;
			int			workers;
			Cost		startup;
			Cost		total;
			double		reads;

			if (!lo_lion_adds(lion, ipath->indexclauses))
				continue;
			lionqual = lo_lion_qual(lion, &lionrinfos, &lossy);
			residual = lo_residual(rel->baserestrictinfo, ordrinfos,
								   lionrinfos, lionqual);
			if (!lo_index_only(ipath->indexinfo,
							   lo_needed_attrs(rel, residual,
											   ipath->indexclauses,
											   lionqual, lossy),
							   &iocols))
				continue;
			lo_cost(root, rel, ipath, lion, lionrinfos, lionqual, residual,
					rows, true, 1.0, 1.0, ipath->path.param_info != NULL,
					NULL, &startup, &total, &reads, NULL);
			lo_cold_price(&cold, reads, &startup, &total);
			lo_add_path(rel, NULL, pathkeys != NIL ?
						&lion_ordered_path_methods : &lion_btree_path_methods,
						pathkeys, list_make3(ipath, lion, iocols),
						rows, startup, total, rel->consider_parallel, 0);

			/* ... and its walk divided among a parallel plan's participants */
			if (lo_parallel_ok(rel, ipath) &&
				lo_cost(root, rel, ipath, lion, lionrinfos, lionqual, residual,
						rows, true, 1.0, 1.0, ipath->path.param_info != NULL,
						&workers, &startup, &total, &reads, NULL))
			{
				lo_cold_price_partial(&cold, reads, rows, &startup, &total);
				lo_add_path(rel, NULL, pathkeys != NIL ?
							&lion_ordered_path_methods : &lion_btree_path_methods,
							pathkeys, list_make3(ipath, lion, iocols),
							rows, startup, total, true, workers);
			}
		}
	}

	/*
	 * A covering btree's paths parameterized by the outer side of a nested
	 * loop (DESIGN.md §40.10): its walk as the loop's probe, each scan a
	 * descent for the outer row's key, over a set built from the relation's
	 * restriction clauses alone - never from a join clause, which stays the
	 * btree's index clause or the node's filter - and kept for every scan.
	 * Paired with each lion access as the unordered paths are, priced as a
	 * probe in index-only mode, and offered claiming no order: an inner
	 * side's order is no use to a nested loop.
	 */
	foreach(lc, probepaths)
	{
		IndexPath  *ipath = (IndexPath *) lfirst(lc);
		ParamPathInfo *ppi = get_baserel_parampathinfo(root, rel,
													   PATH_REQ_OUTER(&ipath->path));
		List	   *ordrinfos = lo_ord_rinfos(ipath);
		List	   *clauses = list_concat_copy(rel->baserestrictinfo,
											   ppi->ppi_clauses);
		double		loops;
		double		scans = lo_probe_scans(root, rel, ppi, &loops);

		foreach(lc2, cands)
		{
			Path	   *lion = (Path *) lfirst(lc2);
			List	   *lionrinfos = NIL;
			bool		lossy = false;
			List	   *lionqual;
			List	   *residual;
			List	   *iocols;
			Cost		startup;
			Cost		total;

			if (!lo_lion_adds(lion, ipath->indexclauses))
				continue;
			lionqual = lo_lion_qual(lion, &lionrinfos, &lossy);
			residual = lo_residual(clauses, ordrinfos, lionrinfos, lionqual);
			if (!lo_index_only(ipath->indexinfo,
							   lo_needed_attrs(rel, residual,
											   ipath->indexclauses,
											   lionqual, lossy),
							   &iocols))
				continue;
			lo_cost(root, rel, ipath, lion, lionrinfos, lionqual, residual,
					ppi->ppi_rows, true, loops, scans,
					ipath->path.param_info != NULL, NULL, &startup, &total,
					NULL, NULL);
			lo_add_path(rel, ppi, &lion_btree_path_methods, NIL,
						list_make3(ipath, lion, iocols), ppi->ppi_rows,
						startup, total, rel->consider_parallel, 0);
		}
	}

	/*
	 * A lion column's walk takes the lion accesses core builds for every
	 * clause but the range it is bounded by - collecting that range into the
	 * set would read all of it, which the walk exists not to do - and no
	 * access at all: then every row the walk meets is fetched and the
	 * clauses are its filter.  The price decides (§30.11, "Cost").
	 */
	foreach(lc, walks)
	{
		LoWalk	   *w = (LoWalk *) lfirst(lc);
		List	   *wcands = lo_lion_accesses(root, rel, lionidx, w->rinfos,
											  lionam);
		List	   *walkinfo = list_make4_int(LO_WALK_MAGIC, w->indexcol,
											  w->backward ? 1 : 0, w->nulls);

		wcands = lappend(wcands, NULL);
		foreach(lc2, wcands)
		{
			Path	   *lion = (Path *) lfirst(lc2);
			List	   *lionrinfos = NIL;
			bool		lossy = false;
			List	   *lionqual = (lion != NULL) ?
				lo_lion_qual(lion, &lionrinfos, &lossy) : NIL;
			List	   *residual = lo_residual(rel->baserestrictinfo, w->rinfos,
											   lionrinfos, lionqual);
			Cost		startup;
			Cost		total;
			List	   *priv;

			lo_cost_walk(root, rel, w, lion, lionqual, residual, rows,
						 &startup, &total);
			priv = list_make5(walkinfo, w->index, w->rinfos, w->quals,
							  list_make1(w->var));
			priv = lappend(priv, lion);
			priv = lappend(priv, w->vrinfos);
			lo_add_path(rel, NULL, &lion_ordered_path_methods,
						list_make1(w->pathkey), priv, rows, startup, total,
						false, 0);
		}
	}
}

/*
 * set_rel_pathlist_hook: LionOrdered's paths (lo_set_rel_pathlist()), and
 * under pg_lion.vector_postfilter exact or error no plain scan of a lionvec
 * order that a Filter would follow (lo_knn_take(); DESIGN.md §44.6,
 * "Post-filtering").  pg_lion.enable_ordered_scan = off takes LionOrdered
 * away, not the rule: such a query then gets an exact plan, or the error.
 * Under error, the default, a relation whose lionvec order had to go, with
 * no LionOrdered over it and no other lionvec order left (a partial index's,
 * whose predicate implies every clause), fails the query here - except
 * while 16's old_snapshot_threshold is set, when no lionvec order is read
 * at all (DESIGN.md §9; lionveccostestimate() prices each out), so none is
 * filtered after its scan and the exact plan is what is left.  The partial
 * paths of the orders left then get their parallel cost (lo_knn_partial()).
 * Before all that, under every setting, a lionvec order of a table with
 * row-level security fails the query (lionvec_refuse_rls()).
 */
static void
lion_ordered_set_rel_pathlist(PlannerInfo *root, RelOptInfo *rel, Index rti,
							  RangeTblEntry *rte)
{
	LoKnn		knn;
	IndexPath  *first;

	/* core's price of a skewed key's scan, before lion's paths join it */
	lion_skew_rel_note(root, rel);
	if (lion_prev_set_rel_pathlist_hook != NULL)
		lion_prev_set_rel_pathlist_hook(root, rel, rti, rte);

	if (rte->rtekind == RTE_RELATION && (first = lo_knn_first(rel)) != NULL)
	{
		const char *index = get_rel_name(first->indexinfo->indexoid);

		lionvec_refuse_rls(rte->relid, index != NULL ? index : "?");
	}

	memset(&knn, 0, sizeof(knn));
	knn.why = LO_KNN_NONE;
	if (lionvec_postfilter != LIONVEC_POSTFILTER_ALLOW)
		lo_knn_take(root, rel, &knn);
	lo_knn_partial(root, rel);
	lo_set_rel_pathlist(root, rel, rte, &knn);
	if (knn.taken != NIL && knn.offered == 0 &&
		lionvec_postfilter == LIONVEC_POSTFILTER_ERROR && !lo_knn_kept(rel) &&
		!lion_old_snapshot_threshold_active())
		lo_knn_error(root, rel, rte, &knn);
}

/* ---------------------------------------------------------------------
 * The sides of a LionOrderedJoin (lion_ordjoin.c, DESIGN.md §47): the
 * relation's tests, its ordered btrees and lion accesses, and the prices,
 * as LionOrdered's own paths take them, for the join's planner.
 * --------------------------------------------------------------------- */

/*
 * May rel be a side of the join: a base relation LionOrdered may scan
 * (lo_rel_ok()), of the heap AM, and lion indexes read at all (§9)?
 */
bool
lion_ordered_rel_ok(PlannerInfo *root, RelOptInfo *rel)
{
	RangeTblEntry *rte;
	Relation	relation;
	bool		supported;

	if (rel->reloptkind != RELOPT_BASEREL || rel->rtekind != RTE_RELATION)
		return false;
	if (lion_old_snapshot_threshold_active())
		return false;
	rte = planner_rt_fetch(rel->relid, root);
	if (!lo_rel_ok(root, rel, rte))
		return false;
	relation = table_open(rte->relid, NoLock);
	supported = lion_table_am_supported(relation);
	table_close(relation, NoLock);
	return supported;
}

/*
 * rel's ordered btree paths, as LionOrdered takes them (§30.2, step 1):
 * core's unparameterized IndexPaths with pathkeys, and those LionOrdered's
 * own paths walk, which add_path() may have let go of in their favour.
 */
List *
lion_ordered_orders(RelOptInfo *rel)
{
	List	   *result = NIL;
	ListCell   *lc;

	foreach(lc, rel->pathlist)
	{
		Path	   *p = (Path *) lfirst(lc);
		IndexPath  *ipath = NULL;

		if (p->param_info != NULL || p->pathkeys == NIL)
			continue;
		if (IsA(p, IndexPath))
			ipath = (IndexPath *) p;
		else if (IsA(p, CustomPath) &&
				 ((CustomPath *) p)->methods == &lion_ordered_path_methods &&
				 IsA(linitial(((CustomPath *) p)->custom_private), IndexPath))
			ipath = (IndexPath *) linitial(((CustomPath *) p)->custom_private);
		if (ipath == NULL || ipath->path.pathkeys == NIL ||
			ipath->path.param_info != NULL ||
			ipath->indexorderbys != NIL ||
			ipath->indexinfo->relam != BTREE_AM_OID ||
			ipath->indexinfo->hypothetical || !lo_indexpath_ok(ipath) ||
			list_member_ptr(result, ipath))
			continue;
		result = lappend(result, ipath);
	}
	return result;
}

/*
 * rel's ordered btree paths for an order the query's own does not lead with,
 * want - a list of pathkeys: core's index paths, as lion_ordered_orders()
 * takes them, built on a scratch copy of rel that sees only its btrees with
 * want as the query's pathkeys and its sort pathkeys while they are built -
 * truncate_useless_pathkeys() judges an order by the one or the other,
 * depending on the major - so that the orders create_index_paths() keeps are
 * those useful for want.  For a LionOrderedSplit (lion_ordjoin.c,
 * DESIGN.md §48), whose arms walk the fact in the order the ORDER BY has
 * after its first key, a column of another table: core builds no ordered
 * path of the fact for it, its order useless to the query.
 */
List *
lion_ordered_orders_for(PlannerInfo *root, RelOptInfo *rel, List *want)
{
	List	   *btrees = NIL;
	List	   *savedquery = root->query_pathkeys;
	List	   *savedsort = root->sort_pathkeys;
	List	   *result = NIL;
	RelOptInfo *scratch;
	ListCell   *lc;

	foreach(lc, rel->indexlist)
	{
		IndexOptInfo *idx = (IndexOptInfo *) lfirst(lc);

		if (idx->relam == BTREE_AM_OID && !idx->hypothetical)
			btrees = lappend(btrees, idx);
	}
	if (btrees == NIL || want == NIL)
		return NIL;
	scratch = lo_scratch_rel(rel, btrees, NIL, false);
	root->query_pathkeys = want;
	root->sort_pathkeys = want;
	create_index_paths(root, scratch);
	root->query_pathkeys = savedquery;
	root->sort_pathkeys = savedsort;

	foreach(lc, scratch->pathlist)
	{
		Path	   *p = (Path *) lfirst(lc);
		IndexPath  *ipath;

		if (!IsA(p, IndexPath) || p->param_info != NULL ||
			p->pathkeys == NIL)
			continue;
		ipath = (IndexPath *) p;
		if (ipath->indexorderbys != NIL ||
			ipath->indexinfo->relam != BTREE_AM_OID ||
			!lo_indexpath_ok(ipath))
			continue;
		result = lappend(result, ipath);
	}
	return result;
}

/* rel's lion accesses for all its restriction clauses (§30.2, step 2) */
List *
lion_ordered_accesses(PlannerInfo *root, RelOptInfo *rel)
{
	Oid			lionam = lion_get_am_oid();
	List	   *lionidx = NIL;
	ListCell   *lc;

	if (!OidIsValid(lionam) || rel->baserestrictinfo == NIL)
		return NIL;
	foreach(lc, rel->indexlist)
	{
		IndexOptInfo *idx = (IndexOptInfo *) lfirst(lc);

		if (idx->relam == lionam && !idx->hypothetical)
			lionidx = lappend(lionidx, idx);
	}
	if (lionidx == NIL)
		return NIL;
	return lo_lion_accesses(root, rel, lionidx, NIL, lionam);
}

/*
 * The lion index of rel, and its key column, whose directory's order gives
 * the pathkey want (lo_column_gives(), DESIGN.md §30.11), for a
 * LionOrderedSplit's walk of the column (lion_ordjoin.c, §48): *indexcol the
 * column, 0-based, *var the table's.  NULL when none does.  A partial index
 * holds every row the query reads only when the query implies its predicate
 * (predOK), as for the walk of §30.11.
 */
IndexOptInfo *
lion_ordered_column_order(PlannerInfo *root, RelOptInfo *rel, PathKey *want,
						  int *indexcol, Var **var)
{
	RangeTblEntry *rte = planner_rt_fetch(rel->relid, root);
	Oid			lionam = lion_get_am_oid();
	ListCell   *lc;

	if (!OidIsValid(lionam))
		return NULL;
	foreach(lc, rel->indexlist)
	{
		IndexOptInfo *idx = (IndexOptInfo *) lfirst(lc);
		Relation	indexrel = NULL;
		int			c;
		int			found = -1;

		if (idx->relam != lionam || idx->hypothetical ||
			(idx->indpred != NIL && !idx->predOK))
			continue;
		for (c = 0; c < idx->nkeycolumns && found < 0; c++)
		{
			if (lo_column_gives(root, rel, rte, idx, &indexrel, c, want, var))
				found = c;
		}
		if (indexrel != NULL)
			index_close(indexrel, AccessShareLock);
		if (found >= 0)
		{
			*indexcol = found;
			return idx;
		}
	}
	return NULL;
}

/*
 * The set of one lion access of a probed side - or none, lion NULL: the
 * restriction clauses left for the side's filter, the set's price as a nested
 * loop's probe builds it (lion_ordered_set_price(), and its spill), the share
 * of rel's rows it holds, the price of a member's recheck when it leaves
 * quals to it (lion_ordered_set_exact()), and the lion index pages its build
 * reads (lo_lion_pages()).
 */
void
lion_ordered_set_info(PlannerInfo *root, RelOptInfo *rel, Path *lion,
					  LionOrderedSet *out)
{
	List	   *lionrinfos = NIL;
	List	   *lionqual = NIL;
	bool		lossy = false;

	memset(out, 0, sizeof(LionOrderedSet));
	out->lion = lion;
	out->candsel = 1.0;
	if (lion != NULL)
	{
		double		members;
		double		bytes;

		lionqual = lo_lion_qual(lion, &lionrinfos, &lossy);
		out->build = lion_ordered_set_price(root, rel, lion, &members, &bytes) +
			lion_ordered_spill_price(bytes);
		out->candsel = Min(members / Max(rel->tuples, 1.0), 1.0);
		out->lionpages = lo_lion_pages(lion);
		if (!lion_ordered_set_exact(root, lion))
		{
			QualCost	rq;

			cost_qual_eval(&rq, lionqual, root);
			out->recheck = rq.per_tuple;
		}
	}
	out->residual = lo_residual(rel->baserestrictinfo, NIL, lionrinfos,
								lionqual);
}

/*
 * The price of the fact's walk of ord with one lion access, or none (lion
 * NULL, every entry walked fetched and filtered): lo_cost() - the set's
 * build, its lookups as a probe's build reads them (§47.3), the walk, the
 * members' fetches, the filter and the target - in heap mode, or in
 * index-only mode where the btree has every column the side needs and
 * indexonly_ok (*iocols is then its mapping, NIL otherwise).  *fetches is the
 * heap fetches of the whole walk (lo_cost()), *lionpages the lion index pages
 * the set's build reads.
 */
void
lion_ordered_walk_price(PlannerInfo *root, RelOptInfo *rel, IndexPath *ord,
						Path *lion, double rows, bool indexonly_ok,
						List **iocols, Cost *startup, Cost *total,
						double *fetches, double *lionpages)
{
	List	   *lionrinfos = NIL;
	bool		lossy = false;
	List	   *lionqual;
	List	   *residual;
	bool		indexonly = false;

	*iocols = NIL;
	lionqual = (lion != NULL) ? lo_lion_qual(lion, &lionrinfos, &lossy) : NIL;
	residual = lo_residual(rel->baserestrictinfo, lo_ord_rinfos(ord),
						   lionrinfos, lionqual);
	if (indexonly_ok && enable_indexonlyscan)
		indexonly = lo_index_only(ord->indexinfo,
								  lo_needed_attrs(rel, residual,
												  ord->indexclauses,
												  lionqual, lossy),
								  iocols);
	if (!indexonly)
		*iocols = NIL;
	(void) lo_cost(root, rel, ord, lion, lionrinfos, lionqual, residual, rows,
				   indexonly, 1.0, 1.0, true, NULL, startup, total, NULL,
				   fetches);
	*lionpages = lo_lion_pages(lion);
}

/*
 * The pages a LionOrdered walk of a btree reads over `share` of it, as a
 * competitor's (DESIGN.md §47.3, "Cold reads"): a core nested loop over the
 * walk, which a LionOrderedJoin is weighed against.  lo_cost() is taken again
 * for the path's btree and lion access as lo_set_rel_pathlist() took it: the
 * heap pages of its fetches, `share` of the whole walk's
 * (index_pages_fetched() over the table's pages), and the lion index pages
 * its set's build reads (lo_lion_pages()) - the reads lo_cold_price() weighs,
 * less the btree's, which the join's own walk reads as well and which are
 * left out of both.  An index-only walk's price may already carry
 * lo_cold_price()'s discount, its reads weighed cold against a bitmap heap
 * scan's (lion_units_scan_cold()): where it does, *reads is 0, and those
 * reads are not weighed a second time.  False for a path that is not a serial
 * LionOrdered walk of a btree.
 */
bool
lion_ordered_walk_reads(PlannerInfo *root, Path *path, double share,
						double *reads)
{
	CustomPath *cp;
	RelOptInfo *rel;
	IndexPath  *ord;
	Path	   *lion;
	List	   *iocols;
	List	   *lionrinfos = NIL;
	bool		lossy = false;
	List	   *lionqual;
	List	   *residual;
	bool		indexonly;
	Cost		startup;
	Cost		total;
	double		walkreads;
	double		fetches;

	*reads = 0.0;
	if (path == NULL || !IsA(path, CustomPath))
		return false;
	cp = (CustomPath *) path;
	rel = cp->path.parent;
	if (cp->methods != &lion_ordered_path_methods || cp->path.parallel_aware ||
		rel == NULL || list_length(cp->custom_private) != 3 ||
		!IsA(linitial(cp->custom_private), IndexPath))
		return false;
	ord = (IndexPath *) linitial(cp->custom_private);
	lion = (Path *) lsecond(cp->custom_private);
	iocols = (List *) lthird(cp->custom_private);
	if (lo_knn_path(ord))
		return false;
	indexonly = (iocols != NIL);
	lionqual = (lion != NULL) ? lo_lion_qual(lion, &lionrinfos, &lossy) : NIL;
	residual = lo_residual(rel->baserestrictinfo, lo_ord_rinfos(ord),
						   lionrinfos, lionqual);
	(void) lo_cost(root, rel, ord, lion, lionrinfos, lionqual, residual,
				   cp->path.rows, indexonly, 1.0, 1.0,
				   ord->path.param_info != NULL, NULL, &startup, &total,
				   &walkreads, &fetches);
	if (indexonly)
	{
		LionUnits	cold;

		lion_units_scan_cold(root, rel, &cold);
		if (lion_units_bitmap_factor(&cold, total, walkreads) < 1.0)
			return true;
	}
	*reads = index_pages_fetched(Max(share, 0.0) * fetches, rel->pages,
								 Max((double) ord->indexinfo->pages, 1.0),
								 root) +
		lo_lion_pages(lion);
	return true;
}

/*
 * A side of the join as a LionOrdered path of rel, which is never offered to
 * rel's path list: the fact's ordered walk (`weight` the share of it the
 * join's LIMIT is expected to take), or the dimension's probe of its unique
 * btree (`weight` the descents the join is expected to make, keyqual the
 * `pk = NULL` it is descended by).  Serial, unparameterized; a probe in heap
 * mode.
 */
CustomPath *
lion_ordered_side_path(RelOptInfo *rel, IndexPath *ord, Path *lion,
					   List *iocols, bool dim, double weight, Expr *keyqual,
					   double rows, Cost startup, Cost total)
{
	CustomPath *cp = makeNode(CustomPath);
	List	   *info;

	Assert(dim ? (keyqual != NULL && iocols == NIL) : keyqual == NULL);
	info = list_make3(list_make2_int(LO_SIDE_MAGIC,
									 dim ? LO_SIDE_DIM : LO_SIDE_FACT),
					  makeConst(FLOAT8OID, -1, InvalidOid, sizeof(float8),
								Float8GetDatum(weight), false,
								FLOAT8PASSBYVAL),
					  keyqual);
	cp->path.pathtype = T_CustomScan;
	cp->path.parent = rel;
	cp->path.pathtarget = rel->reltarget;
	cp->path.param_info = NULL;
	cp->path.parallel_aware = false;
	cp->path.parallel_safe = false;
	cp->path.parallel_workers = 0;
	cp->path.rows = rows;
	cp->path.startup_cost = startup;
	cp->path.total_cost = total;
	cp->path.pathkeys = dim ? NIL : ord->path.pathkeys;
	cp->flags = CUSTOMPATH_SUPPORT_PROJECTION;
	cp->custom_paths = NIL;
	cp->custom_private = list_make4(ord, lion, iocols, info);
	cp->methods = dim ? &lion_btree_path_methods : &lion_ordered_path_methods;
	return cp;
}

/*
 * The path's pathkeys as sort keys over plain columns of rel, for the
 * fetch-and-sort switch, into p; none (hassort false) when one of them is
 * not a plain column (an expression index's order: its functions would be
 * evaluated where the ordinary plan calls none, which EXECUTE could tell
 * apart, §30.6).
 */
static void
lo_sort_keys(RelOptInfo *rel, List *pathkeys, LoPriv *p)
{
	int			n = list_length(pathkeys);
	ListCell   *lc;

	p->hassort = false;
	p->nsort = 0;
	p->sortattno = (AttrNumber *) palloc(sizeof(AttrNumber) * Max(n, 1));
	p->sortop = (Oid *) palloc(sizeof(Oid) * Max(n, 1));
	p->sortcoll = (Oid *) palloc(sizeof(Oid) * Max(n, 1));
	p->nullsfirst = (bool *) palloc(sizeof(bool) * Max(n, 1));
	foreach(lc, pathkeys)
	{
		PathKey    *pk = (PathKey *) lfirst(lc);
		EquivalenceClass *ec = pk->pk_eclass;
		Var		   *var = NULL;
		Oid			type = InvalidOid;
		Oid			op;
		bool		desc;
		EquivalenceMember *em;
#if PG_VERSION_NUM >= 180000
		EquivalenceMemberIterator it;
#else
		ListCell   *lc2;
#endif

		/*
		 * rel's own member of the class: a partition's is a child member
		 * (§30.11), which 18 keeps apart from the parent's.
		 */
#if PG_VERSION_NUM >= 180000
		setup_eclass_member_iterator(&it, ec, rel->relids);
		while ((em = eclass_member_iterator_next(&it)) != NULL)
#else
		foreach(lc2, ec->ec_members)
#endif
		{
			Node	   *e;

#if PG_VERSION_NUM < 180000
			em = (EquivalenceMember *) lfirst(lc2);
#endif
			e = (Node *) em->em_expr;
			if (em->em_is_const || !bms_equal(em->em_relids, rel->relids))
				continue;
			while (IsA(e, RelabelType))
				e = (Node *) ((RelabelType *) e)->arg;
			if (IsA(e, Var) && ((Var *) e)->varno == (int) rel->relid &&
				((Var *) e)->varattno > 0 && ((Var *) e)->varlevelsup == 0)
			{
				var = (Var *) e;
				type = em->em_datatype;
				break;
			}
		}
		if (var == NULL)
		{
			p->nsort = 0;
			return;
		}
#if PG_VERSION_NUM >= 180000
		desc = (pk->pk_cmptype == COMPARE_GT);
#else
		desc = (pk->pk_strategy == BTGreaterStrategyNumber);
#endif
		op = get_opfamily_member(pk->pk_opfamily, type, type,
								 desc ? BTGreaterStrategyNumber : BTLessStrategyNumber);
		if (!OidIsValid(op))
		{
			p->nsort = 0;
			return;
		}
		p->sortattno[p->nsort] = var->varattno;
		p->sortop[p->nsort] = op;
		p->sortcoll[p->nsort] = ec->ec_collation;
		p->nullsfirst[p->nsort] = pk->pk_nulls_first;
		p->nsort++;
	}
	p->hassort = true;
}

/*
 * The most rows a scan of the node can be asked for, or -1 (LO_PRIV_LIMIT):
 * the rows its query level's LIMIT takes, OFFSET included, when nothing
 * between the node and that Limit can drop a row the node returns or read
 * one past them, for the fetch-and-sort switch's sort keeps no more than
 * that (DESIGN.md §30.4).  So the LIMIT and the OFFSET are constants -
 * root->limit_tuples is an estimate, a guess at a Param or a stable
 * function - and not WITH TIES, which takes the rows tied with the last one
 * too; the query level has no grouping, aggregate, DISTINCT, window or
 * set-returning function, or limit_tuples is -1, and no other relation, nor
 * row marks, whose LockRows skips a row it cannot lock; the node has no
 * filter of its own, whose rejects the Limit does not count; and the node's
 * order is the query's, so that no Sort or Incremental Sort above it reads
 * past the rows it returns.  A partition's walk is under a MergeAppend or an
 * Append, which reads no more of one child than it returns.
 */
static int64
lo_limit(PlannerInfo *root, CustomPath *path, List *filter)
{
	Query	   *parse = root->parse;

	if (root->limit_tuples <= 0 ||
		parse->limitCount == NULL || !IsA(parse->limitCount, Const) ||
		(parse->limitOffset != NULL && !IsA(parse->limitOffset, Const)) ||
		parse->limitOption == LIMIT_OPTION_WITH_TIES ||
		parse->rowMarks != NIL || root->rowMarks != NIL ||
		bms_membership(root->all_baserels) != BMS_SINGLETON ||
		filter != NIL ||
		!pathkeys_contained_in(root->sort_pathkeys, path->path.pathkeys))
		return -1;
	return root->limit_tuples >= (double) PG_INT64_MAX ?
		PG_INT64_MAX : (int64) ceil(root->limit_tuples);
}

static Plan *
lo_plan_path(PlannerInfo *root, RelOptInfo *rel, CustomPath *best_path,
			 List *tlist, List *clauses, List *custom_plans)
{
	CustomScan *cscan = makeNode(CustomScan);
	Path	   *lion;
	List	   *lionrinfos = NIL;
	bool		lossy = false;
	List	   *lionqual = NIL;
	List	   *residual;
	List	   *tree = NIL;
	List	   *leaves = NIL;
	List	   *lionquals = NIL;
	List	   *ordrinfos = NIL;
	List	   *ordquals = NIL;
	List	   *ordcols = NIL;
	List	   *ordorig = NIL;
	List	   *vrinfos = NIL;
	List	   *iocols = NIL;
	List	   *orderbys = NIL;
	double		expected = 0.0;
	int64		limit = -1;
	int			dir;
	int			flags = 0;
	int			nulls = LION_ORDER_NULLS_NONE;
	int			walkattno = 0;
	Oid			ordoid;
	LoPriv		priv;
	ListCell   *lc;
	int			side = 0;
	double		sideweight = 0.0;

	if (list_length(best_path->custom_private) == 4)
	{
		/* a side of a LionOrderedJoin (§47): its walk, and its side */
		List	   *info = (List *) lfourth(best_path->custom_private);
		List	   *head = (List *) linitial(info);
		Const	   *w = (Const *) lsecond(info);

		if (list_length(info) != 3 || !IsA(head, IntList) ||
			linitial_int(head) != LO_SIDE_MAGIC || !IsA(w, Const) ||
			!IsA(linitial(best_path->custom_private), IndexPath))
			elog(ERROR, "LionOrdered: unrecognized side of a join");
		side = lsecond_int(head);
		sideweight = DatumGetFloat8(w->constvalue);
		flags |= LO_FLAG_SIDE;
		if (side == LO_SIDE_DIM)
		{
			Expr	   *keyqual = (Expr *) lthird(info);

			if (keyqual == NULL || !IsA(keyqual, OpExpr))
				elog(ERROR, "LionOrdered: a join's dimension without its key");
			flags |= LO_FLAG_PROBE | LO_FLAG_KEYED;
			ordquals = list_make1(keyqual);
			ordcols = list_make1_int(0);
		}
		else if (side != LO_SIDE_FACT)
			elog(ERROR, "LionOrdered: unrecognized side %d of a join", side);
	}

	if (IsA(linitial(best_path->custom_private), IndexPath))
	{
		/*
		 * A btree's walk (DESIGN.md §30.2), its values from the index tuple
		 * when the planner found it covering (§40).
		 */
		IndexPath  *ord = (IndexPath *) linitial(best_path->custom_private);

		lion = (Path *) lsecond(best_path->custom_private);
		iocols = (List *) lthird(best_path->custom_private);
		if (iocols != NIL)
			flags |= LO_FLAG_INDEXONLY;
		ordrinfos = lo_ord_rinfos(ord);
		foreach(lc, ord->indexclauses)
		{
			IndexClause *iclause = lfirst_node(IndexClause, lc);
			ListCell   *lc2;

			ordorig = lappend(ordorig, iclause->rinfo->clause);
			foreach(lc2, iclause->indexquals)
			{
				RestrictInfo *rinfo = lfirst_node(RestrictInfo, lc2);

				ordquals = lappend(ordquals, rinfo->clause);
				ordcols = lappend_int(ordcols, iclause->indexcol);
			}
		}
		ordoid = ord->indexinfo->indexoid;
		dir = (int) ord->indexscandir;
		expected = ord->indexselectivity * rel->tuples;

		/*
		 * A lionvec index's order (DESIGN.md §44.6): its ORDER BY clause, and
		 * the set built at once, for the scan to be restricted to.
		 */
		if (lo_knn_path(ord))
		{
			if (linitial_int(ord->indexorderbycols) != 0)
				elog(ERROR, "LionOrdered: a lionvec order on column %d",
					 linitial_int(ord->indexorderbycols));
			orderbys = ord->indexorderbys;
			dir = (int) ForwardScanDirection;
			flags |= LO_FLAG_KNN | LO_FLAG_BUILD;

			/*
			 * The rows its query level's LIMIT takes, OFFSET included, when
			 * the planner knows them: root->limit_tuples, -1 for no LIMIT,
			 * one not constant at planning (a generic plan's Param), or one
			 * that does not reach the scan (grouping, aggregates, DISTINCT,
			 * window functions or set-returning functions above it); a
			 * subquery's root has its own.  The vector scan pauses at no
			 * tier below them, and gathers them in a pass when they are
			 * more than pg_lion.vector_batch (§44.4).  It is where to pause
			 * and what to gather, not a bound: a scan asked for more rows -
			 * a residual filter or a join above it taking fewer than it
			 * returns - returns more.
			 */
			if (root->limit_tuples > 0)
				limit = root->limit_tuples >= (double) PG_INT64_MAX ?
					PG_INT64_MAX : (int64) ceil(root->limit_tuples);
		}
	}
	else
	{
		/*
		 * A lion column's walk (DESIGN.md §30.11): its index, the column
		 * (1-based, as a scan key names it), its direction and where its
		 * NULL entry goes; its quals are the range it is bounded by.
		 */
		List	   *walkinfo = (List *) linitial(best_path->custom_private);
		IndexOptInfo *idx = (IndexOptInfo *) lsecond(best_path->custom_private);
		int			indexcol = lsecond_int(walkinfo);

		Assert(linitial_int(walkinfo) == LO_WALK_MAGIC);
		ordrinfos = (List *) lthird(best_path->custom_private);
		ordquals = (List *) lfourth(best_path->custom_private);
		lion = (Path *) list_nth(best_path->custom_private, 5);
		vrinfos = (List *) list_nth(best_path->custom_private, 6);
		foreach(lc, ordrinfos)
		{
			ordorig = lappend(ordorig, lfirst_node(RestrictInfo, lc)->clause);
			ordcols = lappend_int(ordcols, indexcol);
		}
		ordoid = idx->indexoid;
		dir = lthird_int(walkinfo) ? (int) BackwardScanDirection :
			(int) ForwardScanDirection;
		flags |= LO_FLAG_LIONWALK;
		nulls = lfourth_int(walkinfo);
		walkattno = indexcol + 1;
		expected = rel->tuples;
		if (ordrinfos != NIL)
			expected *= clauselist_selectivity(root, ordrinfos, rel->relid,
											   JOIN_INNER, NULL);
	}

	/*
	 * The set lazily only for a walk expected to stop short (§30.4): the
	 * share of its walk a LIMIT on the node alone lets it go, and the entries
	 * that comes to.  A parallel walk's set is built at once, for every
	 * participant (§40.9).  A nested loop's probe (§40.10) keeps its set for
	 * every scan the loop makes, and its walk is all of theirs: lazily while
	 * the loop's scans together are expected to meet fewer entries than a
	 * walk that would be built for - a few outer rows probing a few keys of
	 * the set - and built at once otherwise.
	 */
	if (side == LO_SIDE_DIM)
	{
		/*
		 * A join's dimension (§47): a probe, each descent meeting one entry,
		 * whose set is lazy while the join's descents are fewer than the
		 * entries a walk would be built for, as a nested loop's is.
		 */
		expected = 1.0;
		if (sideweight >= LO_SWITCH_MIN_WALK)
			flags |= LO_FLAG_BUILD;
	}
	else if (side == LO_SIDE_FACT)
	{
		/* a join's fact: the share of its walk the join's LIMIT takes */
		if (sideweight >= LO_LAZY_MAX_SHARE &&
			sideweight * expected >= LO_SWITCH_MIN_WALK)
			flags |= LO_FLAG_BUILD;
	}
	else if (best_path->path.param_info != NULL)
	{
		double		loops;

		flags |= LO_FLAG_PROBE;
		(void) lo_probe_scans(root, rel, best_path->path.param_info, &loops);
		if (loops * expected >= LO_SWITCH_MIN_WALK)
			flags |= LO_FLAG_BUILD;
	}
	else if (best_path->path.parallel_aware)
		flags |= LO_FLAG_BUILD;
	else
	{
		double		share = 1.0;

		if (root->limit_tuples > 0 &&
			bms_membership(root->all_baserels) == BMS_SINGLETON &&
			best_path->path.rows > 0)
			share = Min(1.0, root->limit_tuples / best_path->path.rows);
		if (share >= LO_LAZY_MAX_SHARE &&
			share * expected >= LO_SWITCH_MIN_WALK)
			flags |= LO_FLAG_BUILD;
	}

	if (lion != NULL)
	{
		lionqual = lo_lion_qual(lion, &lionrinfos, &lossy);
		lo_lion_tree(lion, &tree, &leaves, &lionquals);
		if (lossy)
			flags |= LO_FLAG_LOSSY;
	}
	residual = lo_residual(clauses, ordrinfos, lionrinfos, lionqual);

	/*
	 * The most rows a scan of the node can be asked for, which bound the
	 * fetch-and-sort switch's sort (§30.4); a lionvec order's LIMIT is its
	 * scan's pause, above, and never switches, nor does a parallel walk,
	 * which has no sort keys (below).
	 */
	if ((flags & LO_FLAG_KNN) == 0 && !best_path->path.parallel_aware)
		limit = lo_limit(root, best_path,
						 extract_actual_clauses(residual, false));

	/*
	 * Index-only mode: does the lion recheck of an inexact set need the
	 * heap tuple, the btree lacking a column of the lion qual (§40.4)?
	 */
	if (iocols != NIL)
	{
		Bitmapset  *lionattrs = NULL;
		Bitmapset  *returned = NULL;

		pull_varattnos((Node *) lionqual, rel->relid, &lionattrs);
		foreach(lc, iocols)
		{
			int			attno = lfirst_int(lc);

			if (attno > 0)
				returned = bms_add_member(returned,
										  attno - FirstLowInvalidHeapAttributeNumber);
		}
		if (!bms_is_subset(lionattrs, returned))
			flags |= LO_FLAG_HEAPRECHECK;
	}

	cscan->scan.plan.targetlist = tlist;
	cscan->scan.plan.qual = extract_actual_clauses(residual, false);
	cscan->scan.scanrelid = rel->relid;
	cscan->flags = best_path->flags;
	cscan->custom_plans = NIL;
	cscan->custom_scan_tlist = NIL;
	cscan->custom_exprs = list_make5(lionquals, ordquals, lionqual, ordorig,
									 extract_actual_clauses(vrinfos, false));
	cscan->custom_exprs = lappend(cscan->custom_exprs, orderbys);
	priv.ordoid = ordoid;
	priv.dir = dir;
	priv.flags = flags;
	priv.nulls = nulls;
	priv.walkattno = (AttrNumber) walkattno;
	priv.ordcols = ordcols;
	priv.tree = tree;
	priv.leaves = leaves;

	/*
	 * The fetch-and-sort switch's keys - none for a parallel walk, which
	 * never switches: a participant has met only its share of the members,
	 * and the members it has not met are the other participants' rows
	 * (DESIGN.md §40.9).
	 */
	if (best_path->path.parallel_aware)
	{
		priv.hassort = false;
		priv.nsort = 0;
	}
	else
		lo_sort_keys(rel, best_path->path.pathkeys, &priv);
	priv.iocols = iocols;
	priv.expected = expected;
	priv.limit = limit;
	cscan->custom_private = lo_priv_encode(&priv);
	cscan->methods = (best_path->methods == &lion_btree_path_methods) ?
		&lo_btree_scan_methods : &lo_scan_methods;

	return &cscan->scan.plan;
}

/*
 * ReparameterizeCustomPathByChild: a partitionwise join's nested loop probes
 * a partition of rel with a LionBtreeScan parameterized by the outer side's
 * parent, as every inner path of a partition is, and has core move it to the
 * outer partition (reparameterize_path_by_child()) - the path's own
 * parameterization, and through this its private list, of which only the
 * btree's IndexPath names the outer side: its join clauses, translated by
 * core's own reparameterization of an index path.  The lion access is the
 * partition's own and unparameterized (§40.10), the column mapping the
 * btree's.
 */
static List *
lo_reparameterize(PlannerInfo *root, List *custom_private,
				  RelOptInfo *child_rel)
{
	Path	   *ord;
	Path	   *moved;

	if (list_length(custom_private) != 3 ||
		!IsA(linitial(custom_private), IndexPath))
		elog(ERROR, "LionBtreeScan: unrecognized path to reparameterize");
	ord = (Path *) linitial(custom_private);
	moved = reparameterize_path_by_child(root, ord, child_rel);
	if (moved == NULL)
		elog(ERROR, "LionBtreeScan: could not reparameterize the walk of index %u",
			 ((IndexPath *) ord)->indexinfo->indexoid);
	return list_make3(moved, lsecond(custom_private), lthird(custom_private));
}

/* ---------------------------------------------------------------------
 * The set's memory (DESIGN.md §30.4, "Spilling")
 * --------------------------------------------------------------------- */

/*
 * Regions the arena may map: with each doubling the file to 64 MB, 1,024 of
 * them hold a set of 64 GB, a container for every key of a heap of 2^32
 * blocks and then some.
 */
#define LO_ARENA_REGIONS		1024

/* The arena's file is extended this much at a write. */
#define LO_ARENA_ZERO_BYTES		(64 * 1024)

/*
 * The arena asks for its file to be longer: written through fd.c, zeros,
 * so that temp_file_limit, log_temp_files and the temp-file statistics see
 * the bytes as they see a sort's, and so that every byte mapped is on disk
 * before the first write to its page - a page the disk had no room for
 * would end the backend with SIGBUS at that write.
 */
static bool
lo_arena_extend(void *arg, Size from, Size to)
{
	LionOrderedState *st = (LionOrderedState *) arg;
	static const char zeros[LO_ARENA_ZERO_BYTES];
	Size		off = from;

	while (off < to)
	{
		Size		n = Min((Size) LO_ARENA_ZERO_BYTES, to - off);
		int			w = FileWrite(st->arenafile, zeros, n, (off_t) off,
								  WAIT_EVENT_BUFFILE_WRITE);

		if (w <= 0)
		{
			if (w == 0)
				errno = ENOSPC;
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not write to temporary file of LionOrdered's set: %m")));
		}
		off += (Size) w;
	}
	pgBufferUsage.temp_blks_written += (to - from) / BLCKSZ;
	return true;
}

/* ... and for its descriptor, opened again if fd.c closed it meanwhile. */
static int
lo_arena_fd(void *arg)
{
	LionOrderedState *st = (LionOrderedState *) arg;

	if (FileSize(st->arenafile) < 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not access temporary file of LionOrdered's set: %m")));
	return FileGetRawDesc(st->arenafile);
}

/*
 * The mappings go with the query's memory, however the query ends; the file
 * with its resource owner, as a sort's would, unless lo_end() closed it.
 */
static void
lo_arena_release_cb(void *arg)
{
	LionOrderedState *st = (LionOrderedState *) arg;

	if (st->arena != NULL)
		lion_arena_release(st->arena);
}

/*
 * This build's set has outgrown hash_mem: from here on it lives in the
 * arena, over a temporary file, which the first spill of the node sets up.
 */
static void
lo_spill_begin(LionOrderedState *st)
{
	if (st->arena == NULL)
	{
		EState	   *estate = st->css.ss.ps.state;
		MemoryContext oldcxt = MemoryContextSwitchTo(estate->es_query_cxt);
		LionArena  *arena = (LionArena *) palloc0(sizeof(LionArena));
		LionArenaRegion *regions = (LionArenaRegion *)
			palloc(sizeof(LionArenaRegion) * LO_ARENA_REGIONS);

		MemoryContextSwitchTo(oldcxt);
		lion_arena_init(arena, regions, LO_ARENA_REGIONS, lo_arena_extend,
						lo_arena_fd, st);
		/* registered before anything is mapped: it finds nothing to undo */
		st->arenacb.func = lo_arena_release_cb;
		st->arenacb.arg = st;
		MemoryContextRegisterResetCallback(estate->es_query_cxt, &st->arenacb);
		st->arenafile = OpenTemporaryFile(false);
		st->arena = arena;
	}
	st->spilled = true;
}

/*
 * The set's memory: in cxt, charged against hash_mem, while the charge
 * stays within it; past that, for the rest of this build, in the arena,
 * which hash_mem does not count (DESIGN.md §30.4, "Spilling").
 */
static void *
lo_set_alloc(LionOrderedState *st, MemoryContext cxt, Size size)
{
	void	   *p;

	if (!st->spilled)
	{
		if (st->bytes + size <= st->limit)
		{
			st->bytes += size;
			return MemoryContextAllocExtended(cxt, size, MCXT_ALLOC_HUGE);
		}
		lo_spill_begin(st);
	}
	p = lion_arena_alloc(st->arena, size);
	if (p == NULL)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not spill LionOrdered's set to a temporary file: %m")));
	return p;
}

/* Give back `size` bytes lo_set_alloc() handed out, wherever they are. */
static void
lo_set_free(LionOrderedState *st, void *p, Size size)
{
	if (st->arena != NULL && lion_arena_contains(st->arena, p))
		lion_arena_free(st->arena, p, size);
	else
	{
		Assert(st->bytes >= size);
		st->bytes -= size;
		pfree(p);
	}
}

/* A block of newsize bytes with the first oldsize bytes of p, p given back. */
static void *
lo_set_grow(LionOrderedState *st, MemoryContext cxt, void *p, Size oldsize,
			Size newsize)
{
	void	   *q = lo_set_alloc(st, cxt, newsize);

	memcpy(q, p, oldsize);
	lo_set_free(st, p, oldsize);
	return q;
}

/* ---------------------------------------------------------------------
 * The TID set
 * --------------------------------------------------------------------- */

static LionTidSet *
lo_set_new(LionOrderedState *st)
{
	LionTidSet *set = (LionTidSet *) MemoryContextAllocZero(st->setcxt,
															sizeof(LionTidSet));

	set->cap = 16;
	set->keys = (uint32 *) lo_set_alloc(st, st->setcxt,
										sizeof(uint32) * set->cap);
	set->conts = (LionContainer **)
		lo_set_alloc(st, st->setcxt, sizeof(LionContainer *) * set->cap);
	st->live = lappend(st->live, set);
	return set;
}

static void
lo_set_drop(LionOrderedState *st, LionTidSet *set)
{
	int			i;

	for (i = 0; i < set->n; i++)
		lo_set_free(st, set->conts[i], lion_container_size(set->conts[i]));
	lo_set_free(st, set->keys, sizeof(uint32) * set->cap);
	lo_set_free(st, set->conts, sizeof(LionContainer *) * set->cap);
	st->live = list_delete_ptr(st->live, set);
	pfree(set);
}

/*
 * Append (ckey, c) to the set, in ascending key: a sorted source's
 * containers come so (lion_source_sorted()), a leaf's gathered keys are
 * taken in order (lo_gather_finish()), and lo_set_combine()'s come merged
 * from two sets in order.  c is the set's own, from lo_set_alloc().
 */
static void
lo_set_append(LionOrderedState *st, LionTidSet *set, uint32 ckey,
			  LionContainer *c)
{
	Assert(set->n == 0 || ckey > set->keys[set->n - 1]);
	Assert(c != NULL);
	if (set->n == set->cap)
	{
		int			cap = set->cap * 2;

		set->keys = (uint32 *) lo_set_grow(st, st->setcxt, set->keys,
										   sizeof(uint32) * set->cap,
										   sizeof(uint32) * cap);
		set->conts = (LionContainer **)
			lo_set_grow(st, st->setcxt, set->conts,
						sizeof(LionContainer *) * set->cap,
						sizeof(LionContainer *) * cap);
		set->cap = cap;
	}
	set->keys[set->n] = ckey;
	set->conts[set->n] = c;
	set->n++;
}

/* Append (ckey, a copy of c). */
static void
lo_set_push(LionOrderedState *st, LionTidSet *set, uint32 ckey,
			const LionContainer *c)
{
	Size		size = lion_container_size(c);
	LionContainer *copy = (LionContainer *) lo_set_alloc(st, st->setcxt, size);

	memcpy(copy, c, size);
	lo_set_append(st, set, ckey, copy);
}

/*
 * OR c into the set's last container, whose key it has: a sorted source
 * is not expected to name a key twice, but a set must hold each key once
 * (lo_set_find()), so one that does is taken in rather than refused.
 */
static void
lo_set_or_last(LionOrderedState *st, LionTidSet *set, const LionContainer *c)
{
	LionContainer *last = set->conts[set->n - 1];
	uint64	   *img;
	LionContainer *out;
	LionContainer *copy;
	Size		size;

	Assert(set->n > 0 && c->ckey == set->keys[set->n - 1]);
	img = (uint64 *) MemoryContextAllocZero(st->buildcxt, LION_BITSET_BYTES);
	out = (LionContainer *) MemoryContextAlloc(st->buildcxt,
											   LION_CONTAINER_MAX_SIZE);
	lion_container_or_into_bitset(last, img);
	lion_container_or_into_bitset(c, img);
	lion_bits_to_container(img, c->ckey, out);
	size = lion_container_size(out);
	lo_set_free(st, last, lion_container_size(last));
	copy = (LionContainer *) lo_set_alloc(st, st->setcxt, size);
	memcpy(copy, out, size);
	set->conts[set->n - 1] = copy;
	pfree(img);
	pfree(out);
}

/* ---------------------------------------------------------------------
 * Gathering a leaf's pieces (LoGather)
 * --------------------------------------------------------------------- */

/*
 * The chunks a gather has room for at first, a pointer and a word each: the
 * room doubles as keys need, a few times for the largest heap.
 */
#define LO_GATHER_FIRST_CHUNKS	1

static LoGather *
lo_gather_new(LionOrderedState *st)
{
	LoGather   *g = (LoGather *) MemoryContextAlloc(st->buildcxt,
													sizeof(LoGather));

	g->nchunks = LO_GATHER_FIRST_CHUNKS;
	g->maxchunk = 0;
	g->chunks = (LoGatherSlot **)
		lo_set_alloc(st, st->buildcxt, sizeof(LoGatherSlot *) * g->nchunks);
	memset(g->chunks, 0, sizeof(LoGatherSlot *) * g->nchunks);
	g->touched = (uint64 *) lo_set_alloc(st, st->buildcxt,
										 sizeof(uint64) * g->nchunks);
	memset(g->touched, 0, sizeof(uint64) * g->nchunks);
	g->img = (uint64 *) MemoryContextAlloc(st->buildcxt, LION_BITSET_BYTES);
	g->out = (LionContainer *) MemoryContextAlloc(st->buildcxt,
												  LION_CONTAINER_MAX_SIZE);
	g->pend = (LionContainer *)
		MemoryContextAlloc(st->buildcxt,
						   LION_CONTAINER_HDRSZ +
						   sizeof(uint16) * LO_GATHER_INLINE);
	return g;
}

/* Set the pending members of slot in a bitset image; how many were new. */
static uint32
lo_gather_set_bits(const LoGatherSlot *slot, uint64 *w)
{
	uint32		added = 0;
	uint32		i;

	for (i = 0; i < slot->n; i++)
	{
		uint32		lo = (uint32) slot->vals[i] & LION_LO_MASK;
		uint64		bit = UINT64CONST(1) << (lo & 63);

		if ((w[lo >> 6] & bit) == 0)
		{
			w[lo >> 6] |= bit;
			added++;
		}
	}
	return added;
}

/*
 * The slot's pending members, and the piece c when there is one, into the
 * container of ckey the slot holds (which it may not yet): a new one takes
 * the old one's place.
 */
static void
lo_gather_flush(LionOrderedState *st, LoGather *g, LoGatherSlot *slot,
				uint32 ckey, const LionContainer *c)
{
	LionContainer *old = slot->c;
	uint32		total = (old != NULL ? old->cardinality : 0) + slot->n +
		(c != NULL ? c->cardinality : 0);
	LionContainer *out = g->out;
	LionContainer *copy;
	bool		merged = false;
	Size		size;

	Assert(slot->n > 0 || c != NULL);

	if (old != NULL && old->type == LION_CT_BITSET)
	{
		/* a dense key, kept as a BITSET: its bits are set in place */
		uint32		added = lo_gather_set_bits(slot, LION_BITSET_DATA(old));

		old->cardinality = (uint16) Min((uint32) old->cardinality + added,
										(uint32) LION_CONTAINER_RANGE);
		if (c != NULL)
			lion_container_or_inplace(old, c);
		slot->n = 0;
		return;
	}

	if (total <= LO_MERGE_DIRECT_MAX)
	{
		/* a few members: merged member by member */
		const LionContainer *parts[3];
		uint32		np = 0;

		if (old != NULL)
			parts[np++] = old;
		if (slot->n > 0)
		{
			lion_container_init(g->pend, ckey);
			memcpy(LION_ARRAY_DATA(g->pend), slot->vals,
				   sizeof(uint16) * slot->n);
			g->pend->cardinality = slot->n;
			parts[np++] = g->pend;
		}
		if (c != NULL)
			parts[np++] = c;
		/* false only for a damaged header: then through the image */
		merged = lion_container_or_merge(parts, np, ckey, out);
	}
	if (!merged)
	{
		uint64	   *img = g->img;
		uint32		card;

		memset(img, 0, LION_BITSET_BYTES);
		if (old != NULL)
			lion_container_or_into_bitset(old, img);
		(void) lo_gather_set_bits(slot, img);
		if (c != NULL)
			lion_container_or_into_bitset(c, img);
		lion_bits_to_container(img, ckey, out);
		card = out->cardinality;
		if (card > LION_ARRAY_MAX_CARD && out->type != LION_CT_BITSET)
		{
			/*
			 * More members than an ARRAY holds: from here on the container
			 * is a BITSET, whose bits the next flushes set in place, and
			 * lo_gather_finish() optimizes it.
			 */
			lion_container_bitset_init(out, ckey);
			memcpy(LION_BITSET_DATA(out), img, LION_BITSET_BYTES);
			out->cardinality = (uint16) card;
		}
	}
	size = lion_container_size(out);
	slot->n = 0;
	if (old != NULL)
	{
		lo_set_free(st, old, lion_container_size(old));
		slot->c = NULL;
	}
	copy = (LionContainer *) lo_set_alloc(st, st->setcxt, size);
	memcpy(copy, out, size);
	slot->c = copy;
}

/* Take the members of piece c, a container of key c->ckey. */
static void
lo_gather_push(LionOrderedState *st, LoGather *g, const LionContainer *c)
{
	uint32		chunk = c->ckey / LO_GATHER_CHUNK_KEYS;
	uint32		i = c->ckey % LO_GATHER_CHUNK_KEYS;
	LoGatherSlot *slot;

	if (chunk >= g->nchunks)
	{
		uint32		n = g->nchunks;

		if (c->ckey > MaxBlockNumber / LION_BLOCKS_PER_CONTAINER)
			elog(ERROR, "lion: container key %u is beyond any heap block",
				 c->ckey);
		while (n <= chunk)
			n *= 2;
		g->chunks = (LoGatherSlot **)
			lo_set_grow(st, st->buildcxt, g->chunks,
						sizeof(LoGatherSlot *) * g->nchunks,
						sizeof(LoGatherSlot *) * n);
		memset(g->chunks + g->nchunks, 0,
			   sizeof(LoGatherSlot *) * (n - g->nchunks));
		g->touched = (uint64 *) lo_set_grow(st, st->buildcxt, g->touched,
											sizeof(uint64) * g->nchunks,
											sizeof(uint64) * n);
		memset(g->touched + g->nchunks, 0,
			   sizeof(uint64) * (n - g->nchunks));
		g->nchunks = n;
	}
	g->touched[chunk] |= UINT64CONST(1) << i;
	if (chunk >= g->maxchunk)
		g->maxchunk = chunk + 1;

	if (g->chunks[chunk] == NULL)
	{
		g->chunks[chunk] = (LoGatherSlot *)
			lo_set_alloc(st, st->buildcxt, LO_GATHER_CHUNK_BYTES);
		memset(g->chunks[chunk], 0, LO_GATHER_CHUNK_BYTES);
	}
	slot = &g->chunks[chunk][i];
	if (c->type == LION_CT_ARRAY && c->cardinality <= LO_GATHER_INLINE)
	{
		uint32		card = c->cardinality;

		if (slot->n + card > LO_GATHER_INLINE)
			lo_gather_flush(st, g, slot, c->ckey, NULL);
		memcpy(slot->vals + slot->n, LION_ARRAY_DATA(c),
			   sizeof(uint16) * card);
		slot->n += card;
	}
	else
		lo_gather_flush(st, g, slot, c->ckey, c);
}

/*
 * The gathered keys into the set, in ascending key, each key's members as
 * one container: the pending members flushed, a BITSET optimized.  The
 * chunks go as their keys are done.
 */
static void
lo_gather_finish(LionOrderedState *st, LoGather *g, LionTidSet *set)
{
	uint32		k;

	for (k = 0; k < g->maxchunk; k++)
	{
		uint64		bits = g->touched[k];

		while (bits != 0)
		{
			uint32		i = (uint32) pg_rightmost_one_pos64(bits);
			uint32		ckey = k * LO_GATHER_CHUNK_KEYS + i;
			LoGatherSlot *slot = &g->chunks[k][i];
			LionContainer *c;

			bits &= bits - 1;
			CHECK_FOR_INTERRUPTS();
			Assert(slot->n > 0 || slot->c != NULL);
			if (slot->n > 0)
				lo_gather_flush(st, g, slot, ckey, NULL);
			c = slot->c;
			slot->c = NULL;
			if (c->type == LION_CT_BITSET)
			{
				/* a dense key's working form: the smallest now */
				Size		size;
				LionContainer *copy;

				lion_bits_to_container(LION_BITSET_DATA(c), ckey, g->out);
				size = lion_container_size(g->out);
				copy = (LionContainer *) lo_set_alloc(st, st->setcxt, size);
				memcpy(copy, g->out, size);
				lo_set_free(st, c, lion_container_size(c));
				c = copy;
			}
			lo_set_append(st, set, ckey, c);
		}
		if (g->chunks[k] != NULL)
		{
			lo_set_free(st, g->chunks[k], LO_GATHER_CHUNK_BYTES);
			g->chunks[k] = NULL;
		}
	}
	lo_set_free(st, g->chunks, sizeof(LoGatherSlot *) * g->nchunks);
	lo_set_free(st, g->touched, sizeof(uint64) * g->nchunks);
	pfree(g->img);
	pfree(g->out);
	pfree(g->pend);
	pfree(g);
}

/* a AND b, or a OR b, of two sorted sets; both are consumed. */
static LionTidSet *
lo_set_combine(LionOrderedState *st, LionTidSet *a, LionTidSet *b, bool isand)
{
	LionTidSet *r = lo_set_new(st);
	LionContainer *buf = (LionContainer *)
		MemoryContextAlloc(st->buildcxt, LION_CONTAINER_MAX_SIZE);
	int			i = 0;
	int			j = 0;

	while (i < a->n || j < b->n)
	{
		CHECK_FOR_INTERRUPTS();

		if (j >= b->n || (i < a->n && a->keys[i] < b->keys[j]))
		{
			if (!isand)
			{
				lo_set_push(st, r, a->keys[i], a->conts[i]);
			}
			i++;
			continue;
		}
		if (i >= a->n || b->keys[j] < a->keys[i])
		{
			if (!isand)
				lo_set_push(st, r, b->keys[j], b->conts[j]);
			j++;
			continue;
		}

		/* the same key in both */
		if ((isand ? lion_container_and(a->conts[i], b->conts[j], buf) :
			 lion_container_or(a->conts[i], b->conts[j], buf)) > 0)
			lo_set_push(st, r, a->keys[i], buf);
		i++;
		j++;
	}
	pfree(buf);
	lo_set_drop(st, a);
	lo_set_drop(st, b);
	return r;
}

/* The index of tid's container key if tid is a member, else -1. */
static int
lo_set_find(const LionTidSet *set, ItemPointer tid)
{
	uint64		code = lion_tid_to_code(tid);
	uint32		ckey = lion_code_ckey(code);
	int			lo = 0;
	int			hi = set->n - 1;

	while (lo <= hi)
	{
		int			mid = lo + (hi - lo) / 2;

		if (set->keys[mid] == ckey)
			return lion_container_contains(set->conts[mid],
										   lion_code_lo(code)) ? mid : -1;
		if (set->keys[mid] < ckey)
			lo = mid + 1;
		else
			hi = mid - 1;
	}
	return -1;
}

/* ---------------------------------------------------------------------
 * Building the set (DESIGN.md §30.4)
 * --------------------------------------------------------------------- */

/*
 * A leaf's set from its source: a sorted source's containers straight into
 * the set, one a key in ascending key (§29.3); a WALK's or a LIST's, which
 * come entry by entry or batch by batch, gathered by key first (LoGather).
 */
static LionTidSet *
lo_build_leaf(LionOrderedState *st, LoLeaf *leaf)
{
	LionTidSet *set = lo_set_new(st);
	LionSource *src;
	const LionContainer *c;
	int64		ntids = 0;

	src = lion_source_open(leaf->index, leaf->keys, leaf->nkeys, false,
						   st->buildcxt);
	pgstat_count_index_scan(leaf->index);
	if (!lion_source_exact(src))
		st->exact = false;
	if (lion_source_sorted(src))
	{
		while ((c = lion_source_next(src)) != NULL)
		{
			CHECK_FOR_INTERRUPTS();
			if (c->cardinality == 0)
				continue;
			ntids += c->cardinality;
			if (set->n > 0 && c->ckey == set->keys[set->n - 1])
				lo_set_or_last(st, set, c);
			else if (set->n > 0 && c->ckey < set->keys[set->n - 1])
				elog(ERROR, "lion: a sorted source handed out container key %u after %u",
					 c->ckey, set->keys[set->n - 1]);
			else
				lo_set_push(st, set, c->ckey, c);
		}
	}
	else
	{
		LoGather   *g = lo_gather_new(st);

		while ((c = lion_source_next(src)) != NULL)
		{
			CHECK_FOR_INTERRUPTS();
			ntids += c->cardinality;
			if (c->cardinality > 0)
				lo_gather_push(st, g, c);
		}
		lo_gather_finish(st, g, set);
	}
	lion_source_close(src);
	/* the TIDs the index gave, as a bitmap index scan counts them */
	pgstat_count_index_tuples(leaf->index, ntids);
	return set;
}

static LionTidSet *
lo_build_node(LionOrderedState *st, LoNode *node)
{
	LionTidSet *acc;
	int			i;

	if (node->kind == LO_NODE_LEAF)
		return lo_build_leaf(st, node->leaf);

	acc = lo_build_node(st, node->child[0]);
	for (i = 1; i < node->nchild; i++)
	{
		/* an empty AND stays empty: the other children need not be read */
		if (node->kind == LO_NODE_AND && acc->n == 0)
			break;
		acc = lo_set_combine(st, acc, lo_build_node(st, node->child[i]),
							 node->kind == LO_NODE_AND);
	}
	return acc;
}

/* The leaves' run-time keys, for this scan's Param values. */
static void
lo_leaf_keys(LionOrderedState *st)
{
	int			i;

	ResetExprContext(st->lrtcxt);
	for (i = 0; i < st->nleaves; i++)
	{
		if (st->leaves[i].nrtkeys > 0)
			ExecIndexEvalRuntimeKeys(st->lrtcxt, st->leaves[i].rtkeys,
									 st->leaves[i].nrtkeys);
	}
}

/* Let go of the set's memory, before a build or a view of a shared copy. */
static void
lo_set_reset(LionOrderedState *st)
{
	/* the walk's visited bits are given back before (lo_scan_reset()) */
	Assert(st->visited == NULL);
	MemoryContextReset(st->setcxt);
	if (st->arena != NULL)
		lion_arena_reset(st->arena);
	st->set = NULL;
	st->setview = false;
	st->live = NIL;
	st->bytes = 0;
	st->spilled = false;
	st->limit = get_hash_memory_limit();
}

/*
 * The members of the set, and the places of the walk's visited bits before
 * each key (§30.4, "Stopping early"): at most LION_CONTAINER_RANGE a key.
 */
static void
lo_set_places(LionOrderedState *st)
{
	int			i;

	st->ncont = st->set->n;
	st->members = 0;
	st->places = (uint64 *) lo_set_alloc(st, st->setcxt,
										 sizeof(uint64) * ((Size) st->set->n + 1));
	st->places[0] = 0;
	for (i = 0; i < st->set->n; i++)
	{
		st->members += st->set->conts[i]->cardinality;
		st->places[i + 1] = st->places[i] +
			lion_container_places(st->set->conts[i]);
	}
}

static void
lo_build_set(LionOrderedState *st)
{
	lo_set_reset(st);
	st->exact = !st->lossyqual;

	lo_leaf_keys(st);
	st->set = lo_build_node(st, st->tree);
	MemoryContextReset(st->buildcxt);

	lo_set_places(st);
	st->spillbytes = st->spilled ? lion_arena_used(st->arena) : 0;
	st->builds++;
}

/* ---------------------------------------------------------------------
 * The set shared by a parallel walk's participants (DESIGN.md §40.9)
 * --------------------------------------------------------------------- */

/* The wait for the set's builder, as pg_stat_activity names it. */
static uint32
lo_set_wait_event(void)
{
#if PG_VERSION_NUM >= 170000
	static uint32 event = 0;

	if (event == 0)
		event = WaitEventExtensionNew("LionOrderedSetBuild");
	return event;
#else
	return PG_WAIT_EXTENSION;
#endif
}

/*
 * The set just built, copied into the DSA as one LoSetImage, or
 * InvalidDsaPointer when it would take more than `room` - or more than the
 * DSA will give.
 */
static dsa_pointer
lo_set_publish(LionOrderedState *st, dsa_area *dsa, Size room)
{
	LionTidSet *set = st->set;
	Size		total = LO_IMAGE_HEAD(set->n);
	dsa_pointer p;
	LoSetImage *img;
	Size	   *offs;
	Size		off;
	int			i;

	for (i = 0; i < set->n; i++)
		total += MAXALIGN(lion_container_size(set->conts[i]));
	if (total > room)
		return InvalidDsaPointer;
	p = dsa_allocate_extended(dsa, total, DSA_ALLOC_HUGE | DSA_ALLOC_NO_OOM);
	if (!DsaPointerIsValid(p))
		return InvalidDsaPointer;

	img = (LoSetImage *) dsa_get_address(dsa, p);
	img->n = set->n;
	img->exact = st->exact;
	if (set->n > 0)
		memcpy(LO_IMAGE_KEYS(img), set->keys, sizeof(uint32) * (Size) set->n);
	offs = LO_IMAGE_OFFS(img);
	off = LO_IMAGE_HEAD(set->n);
	for (i = 0; i < set->n; i++)
	{
		Size		size = lion_container_size(set->conts[i]);

		if ((i & 1023) == 0)
			CHECK_FOR_INTERRUPTS();
		memcpy((char *) img + off, set->conts[i], size);
		offs[i] = off;
		off += MAXALIGN(size);
	}
	Assert(off == total);
	return p;
}

/*
 * The set as a view of its shared copy: the keys and the containers read
 * where they are in the DSA, the directory of where each is and the walk's
 * places this participant's own.
 */
static void
lo_set_attach(LionOrderedState *st, LoSetImage *img)
{
	LionTidSet *set;
	Size	   *offs = LO_IMAGE_OFFS(img);
	int			i;

	lo_set_reset(st);
	set = (LionTidSet *) MemoryContextAllocZero(st->setcxt, sizeof(LionTidSet));
	set->n = img->n;
	set->cap = img->n;
	set->keys = LO_IMAGE_KEYS(img);
	set->conts = (LionContainer **)
		lo_set_alloc(st, st->setcxt,
					 sizeof(LionContainer *) * (Size) Max(img->n, 1));
	for (i = 0; i < img->n; i++)
		set->conts[i] = (LionContainer *) ((char *) img + offs[i]);
	st->set = set;
	st->setview = true;
	st->exact = img->exact;
	lo_set_places(st);
	st->spillbytes = 0;
	st->attaches++;
}

/*
 * The set of a parallel walk (DESIGN.md §40.9): the first participant to need
 * it builds it, as the serial node does, and puts it into the DSA when it
 * fits the room; the others wait for it there and read it, or build their
 * own when it did not fit.  A builder's error ends the query, the waiters'
 * sleep with it: the leader rethrows a worker's error at its next interrupt
 * check, which the wait makes, and its abort ends every worker.
 */
static void
lo_shared_set(LionOrderedState *st)
{
	LoShared   *sh = st->shared;
	dsa_area   *dsa = st->css.ss.ps.state->es_query_dsa;
	dsa_pointer p;
	int			state;

	SpinLockAcquire(&sh->mutex);
	state = sh->setstate;
	if (state == LO_SHSET_NONE)
		sh->setstate = LO_SHSET_BUILDING;
	p = sh->setimage;
	SpinLockRelease(&sh->mutex);

	if (state == LO_SHSET_NONE)
	{
		lo_build_set(st);
		p = (dsa != NULL) ? lo_set_publish(st, dsa, sh->setroom) :
			InvalidDsaPointer;
		SpinLockAcquire(&sh->mutex);
		sh->setimage = p;
		sh->setstate = DsaPointerIsValid(p) ? LO_SHSET_SHARED : LO_SHSET_OWN;
		sh->ncont = st->ncont;
		sh->exact = st->exact;
		sh->spillbytes = Max(sh->spillbytes, st->spillbytes);
		SpinLockRelease(&sh->mutex);
		ConditionVariableBroadcast(&sh->setcv);

		/* its own copy goes: it reads the shared one, as the others do */
		if (DsaPointerIsValid(p))
		{
			Size		spill = st->spillbytes;

			lo_set_attach(st, (LoSetImage *) dsa_get_address(dsa, p));
			st->spillbytes = spill;
			st->published++;
		}
		return;
	}

	if (state == LO_SHSET_BUILDING)
	{
		ConditionVariablePrepareToSleep(&sh->setcv);
		for (;;)
		{
			SpinLockAcquire(&sh->mutex);
			state = sh->setstate;
			p = sh->setimage;
			SpinLockRelease(&sh->mutex);
			if (state != LO_SHSET_BUILDING)
				break;
			ConditionVariableSleep(&sh->setcv, lo_set_wait_event());
		}
		ConditionVariableCancelSleep();
	}
	if (state == LO_SHSET_SHARED && dsa != NULL)
		lo_set_attach(st, (LoSetImage *) dsa_get_address(dsa, p));
	else
		lo_build_set(st);
}

/* ---------------------------------------------------------------------
 * The set, lazily (DESIGN.md §30.4, "The set, lazily")
 * --------------------------------------------------------------------- */

/*
 * Can every key of every leaf be sought in: a set tree - an equality or a
 * list - on a scalar column, or `IS NOT NULL` beside one, its column's NULL
 * entry taken away from it?  A range is a walk of entries, and a multi-key
 * column's query may need every row; those leaves are only ever built.
 */
static bool
lo_lazy_ok(LionOrderedState *st)
{
	int			i;
	int			k;

	if (st->noset)
		return false;
	for (i = 0; i < st->nleaves; i++)
	{
		LoLeaf	   *leaf = &st->leaves[i];
		bool		positive = false;

		if (leaf->nkeys == 0)
			return false;
		for (k = 0; k < leaf->nkeys; k++)
		{
			ScanKey		sk = &leaf->keys[k];
			LionState  *col = lion_index_column_state(leaf->index,
													  sk->sk_attno);

			if ((sk->sk_flags & (SK_SEARCHNOTNULL | SK_ROW_HEADER)) ==
				SK_SEARCHNOTNULL)
				continue;		/* any column's: its NULL entry, subtracted */
			if (col->multikey || sk->sk_strategy != LION_STRAT_EQUAL ||
				(sk->sk_flags & (SK_SEARCHNULL | SK_SEARCHNOTNULL |
								 SK_ROW_HEADER)) != 0)
				return false;
			positive = true;
		}
		if (!positive)
			return false;		/* nothing to take a NULL entry away from */
	}
	return true;
}

/*
 * Let go of the lazy set: every stream, and every set - their pins before
 * the memory they live in (lion_posting_set_release()) - and the memo.
 */
static void
lo_lazy_end(LionOrderedState *st)
{
	int			i;
	int			k;
	int			s;

	for (i = 0; i < st->nleaves; i++)
	{
		LoLeaf	   *leaf = &st->leaves[i];

		if (leaf->pkeys == NULL)
			continue;
		for (k = 0; k < leaf->nkeys; k++)
		{
			LoProbeKey *pk = &leaf->pkeys[k];

			if (pk->stream != NULL)
				lion_stream_end(pk->stream);
			pk->stream = NULL;
			for (s = 0; s < pk->nsets; s++)
				lion_posting_set_release(&pk->sets[s]);
			pk->nsets = 0;
		}
		leaf->pkeys = NULL;
	}
	st->memo = NULL;
	st->memobytes = 0;
	st->lazy = false;
	if (st->lazycxt != NULL)
		MemoryContextReset(st->lazycxt);
}

/* Does node select nothing, whatever the key: a key of no entry at all? */
static bool
lo_lazy_empty(LoNode *node)
{
	int			i;

	if (node->kind == LO_NODE_LEAF)
	{
		for (i = 0; i < node->leaf->nkeys; i++)
		{
			if (node->leaf->pkeys[i].tree == NULL &&
				!node->leaf->pkeys[i].negated)
				return true;
		}
		return false;
	}
	for (i = 0; i < node->nchild; i++)
	{
		bool		empty = lo_lazy_empty(node->child[i]);

		if (node->kind == LO_NODE_AND && empty)
			return true;
		if (node->kind == LO_NODE_OR && !empty)
			return false;
	}
	return node->kind == LO_NODE_OR;
}

/*
 * How many members node has at most, by the located entries' recorded member
 * counts: an AND no more than its smallest child, an OR no more than its
 * children together.  The counts are hints; this decides only when the set
 * is built, never what it holds.
 */
static double
lo_lazy_members(LoNode *node)
{
	double		n = 0.0;
	int			i;
	int			s;

	if (node->kind == LO_NODE_LEAF)
	{
		bool		first = true;

		for (i = 0; i < node->leaf->nkeys; i++)
		{
			LoProbeKey *pk = &node->leaf->pkeys[i];
			double		m = 0.0;

			if (pk->negated)
				continue;		/* it only takes members away */
			if (pk->tree != NULL)
				for (s = 0; s < pk->nsets; s++)
					m += (double) pk->sets[s].ntids;
			n = first ? m : Min(n, m);
			first = false;
		}
		return n;
	}
	for (i = 0; i < node->nchild; i++)
	{
		double		m = lo_lazy_members(node->child[i]);

		if (node->kind == LO_NODE_AND)
			n = (i == 0) ? m : Min(n, m);
		else
			n += m;
	}
	return n;
}

/*
 * Start the set lazily: locate every leaf key's posting sets, as a bitmap
 * scan locates them, let go of their pins, and make the memo.  False when a
 * key is no set tree after all, or a list longer than a plain scan opens at
 * once (lion_scan_list_batch(), §29.4; a stream holds a cursor for each of
 * its sets): the set is then built, a batch at a time.
 */
static bool
lo_lazy_begin(LionOrderedState *st)
{
	MemoryContext oldcxt;
	double		containers = 0.0;
	int			i;
	int			k;
	int			s;

	lo_leaf_keys(st);
	lo_lazy_end(st);
	oldcxt = MemoryContextSwitchTo(st->lazycxt);

	for (i = 0; i < st->nleaves; i++)
	{
		LoLeaf	   *leaf = &st->leaves[i];

		leaf->pkeys = (LoProbeKey *) palloc0(sizeof(LoProbeKey) * leaf->nkeys);
		for (k = 0; k < leaf->nkeys; k++)
		{
			LoProbeKey *pk = &leaf->pkeys[k];
			ScanKey		sk = &leaf->keys[k];
			bool		nomatch = false;
			bool		ok;

			if ((sk->sk_flags & (SK_SEARCHARRAY | SK_ISNULL)) == SK_SEARCHARRAY)
			{
				ArrayType  *arr = DatumGetArrayTypeP(sk->sk_argument);

				if (ArrayGetNItems(ARR_NDIM(arr), ARR_DIMS(arr)) >
					lion_scan_list_batch())
				{
					MemoryContextSwitchTo(oldcxt);
					lo_lazy_end(st);
					return false;
				}
			}
			if ((sk->sk_flags & SK_SEARCHNOTNULL) != 0)
			{
				bool		implied = false;
				int			o;

				/*
				 * Its column's NULL entry, to take away (lo_probe_node()) -
				 * unless an equality of its column is beside it, which never
				 * selects a NULL (lo_lazy_ok(): every other key of the leaf is
				 * one), and there is nothing to take away.
				 */
				for (o = 0; o < leaf->nkeys && !implied; o++)
					implied = (o != k &&
							   leaf->keys[o].sk_attno == sk->sk_attno &&
							   (leaf->keys[o].sk_flags & SK_SEARCHNOTNULL) == 0);
				pk->negated = true;
				pk->nsets = 1;
				pk->sets = (LionPostingSet *) palloc0(sizeof(LionPostingSet));
				if (!implied &&
					lion_posting_set_lookup_null_col(leaf->index,
													 sk->sk_attno,
													 &pk->sets[0]))
					pk->tree = lion_keynode_leaf(0);
				else
					pk->tree = NULL;
				ok = true;
			}
			else
				ok = lion_scankey_sets(leaf->index, sk, &pk->nsets, &pk->sets,
									   &pk->tree, &nomatch);
			for (s = 0; s < pk->nsets; s++)
			{
				lion_posting_set_unpin(&pk->sets[s]);
				containers += (double) pk->sets[s].ncontainers;
			}
			if (!ok)
			{
				MemoryContextSwitchTo(oldcxt);
				lo_lazy_end(st);
				return false;
			}
			if (nomatch)
				pk->tree = NULL;
			pk->cxt = AllocSetContextCreate(st->lazycxt, "LionOrdered stream",
											ALLOCSET_SMALL_SIZES);
			CHECK_FOR_INTERRUPTS();
		}
		pgstat_count_index_scan(leaf->index);
	}

	st->memo = lo_memo_create(st->lazycxt, 256, NULL);
	MemoryContextSwitchTo(oldcxt);

	st->memobytes = 0;
	st->lazywork = 0.0;
	st->lazyevals = 0;
	st->lazyback = 0;
	st->lazylast = 0;
	st->lazybudget = Max((double) LO_LAZY_MIN_WORK, containers);
	st->limit = get_hash_memory_limit();
	st->exact = !st->lossyqual;
	st->lazyempty = lo_lazy_empty(st->tree);
	st->lazymembers = lo_lazy_members(st->tree);
	st->lazy = true;
	return true;
}

/*
 * The container of pk's tree at ckey, or NULL: valid until pk is sought
 * again.  The stream only goes forward, so a probe behind where it stands
 * starts it again, over the same located sets.
 */
static const LionContainer *
lo_probe_at(LionOrderedState *st, LoProbeKey *pk, uint32 ckey)
{
	const LionContainer *c;

	if (pk->tree == NULL)
		return NULL;
	if (pk->stream != NULL && ckey < pk->last)
	{
		lion_stream_end(pk->stream);
		pk->stream = NULL;
		MemoryContextReset(pk->cxt);
		st->lazywork += (double) (LO_LAZY_RESTART_WORK * pk->nsets);
	}
	if (pk->stream == NULL)
	{
		MemoryContext oldcxt = MemoryContextSwitchTo(pk->cxt);

		pk->stream = lion_stream_begin(pk->nsets, pk->sets, pk->tree, false);
		MemoryContextSwitchTo(oldcxt);
	}
	pk->last = ckey;
	st->lazywork += (double) Max(pk->nsets, 1);
	c = lion_stream_at(pk->stream, ckey);
	return (c != NULL && c->ckey == ckey && c->cardinality > 0) ? c : NULL;
}

/* A copy of c in probecxt, with room for what an AND or an OR makes. */
static LionContainer *
lo_probe_copy(LionOrderedState *st, const LionContainer *c)
{
	LionContainer *r = (LionContainer *)
		MemoryContextAlloc(st->probecxt, LION_CONTAINER_MAX_SIZE);

	memcpy(r, c, lion_container_size(c));
	return r;
}

/* What node selects at ckey, in probecxt, or NULL for nothing. */
static LionContainer *
lo_probe_node(LionOrderedState *st, LoNode *node, uint32 ckey)
{
	LionContainer *acc = NULL;
	int			i;

	if (node->kind == LO_NODE_LEAF)
	{
		LoLeaf	   *leaf = node->leaf;

		/* the keys that select, ANDed ... */
		for (i = 0; i < leaf->nkeys; i++)
		{
			const LionContainer *c;
			LionContainer *r;

			if (leaf->pkeys[i].negated)
				continue;
			c = lo_probe_at(st, &leaf->pkeys[i], ckey);
			if (c == NULL)
				return NULL;
			pgstat_count_index_tuples(leaf->index, c->cardinality);
			if (acc == NULL)
			{
				acc = lo_probe_copy(st, c);
				continue;
			}
			r = (LionContainer *) MemoryContextAlloc(st->probecxt,
													 LION_CONTAINER_MAX_SIZE);
			if (lion_container_and(acc, c, r) == 0)
				return NULL;
			acc = r;
		}
		Assert(acc != NULL);	/* lo_lazy_ok(): a leaf has one */

		/* ... less the NULL entries of its `IS NOT NULL` keys */
		for (i = 0; i < leaf->nkeys; i++)
		{
			const LionContainer *c;
			LionContainer *r;

			if (!leaf->pkeys[i].negated)
				continue;
			c = lo_probe_at(st, &leaf->pkeys[i], ckey);
			if (c == NULL)
				continue;		/* nothing to take away at this key */
			r = (LionContainer *) MemoryContextAlloc(st->probecxt,
													 LION_CONTAINER_MAX_SIZE);
			if (lion_container_andnot(acc, c, r) == 0)
				return NULL;
			acc = r;
		}
		return acc;
	}

	for (i = 0; i < node->nchild; i++)
	{
		LionContainer *c = lo_probe_node(st, node->child[i], ckey);
		LionContainer *r;

		if (c == NULL)
		{
			/* an empty AND stays empty: the other children need not be read */
			if (node->kind == LO_NODE_AND)
				return NULL;
			continue;
		}
		if (acc == NULL)
		{
			acc = c;
			continue;
		}
		r = (LionContainer *) MemoryContextAlloc(st->probecxt,
												 LION_CONTAINER_MAX_SIZE);
		if (node->kind == LO_NODE_AND)
		{
			if (lion_container_and(acc, c, r) == 0)
				return NULL;
		}
		else
			(void) lion_container_or(acc, c, r);
		acc = r;
	}
	return acc;
}

/*
 * The memo's entry for container key ckey: a key met for the first time is
 * evaluated and remembered, members or not.  Its container is NULL where the
 * set has no member at the key.
 */
static LoMemoEnt *
lo_lazy_entry(LionOrderedState *st, uint32 ckey)
{
	LoMemoEnt  *e = lo_memo_lookup(st->memo, ckey);

	if (e == NULL)
	{
		LionContainer *c;
		bool		found;

		/* how far the walk goes in heap order (LO_LAZY_ORDER_KEYS) */
		if (st->lazyevals > 0 && ckey < st->lazylast)
			st->lazyback++;
		st->lazyevals++;
		st->lazylast = ckey;

		MemoryContextReset(st->probecxt);
		c = lo_probe_node(st, st->tree, ckey);
		e = lo_memo_insert(st->memo, ckey, &found);
		e->c = NULL;
		e->visited = NULL;
		if (c != NULL && c->cardinality > 0)
		{
			Size		size = lion_container_size(c);

			e->c = (LionContainer *) MemoryContextAlloc(st->lazycxt, size);
			memcpy(e->c, c, size);
			st->memobytes += size;
		}
		st->memobytes += sizeof(LoMemoEnt);
		st->lazykeys++;
		CHECK_FOR_INTERRUPTS();
	}
	return e;
}

/* The memo's entry for tid's container key if tid is a member, else NULL. */
static LoMemoEnt *
lo_lazy_find(LionOrderedState *st, ItemPointer tid)
{
	uint64		code = lion_tid_to_code(tid);
	LoMemoEnt  *e = lo_lazy_entry(st, lion_code_ckey(code));

	if (e->c == NULL || !lion_container_contains(e->c, lion_code_lo(code)))
		return NULL;
	return e;
}

/* lo_mark() for the lazy set: the memo keeps what the walk met. */
static bool
lo_lazy_mark(LionOrderedState *st, LoMemoEnt *e, ItemPointer tid)
{
	uint16		lo = lion_code_lo(lion_tid_to_code(tid));
	uint64		bit = UINT64CONST(1) << (lo & 63);

	if (e->visited == NULL)
	{
		e->visited = (uint64 *) MemoryContextAllocZero(st->scancxt,
													   LION_BITSET_BYTES);
		st->memobytes += LION_BITSET_BYTES;
	}
	if (e->visited[lo >> 6] & bit)
		return false;
	e->visited[lo >> 6] |= bit;
	return true;
}

/* The index of container key ckey in set, or -1. */
static int
lo_set_key_index(const LionTidSet *set, uint32 ckey)
{
	int			lo = 0;
	int			hi = set->n - 1;

	while (lo <= hi)
	{
		int			mid = lo + (hi - lo) / 2;

		if (set->keys[mid] == ckey)
			return mid;
		if (set->keys[mid] < ckey)
			lo = mid + 1;
		else
			hi = mid - 1;
	}
	return -1;
}

/*
 * The probes have cost what the build would, or the walk has gone far: build
 * the set, and hand it what the walk met so far, so that the early stop and
 * the switch count those members as met.  The members met are counted again
 * against the built set, not carried over: the probes answered from the index
 * as it was, and a VACUUM in between may have taken out of it the dead TIDs
 * the walk met and marked, which the built set no longer has.  Carried over,
 * those would let the count of members met reach the set's members while
 * live members were still ahead, and the early stop would end the scan before
 * them.  A key the built set has no member at any more lost only such TIDs,
 * which no snapshot sees and the switch does not fetch.
 *
 * The lazy set goes first: the build evaluates the leaves' run-time keys
 * again, and the values the lazy sets were located from go with them.  What
 * the walk met lives in scancxt, and is let go as it is handed over.
 */
static void
lo_lazy_convert(LionOrderedState *st)
{
	lo_memo_iterator it;
	LoMemoEnt  *e;
	uint32	   *mkeys;
	uint64	  **mvisited;
	int			nmet = 0;
	uint16	   *los = NULL;
	uint32	   *places = NULL;
	int			i;

	mkeys = (uint32 *) MemoryContextAlloc(st->scancxt,
										  sizeof(uint32) * Max(st->memo->members, 1));
	mvisited = (uint64 **) MemoryContextAlloc(st->scancxt,
											  sizeof(uint64 *) * Max(st->memo->members, 1));
	LION_SH_START_ITERATE(lo_memo, st->memo, &it);
	while ((e = lo_memo_iterate(st->memo, &it)) != NULL)
	{
		if (e->visited == NULL)
			continue;
		mkeys[nmet] = e->ckey;
		mvisited[nmet++] = e->visited;
	}
	lo_lazy_end(st);

	lo_build_set(st);
	st->distinct = 0;
	if (nmet > 0 && st->set->n > 0)
	{
		los = (uint16 *) palloc(sizeof(uint16) * LION_CONTAINER_RANGE);
		places = (uint32 *) palloc(sizeof(uint32) * LION_CONTAINER_RANGE);
	}
	for (i = 0; i < nmet; i++)
	{
		int			idx = (los != NULL) ?
			lo_set_key_index(st->set, mkeys[i]) : -1;

		/* the members met at this key that the built set still has */
		if (idx >= 0)
		{
			uint32		n = lion_container_to_places(st->set->conts[idx],
													 los, places);
			uint32		j;

			for (j = 0; j < n; j++)
			{
				if ((mvisited[i][los[j] >> 6] &
					 (UINT64CONST(1) << (los[j] & 63))) != 0 &&
					lo_mark_place(st, idx, places[j]))
					st->distinct++;
			}
		}
		pfree(mvisited[i]);
	}
	if (los != NULL)
	{
		pfree(los);
		pfree(places);
	}
	pfree(mkeys);
	pfree(mvisited);
}

/* ---------------------------------------------------------------------
 * Executor
 * --------------------------------------------------------------------- */

static Node *
lo_create_state(CustomScan *cscan)
{
	LionOrderedState *st = (LionOrderedState *)
		newNode(sizeof(LionOrderedState), T_CustomScanState);
	bool		indexonly;

	st->css.methods = &lo_exec_methods;

	/*
	 * The scan slot's type is fixed per plan, here, before ExecInitCustomScan
	 * initialises the projection and the quals on it: heap tuples, fetched
	 * through the table AM into a buffer slot; or, in index-only mode, the
	 * index tuple's values in a virtual slot of the relation's shape
	 * (DESIGN.md §40.4).  lo_begin() checks the rest of the shape.
	 */
	indexonly = (lo_priv_flags(cscan->custom_private) & LO_FLAG_INDEXONLY) != 0;
	st->css.slotOps = indexonly ? &TTSOpsVirtual : &TTSOpsBufferHeapTuple;
	return (Node *) st;
}

/*
 * An index qual as ExecIndexBuildScanKeys() wants it: the key operand
 * replaced by an INDEX_VAR Var for index column col (0-based), as
 * fix_indexqual_references() does for an Index Scan.
 */
static Expr *
lo_fix_qual(Expr *clause, int col)
{
	Node	   *key;
	Var		   *var;

	clause = copyObject(clause);
	if (IsA(clause, OpExpr))
		key = linitial(((OpExpr *) clause)->args);
	else if (IsA(clause, ScalarArrayOpExpr))
		key = linitial(((ScalarArrayOpExpr *) clause)->args);
	else if (IsA(clause, NullTest))
		key = (Node *) ((NullTest *) clause)->arg;
	else
	{
		elog(ERROR, "LionOrdered: unsupported index qual type %d",
			 (int) nodeTag(clause));
		return NULL;			/* keep compiler quiet */
	}

	var = makeVar(INDEX_VAR, (AttrNumber) (col + 1), exprType(key),
				  exprTypmod(key), exprCollation(key), 0);
	if (IsA(clause, OpExpr))
		linitial(((OpExpr *) clause)->args) = var;
	else if (IsA(clause, ScalarArrayOpExpr))
		linitial(((ScalarArrayOpExpr *) clause)->args) = var;
	else
		((NullTest *) clause)->arg = (Expr *) var;
	return clause;
}

/* The lion tree out of custom_private, and every leaf's quals. */
static LoNode *
lo_decode_tree(LionOrderedState *st, List *tree, ListCell **pos,
			   List *leafoids, List *lionquals, int *leafno, int *qualno)
{
	List	   *item = (List *) lfirst(*pos);
	LoNode	   *node = (LoNode *) palloc0(sizeof(LoNode));
	int			i;

	*pos = lnext(tree, *pos);
	node->kind = linitial_int(item);
	node->nchild = lsecond_int(item);

	if (node->kind == LO_NODE_LEAF)
	{
		LoLeaf	   *leaf = &st->leaves[*leafno];

		leaf->indexoid = list_nth_oid(leafoids, *leafno);
		leaf->quals = NIL;
		for (i = 0; i < node->nchild; i++)
		{
			Expr	   *q = (Expr *) list_nth(lionquals, *qualno);

			leaf->quals = lappend(leaf->quals,
								  lo_fix_qual(q, list_nth_int(item, 2 + i)));
			(*qualno)++;
		}
		node->nchild = 0;
		node->leaf = leaf;
		(*leafno)++;
		return node;
	}

	node->child = (LoNode **) palloc(sizeof(LoNode *) * node->nchild);
	for (i = 0; i < node->nchild; i++)
		node->child[i] = lo_decode_tree(st, tree, pos, leafoids, lionquals,
										leafno, qualno);
	return node;
}

static void
lo_begin(CustomScanState *node, EState *estate, int eflags)
{
	LionOrderedState *st = (LionOrderedState *) node;
	CustomScan *cscan = (CustomScan *) node->ss.ps.plan;
	LoPriv		priv;
	List	   *tree;
	List	   *leafoids;
	List	   *lionquals;
	List	   *ordquals;
	List	   *orderbys;
	List	   *fixed = NIL;
	ListCell   *pos;
	int			leafno = 0;
	int			qualno = 0;
	int			i;

	lo_priv_decode(cscan->custom_private, &priv);
	if (list_length(cscan->custom_exprs) != LO_EXPR_NLISTS)
		elog(ERROR, "LionOrdered: unrecognized custom_exprs shape (%d lists)",
			 list_length(cscan->custom_exprs));

	st->ordoid = priv.ordoid;
	st->dir = (ScanDirection) priv.dir;
	st->lossyqual = (priv.flags & LO_FLAG_LOSSY) != 0;
	st->lionwalk = (priv.flags & LO_FLAG_LIONWALK) != 0;
	st->indexonly = (priv.flags & LO_FLAG_INDEXONLY) != 0;
	st->lionheaprecheck = (priv.flags & LO_FLAG_HEAPRECHECK) != 0;
	st->knn = (priv.flags & LO_FLAG_KNN) != 0;
	st->knnlimit = priv.limit;
	st->probe = (priv.flags & LO_FLAG_PROBE) != 0;
	st->side = (priv.flags & LO_FLAG_SIDE) != 0;
	st->keyed = (priv.flags & LO_FLAG_KEYED) != 0;
	st->vmbuffer = InvalidBuffer;
	st->walknulls = priv.nulls;
	st->walkattno = priv.walkattno;
	tree = priv.tree;
	leafoids = priv.leaves;
	lionquals = (List *) list_nth(cscan->custom_exprs, LO_EXPR_LIONQUALS);
	ordquals = (List *) list_nth(cscan->custom_exprs, LO_EXPR_ORDQUALS);
	if (list_length(priv.ordcols) != list_length(ordquals))
		elog(ERROR, "LionOrdered: malformed ordered index quals");
	orderbys = (List *) list_nth(cscan->custom_exprs, LO_EXPR_ORDERBYS);
	if (st->knn ?
		(list_length(orderbys) != 1 || st->lionwalk || st->indexonly ||
		 !ScanDirectionIsForward(st->dir) || tree == NIL) :
		orderbys != NIL)
		elog(ERROR, "LionOrdered: malformed order by distance");

	/*
	 * A parallel walk (DESIGN.md §40.9) is a btree's, over a set built at
	 * once, and has no sort keys: it never switches.
	 */
	st->parallel = cscan->scan.plan.parallel_aware;
	if (st->parallel &&
		(st->lionwalk || st->knn || tree == NIL ||
		 (priv.flags & LO_FLAG_BUILD) == 0 || priv.nsort > 0))
		elog(ERROR, "LionOrdered: malformed parallel walk");

	/*
	 * A nested loop's probe (DESIGN.md §40.10) is an unordered btree walk
	 * over a set, never parallel-aware: it has no sort keys, and it never
	 * switches.
	 */
	if (st->probe &&
		(st->parallel || st->lionwalk || st->knn ||
		 (tree == NIL && !st->side) || priv.nsort > 0))
		elog(ERROR, "LionOrdered: malformed nested loop probe");

	/*
	 * A side of a LionOrderedJoin (DESIGN.md §47) is a serial btree's walk;
	 * its dimension a probe in heap mode, keyed by the join.
	 */
	if ((st->side &&
		 (st->parallel || st->lionwalk || st->knn)) ||
		(st->keyed &&
		 (!st->side || !st->probe || st->indexonly ||
		  list_length(priv.ordcols) != 1)))
		elog(ERROR, "LionOrdered: malformed side of a join");

	/*
	 * What the node evaluates in place of the ordinary plan, initialised - and
	 * so checked for EXECUTE - every time the plan starts, EXPLAIN included,
	 * as an Index Scan initialises its indexqualorig (DESIGN.md §30.6).  The
	 * filter is core's (ExecInitCustomScan).
	 */
	st->lionrecheck = ExecInitQual((List *) list_nth(cscan->custom_exprs,
													  LO_EXPR_LIONQUAL),
								   &node->ss.ps);
	st->ordrecheck = ExecInitQual((List *) list_nth(cscan->custom_exprs,
													 LO_EXPR_ORDORIG),
								  &node->ss.ps);
	st->valuequal = ExecInitQual((List *) list_nth(cscan->custom_exprs,
													LO_EXPR_VALUEQUAL),
								 &node->ss.ps);
	st->valueentry = -1;

	st->nleaves = list_length(leafoids);
	st->leaves = (LoLeaf *) palloc0(sizeof(LoLeaf) * Max(st->nleaves, 1));
	st->noset = (tree == NIL);
	if (st->noset && !st->lionwalk && !st->side)
		elog(ERROR, "LionOrdered: a btree's walk without a lion set");
	if (!st->noset)
	{
		pos = list_head(tree);
		st->tree = lo_decode_tree(st, tree, &pos, leafoids, lionquals,
								  &leafno, &qualno);
		if (pos != NULL)
			elog(ERROR, "LionOrdered: malformed lion tree");
	}
	if (leafno != st->nleaves || qualno != list_length(lionquals))
		elog(ERROR, "LionOrdered: malformed lion tree");

	/* EXPLAIN without ANALYZE opens nothing (it names indexes by Oid). */
	if (eflags & EXEC_FLAG_EXPLAIN_ONLY)
		return;

	lion_check_table_am(node->ss.ss_currentRelation);
	if (!IsMVCCSnapshot(estate->es_snapshot))
		elog(ERROR, "LionOrdered: requires an MVCC snapshot");

	st->setcxt = AllocSetContextCreate(estate->es_query_cxt,
									   "LionOrdered set",
									   ALLOCSET_DEFAULT_SIZES);
	st->arena = NULL;
	st->arenafile = -1;
	st->buildcxt = AllocSetContextCreate(estate->es_query_cxt,
										 "LionOrdered build",
										 ALLOCSET_DEFAULT_SIZES);
	st->ortcxt = CreateExprContext(estate);
	st->scancxt = AllocSetContextCreate(estate->es_query_cxt,
										"LionOrdered scan",
										ALLOCSET_DEFAULT_SIZES);
	if (st->lionwalk)
		st->walkcxt = AllocSetContextCreate(estate->es_query_cxt,
											"LionOrdered walk",
											ALLOCSET_DEFAULT_SIZES);

	/*
	 * The fetch-and-sort switch's sort (§30.4), when the order has keys to
	 * sort by: its tuples are the keys, as the relation's columns are, and
	 * then the row's bytes; the rows a scan can be asked for at most bound
	 * it (LO_PRIV_LIMIT, a lionvec order's pause, which never switches).
	 */
	st->bound = st->knn ? -1 : priv.limit;
	if (priv.hassort && priv.nsort > 0)
	{
		TupleDesc	reldesc = RelationGetDescr(node->ss.ss_currentRelation);

		st->nsort = priv.nsort;
		st->sortattnos = priv.sortattno;
		st->sortops = priv.sortop;
		st->sortcolls = priv.sortcoll;
		st->sortnullsfirst = priv.nullsfirst;
		st->sortcols = (AttrNumber *) palloc(sizeof(AttrNumber) * st->nsort);
		st->sortdesc = CreateTemplateTupleDesc(st->nsort + 1);
		for (i = 0; i < st->nsort; i++)
		{
			AttrNumber	attno = st->sortattnos[i];

			if (attno < 1 || attno > reldesc->natts ||
				TupleDescAttr(reldesc, attno - 1)->attisdropped)
				elog(ERROR, "LionOrdered: malformed sort key %d", (int) attno);
			TupleDescCopyEntry(st->sortdesc, (AttrNumber) (i + 1), reldesc,
							   attno);
			st->sortcols[i] = (AttrNumber) (i + 1);
		}
		TupleDescInitBuiltinEntry(st->sortdesc, (AttrNumber) (st->nsort + 1),
								  "row", TEXTOID, -1, 0);
		TupleDescFinalize(st->sortdesc);
		st->sortin = ExecInitExtraTupleSlot(estate, st->sortdesc,
											&TTSOpsVirtual);
		st->sortout = ExecInitExtraTupleSlot(estate, st->sortdesc,
											 &TTSOpsMinimalTuple);
	}

	/* the switch's terms (§30.4, §40.4): the setting, and the planner's walk */
	st->switchratio = lion_ordered_switch_ratio;
	st->expectwalk = priv.expected;
	st->lrtcxt = CreateExprContext(estate);

	/* the ordered index and its scan keys */
	st->ordidx = index_open(st->ordoid, AccessShareLock);
	i = 0;
	foreach(pos, ordquals)
		fixed = lappend(fixed, lo_fix_qual((Expr *) lfirst(pos),
										   list_nth_int(priv.ordcols, i++)));
	ExecIndexBuildScanKeys(&node->ss.ps, st->ordidx, fixed, false,
						   &st->okeys, &st->nokeys,
						   &st->ortkeys, &st->nortkeys, NULL, NULL);

	/*
	 * A join's dimension (§47): its one key, `pk = NULL` as planned, takes
	 * the key the join hands it before each descent.
	 */
	if (st->keyed &&
		(st->nokeys != 1 || st->nortkeys != 0 ||
		 (st->okeys[0].sk_flags & SK_ISNULL) == 0 ||
		 st->okeys[0].sk_attno != 1 ||
		 st->okeys[0].sk_strategy != BTEqualStrategyNumber))
		elog(ERROR, "LionOrdered: malformed key of a join's dimension");
	if (st->knn)
	{
		/*
		 * A lionvec index's order (DESIGN.md §44.6): the index checked to be
		 * one - its scan is handed the set, which only lionvec takes - and
		 * its ORDER BY key, a run-time key among the btree's kind when the
		 * query vector is a Param, as an Index Scan builds them.
		 */
		if (!lion_vec_relation(st->ordidx))
			elog(ERROR, "LionOrdered: \"%s\" is not a lionvec index",
				 RelationGetRelationName(st->ordidx));
		ExecIndexBuildScanKeys(&node->ss.ps, st->ordidx,
							   list_make1(lo_fix_qual((Expr *) linitial(orderbys),
													  0)),
							   true, &st->obkeys, &st->nobkeys,
							   &st->ortkeys, &st->nortkeys, NULL, NULL);
	}
	if (st->indexonly)
	{
		/*
		 * Index-only mode (DESIGN.md §40.4): the btree's columns mapped to
		 * the relation's, a name column the btree stores as a cstring (its
		 * name_ops) copied back into a Name, as core's Index Only Scan copies
		 * it, and a slot of the table AM's for the heap tuple of a page not
		 * all-visible, read for its visibility alone.
		 */
		Relation	heap = node->ss.ss_currentRelation;
		TupleDesc	heapdesc = RelationGetDescr(heap);
		TupleDesc	idxdesc = RelationGetDescr(st->ordidx);
		List	   *iocols = priv.iocols;

		if (st->lionwalk || list_length(iocols) != idxdesc->natts)
			elog(ERROR, "LionOrdered: malformed index-only mapping");
		st->niocols = idxdesc->natts;
		st->iocols = (AttrNumber *) palloc(sizeof(AttrNumber) * st->niocols);
		st->ionames = (bool *) palloc0(sizeof(bool) * st->niocols);
		st->ionamebuf = (NameData *) palloc0(sizeof(NameData) * st->niocols);
		st->iovals = (Datum *) palloc(sizeof(Datum) * st->niocols);
		st->ionulls = (bool *) palloc(sizeof(bool) * st->niocols);
		for (i = 0; i < st->niocols; i++)
		{
			AttrNumber	attno = (AttrNumber) list_nth_int(iocols, i);

			if (attno < 0 || attno > heapdesc->natts)
				elog(ERROR, "LionOrdered: malformed index-only mapping");
			st->iocols[i] = attno;
			st->ionames[i] = attno > 0 &&
				TupleDescAttr(idxdesc, i)->atttypid == CSTRINGOID &&
				TupleDescAttr(heapdesc, attno - 1)->atttypid == NAMEOID;
		}
		st->tabslot = table_slot_create(heap, &estate->es_tupleTable);
	}
	if (st->lionwalk)
	{
		/*
		 * The walk reads the lion index without index_beginscan(), as the
		 * set does, and needs the heap fetch state of an index scan for what
		 * it meets; its leaves are predicate-locked as it reads them (§9).
		 */
#if PG_VERSION_NUM >= 200000
#elif PG_VERSION_NUM >= 190000
		st->fetch = table_index_fetch_begin(node->ss.ss_currentRelation,
											SO_NONE);
#else
		st->fetch = table_index_fetch_begin(node->ss.ss_currentRelation);
#endif
	}

	/*
	 * Every lion index; what the node reads of each is predicate-locked as
	 * it is read (DESIGN.md §9).
	 */
	for (i = 0; i < st->nleaves; i++)
	{
		LoLeaf	   *leaf = &st->leaves[i];

		leaf->index = index_open(leaf->indexoid, AccessShareLock);
		ExecIndexBuildScanKeys(&node->ss.ps, leaf->index, leaf->quals, false,
							   &leaf->keys, &leaf->nkeys,
							   &leaf->rtkeys, &leaf->nrtkeys, NULL, NULL);
	}

	/* which Params a rescan has to rebuild the set for */
	st->lionparams = pull_paramids((Expr *) lionquals);

	/* the set evaluated lazily, when every leaf's keys can be sought (§30.4) */
	st->lazyok = lion_enable_lazy_set && !st->knn &&
		(priv.flags & LO_FLAG_BUILD) == 0 && lo_lazy_ok(st);
	if (st->lazyok)
	{
		st->lazycxt = AllocSetContextCreate(estate->es_query_cxt,
											"LionOrdered lazy set",
											ALLOCSET_DEFAULT_SIZES);
		st->probecxt = AllocSetContextCreate(estate->es_query_cxt,
											 "LionOrdered probe",
											 ALLOCSET_DEFAULT_SIZES);
	}
}

/* The next TID of the ordered index, or NULL at its end. */
static ItemPointer
lo_next_tid(LionOrderedState *st)
{
	if (st->lionwalk)
	{
		if (!lion_order_walk_next(st->owalk, &st->walktid))
			return NULL;
		/* the btree's are counted by index_getnext_tid(), as for any scan */
		pgstat_count_index_tuples(st->ordidx, 1);
		return &st->walktid;
	}
#if PG_VERSION_NUM >= 200000
	if (!tableam_index_getnext_tid(st->scan, st->dir))
		return NULL;
	return &st->scan->xs_heaptid;
#else
	return index_getnext_tid(st->scan, st->dir);
#endif
}

/*
 * Fetch the version of the TID just returned that the snapshot sees, into
 * slot (DESIGN.md §30.4).  20 moved the heap side of index scans into the
 * table AM; there the HOT chain is searched with table_fetch_tid(), which
 * moves the TID to the visible version, and that version is then read.
 */
static bool
lo_fetch_btree(LionOrderedState *st, TupleTableSlot *slot)
{
#if PG_VERSION_NUM >= 200000
	IndexScanDesc scan = st->scan;
	ItemPointerData tid = scan->xs_heaptid;
	bool		all_dead = false;
	Snapshot	snapshot = scan->xs_snapshot;

	if (!table_fetch_tid(scan->heapRelation, &tid, snapshot, &all_dead))
	{
		/* as an index scan does: let the index mark a dead chain */
		if (!scan->xactStartedInRecovery)
			scan->kill_prior_tuple = all_dead;
		return false;
	}
	return table_tuple_fetch_row_version(scan->heapRelation, &tid, snapshot,
										 slot);
#else
	return index_fetch_heap(st->scan, slot);
#endif
}

/*
 * ... and of the TID a lion column's walk returned (DESIGN.md §30.11): the
 * root of a HOT chain, as an index's TID is, looked up the way an index scan
 * looks one up - on 16 .. 19 through the fetch state an index scan keeps,
 * which holds the pin on the last heap page between calls.  Nothing marks
 * dead entries: lion has no such hint.
 */
static bool
lo_fetch_walk(LionOrderedState *st, TupleTableSlot *slot)
{
	Snapshot	snapshot = st->css.ss.ps.state->es_snapshot;
	ItemPointerData tid = st->walktid;
	bool		all_dead = false;
	bool		found;
#if PG_VERSION_NUM >= 200000
	Relation	heap = st->css.ss.ss_currentRelation;

	found = table_fetch_tid(heap, &tid, snapshot, &all_dead) &&
		table_tuple_fetch_row_version(heap, &tid, snapshot, slot);
#else
	bool		call_again = false;

	found = table_index_fetch_tuple(st->fetch, &tid, snapshot, slot,
									&call_again, &all_dead);
#endif
	/* as index_fetch_heap() counts the rows an index scan fetches */
	if (found)
		pgstat_count_heap_fetch(st->ordidx);
	return found;
}

static bool
lo_fetch(LionOrderedState *st, TupleTableSlot *slot)
{
	return st->lionwalk ? lo_fetch_walk(st, slot) : lo_fetch_btree(st, slot);
}

/*
 * The row of the TID the btree just returned, in index-only mode (DESIGN.md
 * §40.4), as core's Index Only Scan makes it: when the visibility map calls
 * its page all-visible every tuple there is visible to every snapshot, this
 * one included, and the heap is not read; otherwise the heap tuple is fetched
 * - into the node's own slot, for its visibility - and a TID with no visible
 * version is skipped.  Either way the values are the index tuple's, which the
 * visible version reached from the root TID shares (an update of an indexed
 * or INCLUDE column is never HOT), laid into the scan slot through the
 * mapping, every other column NULL.  *fromheap says the heap was read: the
 * row then holds its tuple predicate lock already, and its heap tuple stays
 * in the node's slot for a recheck from the heap (lo_lion_recheck()) until
 * the caller is done with the row.
 */
static bool
lo_fetch_indexonly(LionOrderedState *st, TupleTableSlot *slot, bool *fromheap)
{
	IndexScanDesc scan = st->scan;
	ItemPointer tid = &scan->xs_heaptid;
	int			i;

	*fromheap = false;
	if (!VM_ALL_VISIBLE(scan->heapRelation, ItemPointerGetBlockNumber(tid),
						&st->vmbuffer))
	{
		st->fetched++;
		if (!lo_fetch_btree(st, st->tabslot))
			return false;
		*fromheap = true;
	}

	/* the index tuple, or the heap-format tuple an AM may return instead */
	if (scan->xs_hitup != NULL)
	{
		Assert(scan->xs_hitupdesc->natts == st->niocols);
		heap_deform_tuple(scan->xs_hitup, scan->xs_hitupdesc, st->iovals,
						  st->ionulls);
	}
	else if (scan->xs_itup != NULL)
	{
		Assert(scan->xs_itupdesc->natts == st->niocols);
		index_deform_tuple(scan->xs_itup, scan->xs_itupdesc, st->iovals,
						   st->ionulls);
	}
	else
		elog(ERROR, "LionOrdered: no index tuple returned in index-only mode");

	ExecClearTuple(slot);
	memset(slot->tts_isnull, true,
		   sizeof(bool) * slot->tts_tupleDescriptor->natts);
	for (i = 0; i < st->niocols; i++)
	{
		AttrNumber	attno = st->iocols[i];

		if (attno == 0)
			continue;
		if (st->ionames[i] && !st->ionulls[i])
		{
			namestrcpy(&st->ionamebuf[i], DatumGetCString(st->iovals[i]));
			st->iovals[i] = NameGetDatum(&st->ionamebuf[i]);
		}
		slot->tts_values[attno - 1] = st->iovals[i];
		slot->tts_isnull[attno - 1] = st->ionulls[i];
	}
	ExecStoreVirtualTuple(slot);
	slot->tts_tid = *tid;
	slot->tts_tableOid = RelationGetRelid(scan->heapRelation);
	return true;
}

/*
 * The lion recheck of a row the walk met, when the set is inexact
 * (DESIGN.md §30.4): on the scan slot, as every recheck is - or, in index-only
 * mode over a btree that lacks a column of the lion qual (§40.4), on the heap
 * tuple, fetched now into the node's table slot if the visibility test did
 * not fetch it already (and counted as a heap fetch; a TID with no visible
 * version is skipped).  Evaluating the recheck on that slot is sound although
 * it was initialised for the virtual scan slot: the scan slot's type is fixed
 * (ExecInitScanTupleSlot sets scanopsfixed), so ExecInitQual emitted no
 * EEOP_SCAN_FETCHSOME step for it (ExecComputeSlotInfo drops the step for a
 * fixed virtual slot, and with it the slot type check that step makes), and
 * its Var steps read tts_values[] and tts_isnull[] directly - which
 * slot_getallattrs() fills for the heap tuple.  *fromheap is set when the heap
 * was read here: the row then holds its tuple predicate lock.
 */
static bool
lo_lion_recheck(LionOrderedState *st, TupleTableSlot *slot, bool *fromheap)
{
	ExprContext *econtext = st->css.ss.ps.ps_ExprContext;
	TupleTableSlot *rslot = slot;

	if (st->indexonly && st->lionheaprecheck)
	{
		if (!*fromheap)
		{
			st->fetched++;
			if (!lo_fetch_btree(st, st->tabslot))
				return false;
			*fromheap = true;
		}
		slot_getallattrs(st->tabslot);
		rslot = st->tabslot;
	}
	ResetExprContext(econtext);
	econtext->ecxt_scantuple = rslot;
	if (ExecQual(st->lionrecheck, econtext))
		return true;
	st->removed++;
	return false;
}

/* End this scan's walk of a lion column, keeping its count of keys. */
static void
lo_walk_end(LionOrderedState *st)
{
	int64		entries;
	int64		leaves;

	if (st->owalk == NULL)
		return;
	lion_order_walk_counts(st->owalk, &entries, &leaves);
	st->walkkeys += (uint64) entries;
	lion_order_walk_end(st->owalk);
	st->owalk = NULL;
	MemoryContextReset(st->walkcxt);
}

static void
lo_start_walk(LionOrderedState *st)
{
	EState	   *estate = st->css.ss.ps.state;

	if (st->nortkeys > 0)
	{
		ResetExprContext(st->ortcxt);
		ExecIndexEvalRuntimeKeys(st->ortcxt, st->ortkeys, st->nortkeys);
	}
	if (st->lionwalk)
	{
		lo_walk_end(st);
		st->owalk = lion_order_walk_begin(st->ordidx, st->walkattno,
										  st->okeys, st->nokeys,
										  ScanDirectionIsBackward(st->dir),
										  st->walknulls, st->walkcxt);
		st->valueentry = -1;	/* a new walk numbers its entries afresh */
		pgstat_count_index_scan(st->ordidx);
		st->scans++;
		st->started = true;
		st->done = false;
		return;
	}
	if (st->scan == NULL)
	{
#if PG_VERSION_NUM >= 200000
		st->scan = index_beginscan(st->css.ss.ss_currentRelation, st->ordidx,
								   false, estate->es_snapshot, NULL,
								   st->nokeys, st->nobkeys, SO_NONE);
#elif PG_VERSION_NUM >= 190000
		st->scan = index_beginscan(st->css.ss.ss_currentRelation, st->ordidx,
								   estate->es_snapshot, NULL,
								   st->nokeys, st->nobkeys, SO_NONE);
#elif PG_VERSION_NUM >= 180000
		st->scan = index_beginscan(st->css.ss.ss_currentRelation, st->ordidx,
								   estate->es_snapshot, NULL, st->nokeys,
								   st->nobkeys);
#else
		st->scan = index_beginscan(st->css.ss.ss_currentRelation, st->ordidx,
								   estate->es_snapshot, st->nokeys,
								   st->nobkeys);
#endif
		/* index-only mode reads the index tuples (§40.4), as core's does */
		st->scan->xs_want_itup = st->indexonly;
	}
	index_rescan(st->scan, st->okeys, st->nokeys, st->obkeys, st->nobkeys);

	/*
	 * A lionvec scan reads and scores only the set's members (§44.6), and
	 * pauses and gathers by the rows the LIMIT takes (§44.4); every rescan
	 * forgets both, and a rebuilt set has moved.
	 */
	if (st->knn)
	{
		if (st->set == NULL)
			elog(ERROR, "LionOrdered: a lionvec order without its set");
		lion_vec_restrict(st->scan, st->set->n, st->set->keys,
						  st->set->conts);
		lion_vec_scan_limit(st->scan, st->knnlimit);
	}
	st->scans++;
	st->started = true;
	st->done = false;
}

/*
 * End the switch's sort, if this scan made one: its memory, under sortcxt,
 * and its temporary files, which its resource owner would otherwise keep to
 * the end of the query.
 */
static void
lo_sort_end(LionOrderedState *st)
{
	if (st->sortout != NULL)
		ExecClearTuple(st->sortout);
	if (st->sortstate != NULL)
		tuplesort_end(st->sortstate);
	st->sortstate = NULL;
}

/* Forget this scan's walk: what it met, and a switch it made. */
static void
lo_scan_reset(LionOrderedState *st)
{
	/*
	 * what the lazy set's memo says this scan met goes with scancxt - none
	 * of it in a nested loop's probe, which marks nothing (§40.10) and whose
	 * memo, kept for every scan of the loop, is not walked at each
	 */
	if (st->memo != NULL && !st->probe)
	{
		lo_memo_iterator it;
		LoMemoEnt  *e;

		LION_SH_START_ITERATE(lo_memo, st->memo, &it);
		while ((e = lo_memo_iterate(st->memo, &it)) != NULL)
		{
			if (e->visited != NULL)
				st->memobytes -= LION_BITSET_BYTES;
			e->visited = NULL;
		}
	}

	/* the visited bits, back to where lo_set_alloc() took them from */
	if (st->visited != NULL)
	{
		uint64		i;

		for (i = 0; i < st->nvisited; i++)
		{
			if (st->visited[i] != NULL)
				lo_set_free(st, st->visited[i], LO_VISITED_BYTES);
		}
		lo_set_free(st, st->visited, sizeof(uint64 *) * st->nvisited);
	}
	st->visited = NULL;
	st->nvisited = 0;

	/* the switch's sort, and its files, before the memory it lives in */
	lo_sort_end(st);
	if (st->scancxt != NULL)
		MemoryContextReset(st->scancxt);
	st->distinct = 0;
	st->scanwalked = 0;
	st->scanreturned = 0;
	st->sorting = false;
	st->sortcxt = NULL;
	st->sortitem = NULL;
	st->sortitemcap = 0;
	st->sortrow = NULL;
	st->sortrowcap = 0;
}

/*
 * Has this scan's walk met the member at `place` of the set's key idx
 * (DESIGN.md §30.4, "Stopping early")?  A place past the key's - only a
 * damaged container has a member there - never has been.
 */
static inline bool
lo_visited(LionOrderedState *st, int idx, uint32 place)
{
	uint64		p;
	uint64	   *chunk;

	if (st->visited == NULL ||
		(uint64) place >= st->places[idx + 1] - st->places[idx])
		return false;
	p = st->places[idx] + place;
	Assert(p / LO_VISITED_BITS < st->nvisited);
	chunk = st->visited[p / LO_VISITED_BITS];
	return chunk != NULL &&
		(chunk[(p % LO_VISITED_BITS) >> 6] & (UINT64CONST(1) << (p & 63))) != 0;
}

/*
 * Note that this scan's walk met the member at `place` of the set's key idx,
 * and say whether it is the first time (DESIGN.md §30.4, "Stopping early").
 * The bits are the set's places, a chunk of LO_VISITED_BITS made, cleared,
 * when the walk first meets a member of its places, through lo_set_alloc():
 * within hash_mem with the set, and past it in the arena, so the walk
 * counts whatever work_mem is.  A place past the key's - only a damaged
 * container has a member there - is a first meeting every time, and marks
 * nothing.
 */
static bool
lo_mark_place(LionOrderedState *st, int idx, uint32 place)
{
	uint64		p;
	uint64		c;
	uint64	   *w;
	uint64		bit;

	if ((uint64) place >= st->places[idx + 1] - st->places[idx])
		return true;
	p = st->places[idx] + place;
	if (st->visited == NULL)
	{
		uint64		total = st->places[st->set->n];
		uint64		n = (total + LO_VISITED_BITS - 1) / LO_VISITED_BITS;

		if (n > (uint64) (MaxAllocHugeSize / sizeof(uint64 *)))
			elog(ERROR, "LionOrdered: %llu places of members to mark",
				 (unsigned long long) total);
		st->visited = (uint64 **)
			lo_set_alloc(st, st->scancxt, sizeof(uint64 *) * (Size) n);
		memset(st->visited, 0, sizeof(uint64 *) * (Size) n);
		st->nvisited = n;
	}
	c = p / LO_VISITED_BITS;
	Assert(c < st->nvisited);
	if (st->visited[c] == NULL)
	{
		st->visited[c] = (uint64 *) lo_set_alloc(st, st->scancxt,
												 LO_VISITED_BYTES);
		memset(st->visited[c], 0, LO_VISITED_BYTES);
		/* EXPLAIN's spill: the set's, and its walks' bits past hash_mem */
		if (st->spilled)
			st->spillbytes = Max(st->spillbytes, lion_arena_used(st->arena));
	}
	w = &st->visited[c][(p % LO_VISITED_BITS) >> 6];
	bit = UINT64CONST(1) << (p & 63);
	if ((*w & bit) != 0)
		return false;
	*w |= bit;
	return true;
}

/*
 * lo_mark_place() for member tid of the set's key idx.  A TID met twice is
 * a recycled slot, never visible to the snapshot (§30.4).
 */
static bool
lo_mark(LionOrderedState *st, int idx, ItemPointer tid)
{
	uint16		lo = lion_code_lo(lion_tid_to_code(tid));

	return lo_mark_place(st, idx,
						 lion_container_place(st->set->conts[idx], lo));
}

/*
 * Is the fetch-and-sort switch due (DESIGN.md §30.4, "When the walk is not
 * paying")?  This scan's walk has met at least LO_SWITCH_MIN_WALK entries and
 * switchratio of them for every member it has not met - and, in index-only
 * mode (§40.4), markedly fewer members than a uniform spread of them over
 * the entries the planner expects it to visit would have given it by now:
 * the walk of a uniform filter under no LIMIT then never switches, which
 * would fetch every member left from a heap the walk never touches, while
 * the correlated hazard of §30.3 - members at the far end of the order,
 * `distinct` near zero as the entries go by - still switches at the measured
 * ratio.  `members` is the set's count, or the lazy set's estimate of it.
 */
static bool
lo_switch_due(LionOrderedState *st, double members)
{
	if (st->nsort == 0 || st->scanwalked < LO_SWITCH_MIN_WALK)
		return false;
	if ((double) st->scanwalked <
		st->switchratio * (members - (double) st->distinct))
		return false;
	if (st->indexonly && st->expectwalk > 0 &&
		(double) st->distinct >= LO_SWITCH_DENSITY * members *
		((double) st->scanwalked / st->expectwalk))
		return false;
	return true;
}

/*
 * A row into the switch's sort: its sort keys, and its heap tuple's bytes
 * behind the TID it goes by - the header too, so that what comes out is the
 * tuple as it was fetched, its system columns with it, as the walk would
 * have handed it out.  In index-only mode the slot is virtual and its tuple
 * is formed from its values (§40.4).
 */
static void
lo_sort_put(LionOrderedState *st, TupleTableSlot *slot, ItemPointer tid)
{
	HeapTuple	tup;
	bool		shouldfree;
	Size		need;
	int			k;

	tup = ExecFetchSlotHeapTuple(slot, false, &shouldfree);
	need = VARHDRSZ + sizeof(ItemPointerData) + (Size) tup->t_len;
	if (need > MaxAllocSize)
		elog(ERROR, "LionOrdered: a row of %u bytes for the switch's sort",
			 tup->t_len);
	if (need > st->sortitemcap)
	{
		Size		cap = Max(need, (Size) BLCKSZ);

		if (st->sortitem != NULL)
			pfree(st->sortitem);
		st->sortitem = (char *) MemoryContextAlloc(st->sortcxt, cap);
		st->sortitemcap = cap;
	}
	SET_VARSIZE(st->sortitem, need);
	memcpy(VARDATA(st->sortitem), tid, sizeof(ItemPointerData));
	memcpy(VARDATA(st->sortitem) + sizeof(ItemPointerData), tup->t_data,
		   tup->t_len);

	ExecClearTuple(st->sortin);
	for (k = 0; k < st->nsort; k++)
		st->sortin->tts_values[k] = slot_getattr(slot, st->sortattnos[k],
												 &st->sortin->tts_isnull[k]);
	st->sortin->tts_values[st->nsort] = PointerGetDatum(st->sortitem);
	st->sortin->tts_isnull[st->nsort] = false;
	ExecStoreVirtualTuple(st->sortin);
	tuplesort_puttupleslot(st->sortstate, st->sortin);
	ExecClearTuple(st->sortin);
	if (shouldfree)
		heap_freetuple(tup);
}

/*
 * The fetch-and-sort switch (DESIGN.md §30.4, "When the walk is not paying"):
 * fetch every member this scan's walk has not met, keep the visible ones that
 * pass the lion recheck (for an inexact set) and the ordered index's own
 * clauses (they did not come through the index), and sort them by the
 * pathkeys.  Every row the walk has not returned yet is among them - a
 * visible member's index entry lies where the walk has not been - and none
 * it has returned is.  The sort is a tuplesort, in work_mem and past it on
 * disk, as a Sort node's is; when the plan knows the most rows the scan can
 * be asked for (`bound`), it keeps only as many as are left of them, the
 * walk's rows counted, in a heap of those.  It never gives up: the rows
 * that do not fit in memory spill, and the walk is not left to go on.
 */
static void
lo_switch(LionOrderedState *st)
{
	Relation	heap = st->css.ss.ss_currentRelation;
	Snapshot	snapshot = st->css.ss.ps.state->es_snapshot;
	TupleTableSlot *slot = st->css.ss.ss_ScanTupleSlot;
	/* the heap tuple lands in the table AM's slot, then in the scan slot */
	TupleTableSlot *fslot = st->indexonly ? st->tabslot : slot;
	ExprContext *econtext = st->css.ss.ps.ps_ExprContext;
	uint16	   *los;
	uint32	   *places;
	MemoryContext oldcxt;
	int			i;

	Assert(st->nsort > 0 && st->sortstate == NULL);
	st->sortcxt = AllocSetContextCreate(st->scancxt, "LionOrdered sort",
										ALLOCSET_DEFAULT_SIZES);
	oldcxt = MemoryContextSwitchTo(st->sortcxt);
	st->sortstate = tuplesort_begin_heap(st->sortdesc, st->nsort,
										 st->sortcols, st->sortops,
										 st->sortcolls, st->sortnullsfirst,
										 work_mem, NULL,
										 TUPLESORT_ALLOWBOUNDED);
	if (st->bound > 0 && (uint64) st->bound > st->scanreturned)
		tuplesort_set_bound(st->sortstate,
							st->bound - (int64) st->scanreturned);
	los = (uint16 *) palloc(sizeof(uint16) * LION_CONTAINER_RANGE);
	places = (uint32 *) palloc(sizeof(uint32) * LION_CONTAINER_RANGE);
	MemoryContextSwitchTo(oldcxt);

	for (i = 0; i < st->set->n; i++)
	{
		uint32		n = lion_container_to_places(st->set->conts[i], los,
												 places);
		uint32		j;

		for (j = 0; j < n; j++)
		{
			ItemPointerData tid;
			bool		all_dead = false;

			if (lo_visited(st, i, places[j]))
				continue;
			CHECK_FOR_INTERRUPTS();
			lion_code_to_tid(lion_make_code(st->set->keys[i], los[j]), &tid);
			st->sortfetched++;
			/* a heap visit counts as the walk counts them in that mode (§40.6) */
			if (st->indexonly)
				st->fetched++;
			if (!lion_table_fetch_tid(heap, &tid, snapshot, &all_dead) ||
				!table_tuple_fetch_row_version(heap, &tid, snapshot, fslot))
				continue;
			/* rows fetched by the set's TIDs, as a bitmap heap scan's are */
			pgstat_count_heap_fetch(heap);
			if (!st->indexonly)
				st->fetched++;
			if (fslot != slot)
			{
				/* the scan slot's type is fixed per plan (§40.4) */
				ExecCopySlot(slot, fslot);
				ExecClearTuple(fslot);
			}
			ResetExprContext(econtext);
			econtext->ecxt_scantuple = slot;
			if (!st->exact && !ExecQual(st->lionrecheck, econtext))
			{
				st->removed++;
				continue;
			}
			if (!ExecQual(st->ordrecheck, econtext))
				continue;
			lo_sort_put(st, slot, &tid);
		}
	}
	ExecClearTuple(slot);
	pfree(los);
	pfree(places);

	tuplesort_performsort(st->sortstate);
	tuplesort_get_stats(st->sortstate, &st->sortstats);
	st->sorted = true;
	st->sorting = true;
	st->switches++;
}

/*
 * The switch's next row, as the walk would have handed it out: its heap
 * tuple, the TID it goes by, in the scan slot.
 */
static TupleTableSlot *
lo_sort_next(LionOrderedState *st, TupleTableSlot *slot)
{
	Oid			relid = RelationGetRelid(st->css.ss.ss_currentRelation);
	HeapTuple	tup;
	Datum		d;
	bool		isnull;
	const char *item;
	Size		len;
	Size		tlen;

	if (!tuplesort_gettupleslot(st->sortstate, true, false, st->sortout, NULL))
	{
		st->done = true;
		return ExecClearTuple(slot);
	}
	d = slot_getattr(st->sortout, st->nsort + 1, &isnull);
	len = isnull ? 0 : VARSIZE_ANY_EXHDR(DatumGetPointer(d));
	if (len < sizeof(ItemPointerData) + SizeofHeapTupleHeader)
		elog(ERROR, "LionOrdered: a row of %zu bytes out of the switch's sort",
			 len);
	item = VARDATA_ANY(DatumGetPointer(d));
	tlen = len - sizeof(ItemPointerData);

	/* in memory of its own, MAXALIGNed, as a heap tuple's header must be */
	if (HEAPTUPLESIZE + tlen > st->sortrowcap)
	{
		Size		cap = Max(HEAPTUPLESIZE + tlen, (Size) BLCKSZ);

		if (st->sortrow != NULL)
			pfree(st->sortrow);
		st->sortrow = (HeapTuple) MemoryContextAlloc(st->sortcxt, cap);
		st->sortrowcap = cap;
	}
	tup = st->sortrow;
	tup->t_data = (HeapTupleHeader) ((char *) tup + HEAPTUPLESIZE);
	memcpy(&tup->t_self, item, sizeof(ItemPointerData));
	memcpy(tup->t_data, item + sizeof(ItemPointerData), tlen);
	tup->t_len = (uint32) tlen;
	tup->t_tableOid = relid;

	/*
	 * A buffer slot copies the tuple; a virtual one (§40.4) points into it,
	 * and is given a copy of its own, as the next row overwrites it.
	 */
	ExecForceStoreHeapTuple(tup, slot, false);
	if (st->indexonly)
		ExecMaterializeSlot(slot);
	slot->tts_tid = tup->t_self;
	slot->tts_tableOid = relid;
	st->scanreturned++;
	return slot;
}

/*
 * Should the lazy set be built now, and why (LoLazyWhy), or -1: the walk
 * does not go in heap order, or the lazy set's probes have cost what
 * building it would, or the walk has gone as far as the switch would let it
 * go were the set as big as its entries' counts say - or has cost about what
 * the build will, so that the early stop and the switch have the set's size
 * to go by from there on - or the memo has outgrown its share of hash_mem
 * (§30.4, "The set, lazily").  Whether the switch comes is decided on the
 * set built, so a count that is off moves only when the set is built.
 */
static int
lo_lazy_why(LionOrderedState *st)
{
	if (st->lazyevals >= LO_LAZY_ORDER_KEYS &&
		st->lazyback * LO_LAZY_BACK_SHARE >= st->lazyevals)
		return LO_LAZY_HEAP_ORDER;
	if (st->lazywork >= st->lazybudget)
		return LO_LAZY_BUDGET;
	if (lo_switch_due(st, st->lazymembers))
		return LO_LAZY_SWITCH;
	if ((double) st->scanwalked >= Max((double) LO_SWITCH_MIN_WALK,
									   LO_LAZY_WALK_WORK * st->lazybudget))
		return LO_LAZY_LONG_WALK;
	if (st->memobytes > st->limit / 2)
		return LO_LAZY_MEMO;
	return -1;
}

/* ExecScan's access method: the next member with a visible version. */
static TupleTableSlot *
lo_next(ScanState *ss)
{
	LionOrderedState *st = (LionOrderedState *) ss;
	TupleTableSlot *slot = ss->ss_ScanTupleSlot;
	ExprContext *econtext = ss->ps.ps_ExprContext;

	st->ran = true;

	/*
	 * The set: lazily, evaluated only where the walk goes, when its keys
	 * can be sought (§30.4, "The set, lazily"); built otherwise - by one
	 * participant of a parallel walk for all of them, when the Gather has
	 * set one up (§40.9).
	 */
	if (st->set == NULL && !st->noset && !st->lazy &&
		!(st->lazyok && lo_lazy_begin(st)))
	{
		if (st->shared != NULL)
			lo_shared_set(st);
		else
			lo_build_set(st);
	}
	if (!st->started)
		lo_start_walk(st);

	/* an empty set selects nothing, and needs no walk */
	if (st->done ||
		(!st->noset && (st->lazy ? st->lazyempty : st->set->n == 0)) ||
		(st->narrowing && st->narrow->n == 0))
		return ExecClearTuple(slot);
	if (st->sorting)
		return lo_sort_next(st, slot);

	for (;;)
	{
		ItemPointer tid;
		int			idx;
		bool		counted;
		bool		ordrecheck;
		bool		keep;
		bool		fromheap = true;

		CHECK_FOR_INTERRUPTS();

		/* the lazy set built, when lo_lazy_why() says it is time */
		if (st->lazy)
		{
			int			why = lo_lazy_why(st);

			if (why >= 0)
			{
				st->lazywhy[why]++;
				lo_lazy_convert(st);
			}
		}
		counted = !st->noset && !st->lazy && !st->probe;

		/*
		 * Every member met: nothing further along can be one (§30.4,
		 * "Stopping early").  A member counts once however often the walk
		 * meets its TID.
		 */
		if (counted && st->distinct >= st->members)
		{
			st->earlystops++;
			break;
		}

		/*
		 * The walk has cost what fetching the members it has not met would:
		 * fetch and sort those instead (§30.4, "When the walk is not
		 * paying").
		 */
		if (counted && lo_switch_due(st, (double) st->members))
		{
			lo_switch(st);
			return lo_sort_next(st, slot);
		}

		tid = lo_next_tid(st);
		if (tid == NULL)
			break;
		st->walked++;
		st->scanwalked++;
		if (st->narrowing)
		{
			/*
			 * A LionOrderedSplit's arm (§48): the set narrowed to the rows
			 * of the arm's entry, which holds no TID the set does not.
			 */
			if (lo_set_find(st->narrow, tid) < 0)
				continue;
			st->distinct++;
			st->hits++;
		}
		else if (st->lazy)
		{
			LoMemoEnt  *e = lo_lazy_find(st, tid);

			if (e == NULL)
				continue;
			if (!st->probe && !lo_lazy_mark(st, e, tid))
				continue;		/* met before: a recycled slot (§30.4) */
			st->distinct++;
			st->hits++;
		}
		else if (!st->noset)
		{
			idx = lo_set_find(st->set, tid);
			if (idx < 0)
				continue;

			/*
			 * A nested loop's probe marks nothing (§40.10): its scans stop
			 * nowhere early and never switch, which are what the marks are
			 * counted for, and the visited bits' directory, made again for
			 * every scan, is as long as the set's places.  A TID met twice
			 * in one scan is then fetched twice; the second time it names a
			 * tuple written after the snapshot (§30.4), which the heap's
			 * visibility check skips.
			 */
			if (!st->probe && !lo_mark(st, idx, tid))
				continue;		/* met before: a recycled slot (§30.4) */
			st->distinct++;
			st->hits++;
		}
		if (st->indexonly)
		{
			/* the index tuple's values; the heap for visibility (§40.4) */
			if (!lo_fetch_indexonly(st, slot, &fromheap))
				continue;
		}
		else
		{
			if (!lo_fetch(st, slot))
				continue;
			st->fetched++;
		}

		/*
		 * A lion column's walk is exact: its entries are tested against the
		 * range with the column's own comparison (§28).  Without a set there
		 * is nothing to recheck either: the clauses are the filter.  In
		 * index-only mode the heap tuple fetched for the row, if any, is let
		 * go once the rechecks are done with it.
		 */
		ordrecheck = !st->lionwalk && st->scan->xs_recheck;
		keep = true;
		if (!st->noset && !st->exact)
			keep = lo_lion_recheck(st, slot, &fromheap);
		if (keep && ordrecheck)
		{
			ResetExprContext(econtext);
			econtext->ecxt_scantuple = slot;
			keep = ExecQual(st->ordrecheck, econtext);
		}
		if (st->indexonly && fromheap)
			ExecClearTuple(st->tabslot);
		if (!keep)
			continue;

		/*
		 * A filter on the walked column (DESIGN.md §30.11): the rows of one
		 * entry hold one value, byte for byte, so the first visible one
		 * decides for the entry - evaluated here on a visible row, as the
		 * filter above the node would, and one that fails takes the rest of
		 * the entry with it, unread.
		 */
		if (st->valuequal != NULL && st->lionwalk && st->owalk != NULL &&
			lion_order_walk_entryno(st->owalk) != st->valueentry)
		{
			st->valueentry = lion_order_walk_entryno(st->owalk);
			ResetExprContext(econtext);
			econtext->ecxt_scantuple = slot;
			if (!ExecQual(st->valuequal, econtext))
			{
				lion_order_walk_skip_entry(st->owalk);
				st->valueskips++;
				continue;
			}
		}

		/*
		 * A row returned without a heap visit has no tuple to predicate-lock
		 * (its xmin was never read): its page is locked, as core's Index Only
		 * Scan locks it (DESIGN.md §40.5).
		 */
		if (!fromheap)
			PredicateLockPage(ss->ss_currentRelation,
							  ItemPointerGetBlockNumber(&st->scan->xs_heaptid),
							  ss->ps.state->es_snapshot);
		st->scanreturned++;
		return slot;
	}
	st->done = true;
	return ExecClearTuple(slot);
}

/* ExecScan's recheck method (EvalPlanQual): the quals the scan answered. */
static bool
lo_recheck(ScanState *ss, TupleTableSlot *slot)
{
	LionOrderedState *st = (LionOrderedState *) ss;
	ExprContext *econtext = ss->ps.ps_ExprContext;

	/*
	 * The quals were initialised on the scan slot's type.  A row mark's ctid
	 * is a system column no index returns, so a locked relation never gets an
	 * index-only plan (§40.1) and the test tuple EvalPlanQual hands in is
	 * always a heap-mode one; the recheck from the heap of lo_lion_recheck()
	 * is index-only mode's alone and never meets this path.
	 */
	Assert(slot->tts_ops == ss->ss_ScanTupleSlot->tts_ops);
	ResetExprContext(econtext);
	econtext->ecxt_scantuple = slot;
	return ExecQual(st->lionrecheck, econtext) &&
		ExecQual(st->ordrecheck, econtext);
}

static TupleTableSlot *
lo_exec(CustomScanState *node)
{
	return ExecScan(&node->ss, (ExecScanAccessMtd) lo_next,
					(ExecScanRecheckMtd) lo_recheck);
}

static void
lo_rescan(CustomScanState *node)
{
	LionOrderedState *st = (LionOrderedState *) node;

	/*
	 * The walk restarts, with its keys evaluated again.  A nested loop's
	 * probe keeps the visibility map's page it read last, as core's Index
	 * Only Scan keeps it across its rescans: the next outer row's key is
	 * often on the same page (DESIGN.md §40.10).
	 */
	st->started = false;
	st->done = false;
	lo_scan_reset(st);
	if (BufferIsValid(st->vmbuffer) && !st->probe)
	{
		ReleaseBuffer(st->vmbuffer);
		st->vmbuffer = InvalidBuffer;
	}

	/*
	 * The set is rebuilt only when a Param of the lion quals changed; the
	 * snapshot is the same for the whole execution (DESIGN.md §30.4).
	 */
	if (node->ss.ps.chgParam != NULL &&
		bms_overlap(node->ss.ps.chgParam, st->lionparams))
	{
		st->set = NULL;
		if (st->lazy)
			lo_lazy_end(st);
	}

	ExecScanReScan(&node->ss);
}

static void
lo_end(CustomScanState *node)
{
	LionOrderedState *st = (LionOrderedState *) node;
	int			i;

	if (st->scan != NULL)
		index_endscan(st->scan);
	st->scan = NULL;
	if (BufferIsValid(st->vmbuffer))
		ReleaseBuffer(st->vmbuffer);
	st->vmbuffer = InvalidBuffer;
	if (st->tabslot != NULL)
		ExecClearTuple(st->tabslot);
	if (st->owalk != NULL)
		lo_walk_end(st);
	lo_sort_end(st);
#if PG_VERSION_NUM < 200000
	if (st->fetch != NULL)
		table_index_fetch_end(st->fetch);
	st->fetch = NULL;
#endif
	if (st->walkcxt != NULL)
		MemoryContextDelete(st->walkcxt);
	st->walkcxt = NULL;
	if (st->ordidx != NULL)
		index_close(st->ordidx, AccessShareLock);
	lo_lazy_end(st);
	for (i = 0; i < st->nleaves; i++)
	{
		if (st->leaves[i].index != NULL)
			index_close(st->leaves[i].index, AccessShareLock);
	}
	if (st->lazycxt != NULL)
		MemoryContextDelete(st->lazycxt);
	if (st->probecxt != NULL)
		MemoryContextDelete(st->probecxt);
	st->lazycxt = NULL;
	st->probecxt = NULL;
	if (st->setcxt != NULL)
		MemoryContextDelete(st->setcxt);
	if (st->buildcxt != NULL)
		MemoryContextDelete(st->buildcxt);
	if (st->scancxt != NULL)
		MemoryContextDelete(st->scancxt);
	st->scancxt = NULL;
	st->setcxt = NULL;
	st->buildcxt = NULL;
	st->set = NULL;
	if (st->arena != NULL)
	{
		/* the mappings first: the file is unlinked as it is closed */
		lion_arena_release(st->arena);
		FileClose(st->arenafile);
		st->arenafile = -1;
		st->arena = NULL;
	}
}

/* ---------------------------------------------------------------------
 * A parallel walk (DESIGN.md §40.9)
 *
 * The btree's parallel scan is core's: the descriptor in the Gather's
 * dynamic shared memory, initialised by the leader and joined by every
 * participant, hands each of them whole leaf pages, in the walk's direction,
 * as a Parallel Index Only Scan's participants get theirs.  What a
 * participant does with an entry - the probe, the visibility map, the heap
 * fetch of a page it does not call all-visible, the rechecks, the page's
 * predicate lock - is the serial node's, on the entries it is handed.
 * --------------------------------------------------------------------- */

/* The btree's parallel scan descriptor, as each major sizes it. */
static Size
lo_pscan_estimate(LionOrderedState *st, Snapshot snapshot, int nworkers)
{
#if PG_VERSION_NUM >= 190000
	return index_parallelscan_estimate(st->ordidx, st->nokeys, st->nobkeys,
									   snapshot);
#elif PG_VERSION_NUM >= 180000
	return index_parallelscan_estimate(st->ordidx, st->nokeys, st->nobkeys,
									   snapshot, false, true, nworkers);
#elif PG_VERSION_NUM >= 170000
	return index_parallelscan_estimate(st->ordidx, st->nokeys, st->nobkeys,
									   snapshot);
#else
	return index_parallelscan_estimate(st->ordidx, snapshot);
#endif
}

/* ... initialises it, in the leader ... */
static void
lo_pscan_initialize(LionOrderedState *st, Snapshot snapshot, int nworkers,
					ParallelIndexScanDesc pscan)
{
	Relation	heap = st->css.ss.ss_currentRelation;

#if PG_VERSION_NUM >= 190000 || PG_VERSION_NUM < 180000
	index_parallelscan_initialize(heap, st->ordidx, snapshot, pscan);
#else
	SharedIndexScanInstrumentation *sharedinfo = NULL;

	index_parallelscan_initialize(heap, st->ordidx, snapshot, false, true,
								  nworkers, &sharedinfo, pscan);
#endif
}

/* ... and joins it, in every participant */
static IndexScanDesc
lo_pscan_begin(LionOrderedState *st, ParallelIndexScanDesc pscan)
{
	Relation	heap = st->css.ss.ss_currentRelation;

#if PG_VERSION_NUM >= 200000
	return index_beginscan_parallel(heap, st->ordidx, false, NULL, st->nokeys,
									st->nobkeys, pscan, SO_NONE);
#elif PG_VERSION_NUM >= 190000
	return index_beginscan_parallel(heap, st->ordidx, NULL, st->nokeys,
									st->nobkeys, pscan, SO_NONE);
#elif PG_VERSION_NUM >= 180000
	return index_beginscan_parallel(heap, st->ordidx, NULL, st->nokeys,
									st->nobkeys, pscan);
#else
	return index_beginscan_parallel(heap, st->ordidx, st->nokeys,
									st->nobkeys, pscan);
#endif
}

/*
 * A participant joins the shared struct: the btree's parallel scan, in place
 * of a scan of its own - or of the scan of an earlier Gather run's memory,
 * which the leader has when the Gather was shut down and started again (a
 * LIMIT, then a rescan) - and the set, which an earlier run's view of
 * cannot be read any more.
 */
static void
lo_parallel_attach(LionOrderedState *st, LoShared *sh)
{
	if (st->setview)
	{
		st->set = NULL;
		st->setview = false;
	}
	if (st->scan != NULL)
		index_endscan(st->scan);
	st->scan = lo_pscan_begin(st,
							  (ParallelIndexScanDesc) ((char *) sh + sh->pscanoff));
	/* index-only mode reads the index tuples (§40.4), as core's does */
	st->scan->xs_want_itup = st->indexonly;
	st->shared = sh;
	st->counted = false;
}

static Size
lo_estimate_dsm(CustomScanState *node, ParallelContext *pcxt)
{
	LionOrderedState *st = (LionOrderedState *) node;

	return add_size(MAXALIGN(sizeof(LoShared)),
					lo_pscan_estimate(st, node->ss.ps.state->es_snapshot,
									  pcxt->nworkers));
}

/*
 * The leader sets the struct up, before the workers are launched.  The set's
 * room is a Parallel Hash's: a hash table's memory for each participant the
 * plan was made for, the leader among them - none with
 * pg_lion.enable_shared_set off.
 */
static void
lo_initialize_dsm(CustomScanState *node, ParallelContext *pcxt,
				  void *coordinate)
{
	LionOrderedState *st = (LionOrderedState *) node;
	LoShared   *sh = (LoShared *) coordinate;
	double		room = (double) get_hash_memory_limit() * (pcxt->nworkers + 1);

	memset(sh, 0, sizeof(LoShared));
	SpinLockInit(&sh->mutex);
	ConditionVariableInit(&sh->setcv);
	sh->setstate = LO_SHSET_NONE;
	sh->setimage = InvalidDsaPointer;
	sh->setroom = lion_enable_shared_set ?
		(Size) Min(room, (double) (SIZE_MAX / 2)) : 0;
	sh->pscanoff = MAXALIGN(sizeof(LoShared));
	lo_pscan_initialize(st, node->ss.ps.state->es_snapshot, pcxt->nworkers,
						(ParallelIndexScanDesc) ((char *) sh + sh->pscanoff));
	memset(&st->wseen, 0, sizeof(LoCounts));
	lo_parallel_attach(st, sh);
}

/*
 * Before the workers are launched again, for a rescan of the Gather: the
 * btree's parallel scan starts over, and the set is built again by the next
 * run, whose parameters may be others - its shared copy freed, and the
 * leader's view of it let go first: the rescan of the leader's own node may
 * come after this (ExecReScanGather()).  The sums are kept: they are the
 * whole execution's, as the leader's own counters are.  No participant is
 * running.
 */
static void
lo_reinitialize_dsm(CustomScanState *node, ParallelContext *pcxt,
					void *coordinate)
{
	LionOrderedState *st = (LionOrderedState *) node;
	LoShared   *sh = (LoShared *) coordinate;
	dsa_area   *dsa = node->ss.ps.state->es_query_dsa;

	st->shared = sh;
	if (st->setview)
	{
		st->set = NULL;
		st->setview = false;
	}
	if (DsaPointerIsValid(sh->setimage) && dsa != NULL)
		dsa_free(dsa, sh->setimage);
	sh->setimage = InvalidDsaPointer;
	sh->setstate = LO_SHSET_NONE;
	index_parallelrescan(st->scan);
}

static void
lo_initialize_worker(CustomScanState *node, shm_toc *toc, void *coordinate)
{
	lo_parallel_attach((LionOrderedState *) node, (LoShared *) coordinate);
}

/* a += b - c, counter by counter */
static void
lo_counts_add(LoCounts *a, const LoCounts *b, const LoCounts *c)
{
	a->walked += b->walked - c->walked;
	a->hits += b->hits - c->hits;
	a->fetched += b->fetched - c->fetched;
	a->removed += b->removed - c->removed;
	a->builds += b->builds - c->builds;
	a->published += b->published - c->published;
	a->earlystops += b->earlystops - c->earlystops;
}

/*
 * The end of a participant's execution.  A worker adds its counters to the
 * sums, once.  The leader takes what the sums gained since it last took them
 * - its ExecShutdownNode() reaches this node before the Gather above it
 * detaches the memory, and after the Gather has read every worker's last
 * row unless a LIMIT stopped it, which leaves the workers' counters out of
 * EXPLAIN and nothing else (as §27's parallel node does) - and lets go of
 * its view of the shared set, whose memory goes next.
 */
static void
lo_shutdown(CustomScanState *node)
{
	LionOrderedState *st = (LionOrderedState *) node;
	LoShared   *sh = st->shared;
	LoCounts	mine;
	LoCounts	zero;
	LoCounts	sums;

	if (sh == NULL)
		return;
	memset(&zero, 0, sizeof(LoCounts));
	if (IsParallelWorker())
	{
		if (st->counted)
			return;
		mine.walked = st->walked;
		mine.hits = st->hits;
		mine.fetched = st->fetched;
		mine.removed = st->removed;
		mine.builds = st->builds;
		mine.published = st->published;
		mine.earlystops = st->earlystops;
		SpinLockAcquire(&sh->mutex);
		lo_counts_add(&sh->sums, &mine, &zero);
		SpinLockRelease(&sh->mutex);
		st->counted = true;
		return;
	}

	SpinLockAcquire(&sh->mutex);
	sums = sh->sums;
	st->wncont = sh->ncont;
	st->wexact = sh->exact;
	st->wspill = Max(st->wspill, sh->spillbytes);
	SpinLockRelease(&sh->mutex);
	lo_counts_add(&st->wsums, &sums, &st->wseen);
	st->wseen = sums;
	if (st->setview)
	{
		st->set = NULL;
		st->setview = false;
	}
	st->shared = NULL;
}

/* ---------------------------------------------------------------------
 * EXPLAIN (DESIGN.md §30.7)
 * --------------------------------------------------------------------- */

static void
lo_explain_qual(CustomScanState *node, List *qual, const char *label,
				List *ancestors, ExplainState *es)
{
	List	   *context;
	char	   *str;

	if (qual == NIL)
		return;
	context = set_deparse_context_plan(es->deparse_cxt, node->ss.ps.plan,
									   ancestors);
	str = deparse_expression((Node *) make_ands_explicit(qual), context,
							 lion_explain_useprefix(es), false);
	ExplainPropertyText(label, str, es);
}

/*
 * A parallel walk's counters under ANALYZE (DESIGN.md §40.9): the leader's
 * own and the workers' sums, which it took as it shut down; and the set as
 * the leader had it, or as its last builder saw it when the leader never
 * had one (parallel_leader_participation off) - "shared" when the
 * participants read one copy of it, and the builds of all of them.  A
 * parallel walk has no lazy set, no switch and no lion column walk.
 */
static void
lo_explain_parallel(LionOrderedState *st, StringInfo buf, ExplainState *es)
{
	LoCounts	t = st->wsums;
	bool		hadset = (st->builds > 0 || st->attaches > 0);
	int			ncont = hadset ? st->ncont : st->wncont;
	bool		exact = hadset ? st->exact : st->wexact;
	Size		spill = Max(st->spillbytes, st->wspill);

	t.walked += st->walked;
	t.hits += st->hits;
	t.fetched += st->fetched;
	t.removed += st->removed;
	t.builds += st->builds;
	t.published += st->published;
	t.earlystops += st->earlystops;

	ExplainPropertyInteger("Index Entries Walked", NULL, (int64) t.walked, es);
	ExplainPropertyInteger("Lion Set Hits", NULL, (int64) t.hits, es);
	ExplainPropertyInteger("Heap Fetches", NULL, (int64) t.fetched, es);
	if (t.removed > 0 || (t.builds > 0 && !exact))
		ExplainPropertyInteger("Rows Removed by Lion Recheck", NULL,
							   (int64) t.removed, es);
	if (t.builds > 0)
	{
		resetStringInfo(buf);
		appendStringInfo(buf, "%d containers, %s", ncont,
						 exact ? "exact" : "rechecked");
		if (spill > 0)
			appendStringInfo(buf, ", spilled %zu kB", (spill + 1023) / 1024);
		if (t.published > 0)
			appendStringInfoString(buf, ", shared");
		if (t.builds > 1)
			appendStringInfo(buf, ", %llu builds",
							 (unsigned long long) t.builds);
		ExplainPropertyText("Lion Set", buf->data, es);
	}
	if (t.earlystops > 0)
		ExplainPropertyInteger("Scans Stopped Early", NULL,
							   (int64) t.earlystops, es);
}

/* An EXPLAIN label, behind a join's prefix for its side (§47) */
static const char *
lo_label(const char *prefix, const char *label)
{
	if (prefix == NULL || *prefix == '\0')
		return label;
	return psprintf("%s%s", prefix, label);
}

/*
 * What the plan says: the walk, its quals, the set's - and, for a side of a
 * LionOrderedJoin (DESIGN.md §47), its filter, which core would print for a
 * node of its own, each label behind the join's prefix for it.  A join's
 * dimension names its index on the join's own line, and its descent's key is
 * the join's clause: neither is repeated.
 */
static void
lo_explain_lines(LionOrderedState *st, List *ancestors, ExplainState *es,
				 const char *prefix)
{
	CustomScanState *node = &st->css;
	CustomScan *cscan = (CustomScan *) node->ss.ps.plan;
	StringInfoData buf;
	const char *name;
	int			i;

	initStringInfo(&buf);
	name = get_rel_name(st->ordoid);
	appendStringInfoString(&buf, name != NULL ? name : "???");
	if (st->lionwalk)
	{
		/* the column walked, as the index names it (§30.11) */
		appendStringInfo(&buf, " (%s%s%s)",
						 quote_identifier(get_attname(st->ordoid,
													  st->walkattno, false)),
						 ScanDirectionIsBackward(st->dir) ? ", backward" : "",
						 st->walknulls == LION_ORDER_NULLS_FIRST ?
						 ", nulls first" :
						 st->walknulls == LION_ORDER_NULLS_LAST ?
						 ", nulls last" : "");
	}
	else if (ScanDirectionIsBackward(st->dir) || st->indexonly)
		appendStringInfo(&buf, " (%s%s%s)",
						 ScanDirectionIsBackward(st->dir) ? "backward" : "",
						 ScanDirectionIsBackward(st->dir) && st->indexonly ?
						 ", " : "",
						 st->indexonly ? "index only" : "");
	/* a LionBtreeScan claims no order: the index it walks (§40.6) */
	if (!st->keyed)
		ExplainPropertyText(lo_label(prefix,
									 cscan->methods == &lo_btree_scan_methods ?
									 "Index" : "Ordered By"),
							buf.data, es);

	/*
	 * The walk's index quals, key on the left, as core prints an Index Scan's
	 * (its indexqualorig is the same list): `100 > k` as `(k < 100)`, and a
	 * nested loop's probe's join clause as `(d.id = f.dim_id)` however the
	 * query wrote it (DESIGN.md §40.10).  A lossy clause's original is the
	 * Filter's, as there.
	 */
	if (!st->keyed)
		lo_explain_qual(node, (List *) list_nth(cscan->custom_exprs,
												LO_EXPR_ORDQUALS),
						lo_label(prefix, "Index Cond"), ancestors, es);
	if (st->side)
		lo_explain_qual(node, cscan->scan.plan.qual,
						lo_label(prefix, "Filter"), ancestors, es);
	lo_explain_qual(node, (List *) list_nth(cscan->custom_exprs,
											LO_EXPR_ORDERBYS),
					lo_label(prefix, "Order By"), ancestors, es);
	lo_explain_qual(node, (List *) list_nth(cscan->custom_exprs, LO_EXPR_LIONQUAL),
					lo_label(prefix, "Lion Cond"), ancestors, es);
	lo_explain_qual(node, (List *) list_nth(cscan->custom_exprs,
											LO_EXPR_VALUEQUAL),
					lo_label(prefix, "Filter per Value"), ancestors, es);

	resetStringInfo(&buf);
	for (i = 0; i < st->nleaves; i++)
	{
		int			j;

		name = get_rel_name(st->leaves[i].indexoid);
		if (name == NULL)
			name = "???";
		/* each index once, in the order the tree first names it */
		for (j = 0; j < i; j++)
			if (st->leaves[j].indexoid == st->leaves[i].indexoid)
				break;
		if (j < i)
			continue;
		if (buf.len > 0)
			appendStringInfoString(&buf, ", ");
		appendStringInfoString(&buf, name);
	}
	/* a lion column's walk without a set reads no other lion index */
	if (buf.len > 0)
		ExplainPropertyText(lo_label(prefix, "Lion Indexes"), buf.data, es);
	pfree(buf.data);
}

/*
 * What EXPLAIN ANALYZE counted, each label behind a join's prefix for its
 * side (§47) - whose descents are its dimension's walks, and whose filter's
 * rejections core would count for a node of its own.
 */
static void
lo_explain_counters(LionOrderedState *st, ExplainState *es,
					const char *prefix)
{
	CustomScanState *node = &st->css;
	StringInfoData buf;

	if (!es->analyze)
		return;
	initStringInfo(&buf);

	/*
	 * A serial node below a Gather that only workers ran - under
	 * debug_parallel_query, or the inner side of a join a worker ran - has
	 * its counters in those workers, which do not hand them back as a
	 * parallel walk's do (§40.9): none are shown rather than this process's
	 * zeros.  Its rows and loops are core's, summed over the workers.
	 */
	if (!st->parallel && !st->ran &&
		node->ss.ps.instrument != NULL && node->ss.ps.instrument->nloops > 0)
		;
	else if (st->parallel)
		lo_explain_parallel(st, &buf, es);
	else
	{
		if (st->keyed)
			ExplainPropertyInteger(lo_label(prefix, "Descents"), NULL,
								   (int64) st->scans, es);
		else
			ExplainPropertyInteger(lo_label(prefix, "Index Entries Walked"),
								   NULL, (int64) st->walked, es);
		if (st->lionwalk)
		{
			int64		entries = 0;
			int64		leaves;

			if (st->owalk != NULL)
				lion_order_walk_counts(st->owalk, &entries, &leaves);
			ExplainPropertyInteger("Lion Keys Walked", NULL,
								   (int64) st->walkkeys + entries, es);
			if (st->valuequal != NULL)
				ExplainPropertyInteger("Lion Keys Filtered", NULL,
									   (int64) st->valueskips, es);
		}
		if (!st->noset)
			ExplainPropertyInteger(lo_label(prefix, "Lion Set Hits"), NULL,
								   (int64) st->hits, es);
		ExplainPropertyInteger(lo_label(prefix, "Heap Fetches"), NULL,
							   (int64) st->fetched, es);
		/* a node that never made its set has no exactness to report */
		if (st->removed > 0 ||
			((st->builds > 0 || st->lazykeys > 0) && !st->exact))
			ExplainPropertyInteger(lo_label(prefix,
											"Rows Removed by Lion Recheck"),
								   NULL, (int64) st->removed, es);
		if (st->side && node->ss.ps.plan->qual != NIL &&
			node->ss.ps.instrument != NULL)
			ExplainPropertyInteger(lo_label(prefix, "Rows Removed by Filter"),
								   NULL,
								   (int64) node->ss.ps.instrument->nfiltered1,
								   es);
		if (st->builds > 0)
		{
			resetStringInfo(&buf);
			appendStringInfo(&buf, "%d containers, %s", st->ncont,
							 st->exact ? "exact" : "rechecked");
			if (st->spillbytes > 0)
				appendStringInfo(&buf, ", spilled %zu kB",
								 (st->spillbytes + 1023) / 1024);
			if (st->builds > 1)
				appendStringInfo(&buf, ", %llu builds",
								 (unsigned long long) st->builds);
			if (st->lazykeys > 0)
			{
				int			w;
				const char *sep = " (";

				appendStringInfo(&buf, ", after %llu keys probed",
								 (unsigned long long) st->lazykeys);
				for (w = 0; w < LO_LAZY_NWHY; w++)
				{
					if (st->lazywhy[w] == 0)
						continue;
					appendStringInfo(&buf, "%s%s", sep, lo_lazy_why_names[w]);
					if (st->lazywhy[w] > 1)
						appendStringInfo(&buf, " x%llu",
										 (unsigned long long) st->lazywhy[w]);
					sep = ", ";
				}
				if (*sep == ',')
					appendStringInfoChar(&buf, ')');
			}
			ExplainPropertyText(lo_label(prefix, "Lion Set"), buf.data, es);
		}
		else if (st->lazykeys > 0)
		{
			/* the set was never built: only the keys the walk met (§30.4) */
			resetStringInfo(&buf);
			appendStringInfo(&buf, "lazy, %llu keys probed, %s",
							 (unsigned long long) st->lazykeys,
							 st->exact ? "exact" : "rechecked");
			ExplainPropertyText(lo_label(prefix, "Lion Set"), buf.data, es);
		}
		if (st->switches > 0)
		{
			resetStringInfo(&buf);
			appendStringInfo(&buf, "%llu of %llu scans, %llu members fetched",
							 (unsigned long long) st->switches,
							 (unsigned long long) st->scans,
							 (unsigned long long) st->sortfetched);
			ExplainPropertyText(lo_label(prefix, "Switched to Fetch and Sort"),
								buf.data, es);
		}
		if (st->sorted)
		{
			/* the last switch's sort, as a Sort node shows its last run's */
			ExplainPropertyText(lo_label(prefix, "Fetch and Sort Method"),
								tuplesort_method_name(st->sortstats.sortMethod),
								es);
			ExplainPropertyInteger(lo_label(prefix, "Fetch and Sort Space Used"),
								   "kB", st->sortstats.spaceUsed, es);
			ExplainPropertyText(lo_label(prefix, "Fetch and Sort Space Type"),
								tuplesort_space_type_name(st->sortstats.spaceType),
								es);
		}
		if (st->earlystops > 0)
			ExplainPropertyInteger(lo_label(prefix, "Scans Stopped Early"),
								   NULL, (int64) st->earlystops, es);
	}
	pfree(buf.data);
}

static void
lo_explain(CustomScanState *node, List *ancestors, ExplainState *es)
{
	LionOrderedState *st = (LionOrderedState *) node;

	lo_explain_lines(st, ancestors, es, NULL);
	lo_explain_counters(st, es, NULL);
}

/*
 * A side of a LionOrderedJoin (DESIGN.md §47), which its join keeps out of
 * the plan tree EXPLAIN walks and prints in its own lines: what the plan says
 * of it, or what EXPLAIN ANALYZE counted, each label behind `prefix`.
 * ancestors are the join's, the join in front.
 */
void
lion_ordered_explain_side(PlanState *ps, List *ancestors, ExplainState *es,
						  const char *prefix, bool counters)
{
	LionOrderedState *st = (LionOrderedState *) ps;

	if (!IsA(ps, CustomScanState) ||
		((CustomScanState *) ps)->methods != &lo_exec_methods || !st->side)
		elog(ERROR, "LionOrdered: not a side of a join");
	if (counters)
		lo_explain_counters(st, es, prefix);
	else
		lo_explain_lines(st, ancestors, es, prefix);
}

/*
 * Hand a join's dimension (DESIGN.md §47) the key of its next descent, and
 * restart its walk there: the next row it returns is the dimension's row of
 * that key, if it has a visible one that is a member and passes its filter.
 * key must stay valid until that row has been read: it is the scan key's
 * argument.  The set is kept, as a nested loop's probe keeps it (§40.10),
 * unless a Param of its lion quals has changed.
 */
void
lion_ordered_probe_key(PlanState *ps, Datum key)
{
	LionOrderedState *st = (LionOrderedState *) ps;

	if (!IsA(ps, CustomScanState) ||
		((CustomScanState *) ps)->methods != &lo_exec_methods || !st->keyed ||
		st->nokeys != 1)
		elog(ERROR, "LionOrdered: not a join's dimension");
	st->okeys[0].sk_argument = key;
	st->okeys[0].sk_flags &= ~SK_ISNULL;
	ExecReScan(ps);
}

/* ---------------------------------------------------------------------
 * A LionOrderedSplit's arm (lion_ordjoin.c, DESIGN.md §48): a join's
 * dimension narrowed to the rows of one entry of the split's column
 * --------------------------------------------------------------------- */

static LionOrderedState *
lo_narrow_state(PlanState *ps)
{
	LionOrderedState *st = (LionOrderedState *) ps;

	if (!IsA(ps, CustomScanState) ||
		((CustomScanState *) ps)->methods != &lo_exec_methods || !st->keyed)
		elog(ERROR, "LionOrdered: not a join's dimension");
	return st;
}

/*
 * The set's container at ckey, or NULL where it has no member there: the
 * built set's, or the lazy set's, evaluated there and remembered as a
 * descent's TID would have it (lo_lazy_entry()).
 */
static const LionContainer *
lo_set_at(LionOrderedState *st, uint32 ckey)
{
	int			idx;

	if (st->lazy)
		return st->lazyempty ? NULL : lo_lazy_entry(st, ckey)->c;
	idx = lo_set_key_index(st->set, ckey);
	return idx < 0 ? NULL : st->set->conts[idx];
}

/*
 * The entry's TIDs of the container key narrowbuf holds, ANDed with the
 * set's container of the key - without a set, all of them - into the
 * narrowed set, when that leaves any.
 */
static void
lo_narrow_flush(LionOrderedState *st)
{
	const LionContainer *keep = st->narrowbuf;
	LionTidSet *set = st->narrow;
	LionContainer *copy;
	Size		size;

	if (!st->narrowopen)
		return;
	st->narrowopen = false;
	lion_container_optimize(st->narrowbuf);
	if (!st->noset)
	{
		const LionContainer *c = lo_set_at(st, st->narrowbuf->ckey);

		if (c == NULL || lion_container_and(st->narrowbuf, c,
											st->narrowand) == 0)
			return;
		keep = st->narrowand;
	}
	if (set->n == set->cap)
	{
		set->cap *= 2;
		set->keys = (uint32 *) repalloc(set->keys, sizeof(uint32) * set->cap);
		set->conts = (LionContainer **)
			repalloc(set->conts, sizeof(LionContainer *) * set->cap);
	}
	size = lion_container_size(keep);
	copy = (LionContainer *) MemoryContextAlloc(st->narrowcxt, size);
	memcpy(copy, keep, size);
	set->keys[set->n] = keep->ckey;
	set->conts[set->n] = copy;
	set->n++;
	st->narrowmembers += keep->cardinality;
}

/*
 * Begin an arm: the dimension's set, made as its first descent would make it
 * (lo_next()) - built, or lazy - and kept for every arm, unless a Param of
 * its lion quals has changed since the last; and the narrowed set emptied,
 * for lion_ordered_narrow_add() to fill.  The dimension returns no row until
 * lion_ordered_narrow_end().
 */
void
lion_ordered_narrow_begin(PlanState *ps)
{
	LionOrderedState *st = lo_narrow_state(ps);
	MemoryContext oldcxt;

	/* a changed Param of its lion quals: the set made again (lo_rescan()) */
	if (ps->chgParam != NULL)
		ExecReScan(ps);
	if (st->set == NULL && !st->noset && !st->lazy &&
		!(st->lazyok && lo_lazy_begin(st)))
		lo_build_set(st);
	if (st->narrowcxt == NULL)
	{
		MemoryContext qcxt = st->css.ss.ps.state->es_query_cxt;

		st->narrowcxt = AllocSetContextCreate(qcxt, "LionOrdered arm",
											  ALLOCSET_DEFAULT_SIZES);
		st->narrowbuf = (LionContainer *)
			MemoryContextAlloc(qcxt, LION_CONTAINER_MAX_SIZE);
		st->narrowand = (LionContainer *)
			MemoryContextAlloc(qcxt, LION_CONTAINER_MAX_SIZE);
	}
	MemoryContextReset(st->narrowcxt);
	oldcxt = MemoryContextSwitchTo(st->narrowcxt);
	st->narrow = (LionTidSet *) palloc0(sizeof(LionTidSet));
	st->narrow->cap = 16;
	st->narrow->keys = (uint32 *) palloc(sizeof(uint32) * st->narrow->cap);
	st->narrow->conts = (LionContainer **)
		palloc(sizeof(LionContainer *) * st->narrow->cap);
	MemoryContextSwitchTo(oldcxt);
	st->narrowopen = false;
	st->narrowlast = 0;
	st->narrowmembers = 0;
	st->narrowing = true;
}

/*
 * One TID of the arm's entry, in the order the column's walk hands them out:
 * heap order (DESIGN.md §30.11), each container key's TIDs together, which
 * are ANDed with the set's as the next key begins.  A key below the one
 * before it is an entry out of order, which nothing but damage makes.
 */
void
lion_ordered_narrow_add(PlanState *ps, ItemPointer tid)
{
	LionOrderedState *st = (LionOrderedState *) ps;
	uint64		code = lion_tid_to_code(tid);
	uint32		ckey = lion_code_ckey(code);

	Assert(st->narrowing);
	if (st->narrowopen && ckey != st->narrowlast)
		lo_narrow_flush(st);
	if (!st->narrowopen)
	{
		if (ckey < st->narrowlast)
			ereport(ERROR,
					(errcode(ERRCODE_INDEX_CORRUPTED),
					 errmsg("LionOrderedSplit: an entry's TIDs out of heap order")));
		lion_container_init(st->narrowbuf, ckey);
		st->narrowopen = true;
		st->narrowlast = ckey;
	}
	lion_container_append_sorted(st->narrowbuf, lion_code_lo(code));
}

/*
 * End the arm's entry: the members of the narrowed set, which the dimension
 * is narrowed to until the next arm begins.  0: the entry has no row of the
 * set, and the arm need not run.
 */
uint64
lion_ordered_narrow_end(PlanState *ps)
{
	LionOrderedState *st = lo_narrow_state(ps);

	lo_narrow_flush(st);
	return st->narrowmembers;
}

/* ---------------------------------------------------------------------
 * What LionProbeJoin takes from the walk (lion_probejoin.c, DESIGN.md §46)
 *
 * A dimension probed by a fact's rows through its primary key is a nested
 * loop's probe (§40.10) whose sets are the dimension's WHERE and the labels
 * of its cells: the same lion accesses, the same price for the descent, and
 * the same sets - built from lion sources a container at a time, held within
 * hash_mem and spilled past it - so they are this file's, lent out here.
 * --------------------------------------------------------------------- */

/*
 * Whether a lion access answers the clauses its qual names exactly: none of
 * its index clauses lossy.  A lossy one - the range core derives from a
 * LIKE prefix, `code >= 'k1' AND code < 'k2'` for `code LIKE 'k1%7'` -
 * still names the original clause, which lo_residual() then counts as
 * answered, while the set its leaf builds holds every row of the range and
 * says it is exact; the probe would count rows the clause rejects, with no
 * recheck to stop it (2026-10-23 review).  A probe declines such an access.
 */
static bool
lo_where_exact(Path *cand, List **rinfos, List **qual)
{
	bool		lossy = false;

	*qual = lo_lion_qual(cand, rinfos, &lossy);
	return !lossy;
}

/*
 * A BitmapAnd that answers every restriction clause of rel when no access
 * core builds does: core's choice of an AND (choose_bitmap_and()) leaves out
 * a leaf that selects too little to pay for its lookups in a bitmap scan -
 * `active` of two rows in three - where the probe's set must answer it all
 * the same, or read the heap for it.  Each clause the leaves before it do
 * not answer gets the cheapest exact access core builds for it alone
 * (lo_where_exact()); NULL when a clause has none.
 */
static Path *
lo_where_and(PlannerInfo *root, RelOptInfo *rel, List *lionidx, Oid lionam,
			 List **qual)
{
	List	   *parts = NIL;
	List	   *rinfos = NIL;
	List	   *quals = NIL;
	List	   *andrinfos = NIL;
	Path	   *and;
	ListCell   *lc;

	foreach(lc, rel->baserestrictinfo)
	{
		RestrictInfo *rinfo = lfirst_node(RestrictInfo, lc);
		List	   *others;
		List	   *cands;
		Path	   *best = NULL;
		Cost		bestcost = 0;
		List	   *bestrinfos = NIL;
		List	   *bestqual = NIL;
		ListCell   *lc2;

		if (rinfo->pseudoconstant ||
			lo_residual(list_make1(rinfo), NIL, rinfos, quals) == NIL)
			continue;
		others = list_delete_ptr(list_copy(rel->baserestrictinfo), rinfo);
		cands = lo_lion_accesses(root, rel, lionidx, others, lionam);
		foreach(lc2, cands)
		{
			Path	   *cand = (Path *) lfirst(lc2);
			List	   *crinfos = NIL;
			List	   *cqual;
			Cost		cost;
			Selectivity sel;

			if (!lo_where_exact(cand, &crinfos, &cqual) ||
				lo_residual(list_make1(rinfo), NIL, crinfos, cqual) != NIL)
				continue;
			cost_bitmap_tree_node(cand, &cost, &sel);
			if (best == NULL || cost < bestcost)
			{
				best = cand;
				bestcost = cost;
				bestrinfos = crinfos;
				bestqual = cqual;
			}
		}
		if (best == NULL)
			return NULL;
		parts = lappend(parts, best);
		rinfos = list_concat(rinfos, bestrinfos);
		quals = list_concat(quals, bestqual);
	}
	if (parts == NIL)
		return NULL;
	and = (list_length(parts) == 1) ? (Path *) linitial(parts) :
		(Path *) create_bitmap_and_path(root, rel, parts);
	if (!lo_where_exact(and, &andrinfos, &quals) ||
		lo_residual(rel->baserestrictinfo, NIL, andrinfos, quals) != NIL)
		return NULL;
	*qual = quals;
	return and;
}

/*
 * The lion access that answers every restriction clause of rel - the
 * cheapest of those core builds over its lion indexes (lo_lion_accesses()),
 * none of its index clauses lossy (lo_where_exact()), with nothing left to
 * a filter (lo_residual()) - into *lion, and its original qual into
 * *lionqual, as a recheck evaluates it.  False when no access answers them
 * all; true with *lion NULL when rel has no restriction clause.  The
 * caller has checked the relation (§46, "What qualifies").
 */
bool
lion_ordered_where_access(PlannerInfo *root, RelOptInfo *rel, Path **lion,
						  List **lionqual)
{
	Oid			lionam = lion_get_am_oid();
	List	   *lionidx = NIL;
	List	   *cands;
	Path	   *best = NULL;
	Cost		bestcost = 0;
	List	   *bestqual = NIL;
	ListCell   *lc;

	*lion = NULL;
	*lionqual = NIL;
	if (rel->baserestrictinfo == NIL)
		return true;
	if (!OidIsValid(lionam))
		return false;
	foreach(lc, rel->indexlist)
	{
		IndexOptInfo *idx = (IndexOptInfo *) lfirst(lc);

		if (idx->relam == lionam && !idx->hypothetical)
			lionidx = lappend(lionidx, idx);
	}
	if (lionidx == NIL)
		return false;

	cands = lo_lion_accesses(root, rel, lionidx, NIL, lionam);
	foreach(lc, cands)
	{
		Path	   *cand = (Path *) lfirst(lc);
		List	   *rinfos = NIL;
		List	   *qual;
		Cost		cost;
		Selectivity sel;

		if (!lo_where_exact(cand, &rinfos, &qual) ||
			lo_residual(rel->baserestrictinfo, NIL, rinfos, qual) != NIL)
			continue;
		cost_bitmap_tree_node(cand, &cost, &sel);
		if (best == NULL || cost < bestcost)
		{
			best = cand;
			bestcost = cost;
			bestqual = qual;
		}
	}
	if (best == NULL)
		best = lo_where_and(root, rel, lionidx, lionam, &bestqual);
	if (best == NULL)
		return false;
	*lion = best;
	*lionqual = bestqual;
	return true;
}

/*
 * A lion access as custom_private carries it (lo_lion_tree()): its tree in
 * preorder, appended to *tree's; each leaf's index, appended to *leaves; and
 * each leaf's index quals, key on the left, appended to *quals.
 */
void
lion_ordered_lion_tree(Path *lion, List **tree, List **leaves, List **quals)
{
	lo_lion_tree(lion, tree, leaves, quals);
}

/*
 * The price of building a set of lion access `lion` of rel, as a nested
 * loop's probe builds it (§30.3, §40.10): its lookups as the build reads
 * them (lo_build_excess()), a cpu_operator_cost a container key, and the
 * copy of its pieces (lo_set_pieces()); its spill is the caller's, over all
 * of its sets together (lion_ordered_spill_price()).  *members and *bytes
 * are what the set is expected to hold and take (lo_set_bytes()).
 */
Cost
lion_ordered_set_price(PlannerInfo *root, RelOptInfo *rel, Path *lion,
					   double *members, double *bytes)
{
	double		tuples = Max(rel->tuples, 1.0);
	double		pages = Max((double) rel->pages, 1.0);
	Cost		lioncost;
	Selectivity sel;
	Selectivity candsel;
	List	   *rechecked;
	double		m;
	double		ncont;

	cost_bitmap_tree_node(lion, &lioncost, &sel);
	lioncost = Max(lioncost -
				   lo_build_excess(root, lion, tuples, pages) *
				   (cpu_index_tuple_cost + 0.1 * cpu_operator_cost),
				   0.0);
	candsel = sel;
	rechecked = lo_rechecked_quals(root, lion);
	if (rechecked != NIL)
	{
		Selectivity rsel = clauselist_selectivity(root, rechecked, rel->relid,
												  JOIN_INNER, NULL);

		if (rsel > 0)
			candsel = Min(sel / rsel, 1.0);
	}
	m = clamp_row_est(candsel * tuples);
	ncont = Min(ceil(pages / LION_BLOCKS_PER_CONTAINER), m);
	*members = m;
	*bytes = lo_set_bytes(m, ncont);
	return lioncost + ncont * cpu_operator_cost +
		lo_set_pieces(root, lion, m, ncont);
}

/*
 * Whether the set of lion access `lion` is exact as the planner sees it: it
 * leaves no qual to the recheck (lo_rechecked_quals()).
 */
bool
lion_ordered_set_exact(PlannerInfo *root, Path *lion)
{
	return lo_rechecked_quals(root, lion) == NIL;
}

/* What sets of `bytes` together cost past hash_mem (lo_spill_cost()). */
Cost
lion_ordered_spill_price(double bytes)
{
	return lo_spill_cost(bytes);
}

/*
 * The bytes a set of `members` TIDs of a relation of `pages` pages takes
 * (lo_set_bytes()), its containers as many as the members' blocks can fill
 * at most.
 */
double
lion_ordered_set_bytes(double members, double pages)
{
	double		m = clamp_row_est(members);

	return lo_set_bytes(m, Min(ceil(Max(pages, 1.0) /
									LION_BLOCKS_PER_CONTAINER), m));
}

/*
 * One lookup of a key in B-tree path probe's index, with no heap visit and
 * no set: the descent core prices a scan of the path at, and the entry the
 * key finds, read as the probe reads one (LO_PROBE_TUPLES).
 */
Cost
lion_ordered_lookup_price(IndexPath *probe)
{
	return probe->indextotalcost +
		LO_PROBE_TUPLES * cpu_tuple_cost + cpu_operator_cost;
}

/*
 * The price of one descent of B-tree idx of rel to an outer row's key, of
 * `loops` (§40.10's scan of the probe): core's IndexPath of idx
 * parameterized by relations within `outer` (lo_btree_paths()), whose
 * descent and leaf pages core has priced over the loops, each entry's probe
 * of the set of `lion` (or none), each member's visibility-map test, and the
 * heap pages of the members on pages not all-visible - with no row
 * returned, and no set: the build is lion_ordered_set_price()'s.  The
 * IndexPath goes into *probe, for the caller's cold reads.  False when core
 * makes no such path.
 */
bool
lion_ordered_probe_price(PlannerInfo *root, RelOptInfo *rel,
						 IndexOptInfo *idx, Relids outer, Path *lion,
						 List *lionqual, double loops, Cost *perprobe,
						 IndexPath **probe)
{
	List	   *params = NIL;
	IndexPath  *ord = NULL;
	List	   *lionrinfos = NIL;
	bool		lossy = false;
	Cost		startup;
	Cost		total;
	ListCell   *lc;

	(void) lo_btree_paths(root, rel, idx, &params);
	foreach(lc, params)
	{
		IndexPath  *ip = (IndexPath *) lfirst(lc);

		if (!bms_is_subset(PATH_REQ_OUTER(&ip->path), outer))
			continue;
		if (ord == NULL || ip->indextotalcost < ord->indextotalcost)
			ord = ip;
	}
	if (ord == NULL)
		return false;
	if (lion != NULL)
		(void) lo_lion_qual(lion, &lionrinfos, &lossy);
	(void) lo_cost(root, rel, ord, lion, lionrinfos, lionqual, NIL, 0.0,
				   true, Max(loops, 1.0), 1.0, ord->path.param_info != NULL,
				   NULL, &startup, &total, NULL, NULL);
	*perprobe = Max(total - startup, 0.0);
	*probe = ord;
	return true;
}

/*
 * The sets of a LionProbeJoin as it runs: a LionOrderedState lends them its
 * memory - charged against hash_mem, in the arena past it (§30.4,
 * "Spilling") - and its leaves; each tree a set, built by lo_build_node(),
 * and the extra sets the caller makes from TIDs it found itself.
 */
struct LionProbeSets
{
	LionOrderedState st;
	int			ntree;
	LoNode	  **trees;
	int			nset;			/* the trees', then the extra sets */
	LionTidSet **sets;			/* [nset]; NULL until built */
	bool	   *exact;			/* [nset] */
	bool		open;			/* the leaves' indexes are */
};

/*
 * The sets of `trees` - each one as lo_lion_tree() encodes it, their leaves'
 * indexes `leafoids` and index quals `quals` all of them in one list, in
 * order - and room for `nextra` more.  The quals' keys are any expression of
 * the key's type: lo_fix_qual() puts the index column in their place.
 * Without `open` (EXPLAIN alone) nothing is opened; with it the leaves'
 * indexes are opened and their scan keys built, run-time keys under ps.
 */
LionProbeSets *
lion_probe_sets_begin(PlanState *ps, List *trees, List *leafoids, List *quals,
					  int nextra, bool open)
{
	EState	   *estate = ps->state;
	MemoryContext oldcxt = MemoryContextSwitchTo(estate->es_query_cxt);
	LionProbeSets *sets = (LionProbeSets *) palloc0(sizeof(LionProbeSets));
	LionOrderedState *st = &sets->st;
	int			leafno = 0;
	int			qualno = 0;
	int			i = 0;
	ListCell   *lc;

	/* lo_spill_begin() finds the query's memory through it */
	st->css.ss.ps.state = estate;
	st->vmbuffer = InvalidBuffer;
	st->arenafile = -1;
	st->nleaves = list_length(leafoids);
	st->leaves = (LoLeaf *) palloc0(sizeof(LoLeaf) * Max(st->nleaves, 1));
	sets->ntree = list_length(trees);
	sets->trees = (LoNode **) palloc0(sizeof(LoNode *) * Max(sets->ntree, 1));
	foreach(lc, trees)
	{
		List	   *tree = (List *) lfirst(lc);
		ListCell   *pos = list_head(tree);

		if (tree == NIL)
			elog(ERROR, "LionProbeJoin: an empty lion tree");
		sets->trees[i++] = lo_decode_tree(st, tree, &pos, leafoids, quals,
										  &leafno, &qualno);
		if (pos != NULL)
			elog(ERROR, "LionProbeJoin: malformed lion tree");
	}
	if (leafno != st->nleaves || qualno != list_length(quals))
		elog(ERROR, "LionProbeJoin: malformed lion trees");
	if (nextra < 0)
		elog(ERROR, "LionProbeJoin: malformed extra sets");
	sets->nset = sets->ntree + nextra;
	sets->sets = (LionTidSet **) palloc0(sizeof(LionTidSet *) *
										 Max(sets->nset, 1));
	sets->exact = (bool *) palloc0(sizeof(bool) * Max(sets->nset, 1));

	if (open)
	{
		st->setcxt = AllocSetContextCreate(estate->es_query_cxt,
										   "LionProbeJoin sets",
										   ALLOCSET_DEFAULT_SIZES);
		st->buildcxt = AllocSetContextCreate(estate->es_query_cxt,
											 "LionProbeJoin build",
											 ALLOCSET_DEFAULT_SIZES);
		st->lrtcxt = CreateExprContext(estate);
		st->limit = get_hash_memory_limit();
		sets->open = true;

		/* what is read of each lion index is predicate-locked as it is (§9) */
		for (i = 0; i < st->nleaves; i++)
		{
			LoLeaf	   *leaf = &st->leaves[i];

			leaf->index = index_open(leaf->indexoid, AccessShareLock);
			ExecIndexBuildScanKeys(ps, leaf->index, leaf->quals, false,
								   &leaf->keys, &leaf->nkeys,
								   &leaf->rtkeys, &leaf->nrtkeys, NULL, NULL);
		}
	}
	MemoryContextSwitchTo(oldcxt);
	return sets;
}

/*
 * Build every tree's set for this scan's Params, the last build's sets and
 * the extra ones let go first; each tree's set is exact unless a source of
 * it said otherwise (lion_source_exact()).
 */
void
lion_probe_sets_build(LionProbeSets *sets)
{
	LionOrderedState *st = &sets->st;
	int			i;

	if (!sets->open)
		elog(ERROR, "LionProbeJoin: its sets were never opened");
	lo_set_reset(st);
	memset(sets->sets, 0, sizeof(LionTidSet *) * Max(sets->nset, 1));
	memset(sets->exact, 0, sizeof(bool) * Max(sets->nset, 1));
	lo_leaf_keys(st);
	for (i = 0; i < sets->ntree; i++)
	{
		st->exact = true;
		sets->sets[i] = lo_build_node(st, sets->trees[i]);
		sets->exact[i] = st->exact;
		MemoryContextReset(st->buildcxt);
	}
	st->builds++;
}

/*
 * Extra set `setno` from heap TIDs as lion_tid_to_code() codes them, in a
 * datum sort of int8 performed already, ascending, repeats allowed: a
 * container a key, made from its members' bits (lion_bits_to_container()).
 * Built after lion_probe_sets_build(), in the same memory.
 */
void
lion_probe_sets_put_codes(LionProbeSets *sets, int setno,
						  struct Tuplesortstate *codes)
{
	LionOrderedState *st = &sets->st;
	LionTidSet *set;
	uint64	   *img;
	LionContainer *out;
	uint32		cur = 0;
	bool		have = false;
	Datum		d;
	bool		isnull;

	if (setno < sets->ntree || setno >= sets->nset)
		elog(ERROR, "LionProbeJoin: no extra set %d", setno);
	set = lo_set_new(st);
	img = (uint64 *) MemoryContextAllocZero(st->buildcxt, LION_BITSET_BYTES);
	out = (LionContainer *) MemoryContextAlloc(st->buildcxt,
											   LION_CONTAINER_MAX_SIZE);
	while (tuplesort_getdatum(codes, true, false, &d, &isnull, NULL))
	{
		uint64		code;
		uint32		ckey;
		uint32		lo;

		CHECK_FOR_INTERRUPTS();
		if (isnull)
			continue;
		code = (uint64) DatumGetInt64(d);
		ckey = lion_code_ckey(code);
		if (have && ckey != cur)
		{
			if (ckey < cur)
				elog(ERROR, "LionProbeJoin: TID codes out of order");
			lion_bits_to_container(img, cur, out);
			lo_set_push(st, set, cur, out);
			memset(img, 0, LION_BITSET_BYTES);
		}
		cur = ckey;
		have = true;
		lo = lion_code_lo(code);
		img[lo >> 6] |= UINT64CONST(1) << (lo & 63);
	}
	if (have)
	{
		lion_bits_to_container(img, cur, out);
		lo_set_push(st, set, cur, out);
	}
	sets->sets[setno] = set;
	sets->exact[setno] = true;
	MemoryContextReset(st->buildcxt);
}

/* Is tid a member of set setno?  A set not built has none. */
bool
lion_probe_sets_has(LionProbeSets *sets, int setno, ItemPointer tid)
{
	Assert(setno >= 0 && setno < sets->nset);
	if (sets->sets[setno] == NULL)
		return false;
	return lo_set_find(sets->sets[setno], tid) >= 0;
}

/* Was set setno built exact, with nothing to recheck? */
bool
lion_probe_sets_exact(LionProbeSets *sets, int setno)
{
	Assert(setno >= 0 && setno < sets->nset);
	return sets->exact[setno];
}

/*
 * EXPLAIN ANALYZE: the last build's members and containers, over every set,
 * the bytes it spilled, and the builds.
 */
void
lion_probe_sets_counts(LionProbeSets *sets, uint64 *members, uint64 *ncont,
					   Size *spilled, uint64 *builds)
{
	LionOrderedState *st = &sets->st;
	int			i;

	*members = 0;
	*ncont = 0;
	for (i = 0; i < sets->nset; i++)
	{
		LionTidSet *set = sets->sets[i];
		int			k;

		if (set == NULL)
			continue;
		*ncont += (uint64) set->n;
		for (k = 0; k < set->n; k++)
			*members += set->conts[k]->cardinality;
	}
	*spilled = (st->spilled && st->arena != NULL) ?
		lion_arena_used(st->arena) : 0;
	*builds = st->builds;
}

/* The end: the leaves' indexes closed, the sets' memory and file gone. */
void
lion_probe_sets_end(LionProbeSets *sets)
{
	LionOrderedState *st;
	int			i;

	if (sets == NULL)
		return;
	st = &sets->st;
	for (i = 0; i < st->nleaves; i++)
	{
		if (st->leaves[i].index != NULL)
			index_close(st->leaves[i].index, AccessShareLock);
		st->leaves[i].index = NULL;
	}
	if (st->setcxt != NULL)
		MemoryContextDelete(st->setcxt);
	if (st->buildcxt != NULL)
		MemoryContextDelete(st->buildcxt);
	st->setcxt = NULL;
	st->buildcxt = NULL;
	memset(sets->sets, 0, sizeof(LionTidSet *) * Max(sets->nset, 1));
	if (st->arena != NULL)
	{
		/* the mappings first: the file is unlinked as it is closed */
		lion_arena_release(st->arena);
		FileClose(st->arenafile);
		st->arenafile = -1;
		st->arena = NULL;
	}
	sets->open = false;
}

/* ---------------------------------------------------------------------
 * Initialisation, from _PG_init (lion_am.c)
 * --------------------------------------------------------------------- */

void
lion_ordered_init(void)
{
	DefineCustomBoolVariable("pg_lion.enable_ordered_scan",
							 "Answer ORDER BY over an ordered index with a lion-filtered walk of it.",
							 NULL,
							 &lion_enable_ordered_scan,
							 true,
							 PGC_USERSET,
							 0,
							 NULL, NULL, NULL);

	/*
	 * DESIGN.md §30.4, "The set, lazily": off, LionOrdered builds its set
	 * before the walk, as it did before - for comparing the two, and for the
	 * tests of the build that pin it.
	 */
	DefineCustomBoolVariable("pg_lion.enable_lazy_set",
							 "Lets LionOrdered evaluate its lion set only at the container keys its walk meets.",
							 "Off, the set is built for the whole table before the walk starts.",
							 &lion_enable_lazy_set,
							 true,
							 PGC_USERSET,
							 0,
							 NULL, NULL, NULL);

	/*
	 * DESIGN.md §40.9: off, each participant of a parallel walk builds the
	 * set for itself, as it does when the set is too large to share - for
	 * comparing the two, and for the tests of that fallback, which a set of a
	 * regression test's size never reaches.
	 */
	DefineCustomBoolVariable("pg_lion.enable_shared_set",
							 "Lets the participants of a parallel LionOrdered or LionBtreeScan share one lion set.",
							 "Off, each participant builds the set for itself.",
							 &lion_enable_shared_set,
							 true,
							 PGC_USERSET,
							 0,
							 NULL, NULL, NULL);

	/*
	 * DESIGN.md §40: the walks of a covering btree offered without an ORDER
	 * BY.  pg_lion.enable_ordered_scan keeps gating the ordered ones.
	 */
	DefineCustomBoolVariable("pg_lion.enable_btree_scan",
							 "Offer LionBtreeScan: a lion-filtered walk of a covering B-tree that needs no ORDER BY.",
							 "The rows' values come from the index tuples; the heap is read only for pages not all-visible.",
							 &lion_enable_btree_scan,
							 true,
							 PGC_USERSET,
							 0,
							 NULL, NULL, NULL);

	/*
	 * DESIGN.md §30.4: what a member's heap fetch is worth in index entries
	 * walked, the fetch-and-sort switch's term.
	 */
	DefineCustomRealVariable("pg_lion.ordered_switch_ratio",
							 "Sets the index entries LionOrdered walks per member not yet met before it fetches and sorts the rest.",
							 "The measured price of a member's heap fetch in index entries walked; a large value keeps the walk from switching.",
							 &lion_ordered_switch_ratio,
							 LO_SWITCH_RATIO,
							 1.0, 1e6,
							 PGC_USERSET,
							 0,
							 NULL, NULL, NULL);

	RegisterCustomScanMethods(&lo_scan_methods);
	RegisterCustomScanMethods(&lo_btree_scan_methods);

	lion_prev_set_rel_pathlist_hook = set_rel_pathlist_hook;
	set_rel_pathlist_hook = lion_ordered_set_rel_pathlist;
}
