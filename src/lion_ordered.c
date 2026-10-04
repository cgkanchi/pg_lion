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
 *		Or there is no ordered side at all: when the ORDER BY's columns are
 *		stored in a lion index's window store and the query has a LIMIT, the
 *		node gathers their values for the rows the set keeps, window by
 *		window under the pin of DESIGN.md §9, keeps the best limit + offset
 *		of them in a bounded heap, and fetches only those, in order - the
 *		store order of DESIGN.md §40 ("The custom shapes", 2).
 *
 *		LionStoreScan, beside it, has no order at all: it returns every row
 *		the same set keeps, with every column the query reads gathered from
 *		the window stores of the relation's lion indexes - window by window,
 *		under the same pin - and reads the heap only for the rows the store
 *		cannot answer for (DESIGN.md §40, "As built: the row gather").  It
 *		shares the set's machinery, and this file, with LionOrdered.
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
#include "access/htup_details.h"
#include "access/relscan.h"
#include "access/table.h"
#include "access/tableam.h"
#if PG_VERSION_NUM >= 200000
#include "access/tableam_indexscan.h"
#endif
#include "access/stratnum.h"
#include "access/sysattr.h"
#include "access/visibilitymap.h"
#include "access/xact.h"
#include "catalog/pg_am.h"
#include "catalog/pg_class.h"
#include "commands/explain.h"
#include "common/hashfn.h"
#if PG_VERSION_NUM >= 180000
#include "commands/explain_format.h"
#endif
#include "executor/executor.h"
#include "executor/nodeIndexscan.h"
#include "lib/stringinfo.h"
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
#include "optimizer/prep.h"
#include "optimizer/restrictinfo.h"
#include "parser/parsetree.h"
#include "pgstat.h"
#include "storage/predicate.h"
#include "utils/builtins.h"
#include "utils/datum.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/ruleutils.h"
#include "utils/selfuncs.h"
#include "utils/snapmgr.h"
#include "utils/sortsupport.h"
#include "utils/spccache.h"
#include "utils/typcache.h"

#include "lion.h"
#include "lion_count.h"
#include "lion_costs.h"
#include "lion_customscan.h"
#include "lion_store.h"
#include "lion_wal.h"

/* GUCs, and the hook this file chains (all installed by lion_ordered_init). */
bool		lion_enable_ordered_scan = true;
bool		lion_enable_lazy_set = true;

/*
 * The store order (DESIGN.md §40): ORDER BY stored columns LIMIT n from the
 * window store.  Off, LionOrdered offers only its walks - for comparing the
 * two, and for the tests that show both plans of one query.
 */
static bool lion_enable_ordered_store = true;

/*
 * LionStoreScan (DESIGN.md §40, "As built: the row gather" and "As built: the
 * row gather, priced").  On by default: the gather is priced in the units of
 * the scan of core's it would displace and offered beside it.  Off, no path
 * of it is offered: the query gets core's scans and lion's own.
 */
static bool lion_enable_store_scan = true;
static set_rel_pathlist_hook_type lion_prev_set_rel_pathlist_hook = NULL;

/*
 * custom_private of the plan is POSITIONAL, behind a shape marker (as §10's):
 *
 *	LO_PRIV_SHAPE	IntList (LO_PRIV_MAGIC, LO_PRIV_NMEMBERS)
 *	LO_PRIV_ORD		OidList (the ordered index: a btree, or the lion index
 *					whose column is walked, LO_FLAG_LIONWALK, or the lion
 *					index whose store holds the ORDER BY's columns,
 *					LO_FLAG_STORE)
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
 *	LO_PRIV_STORE	the store order (LO_FLAG_STORE, DESIGN.md §40): IntList
 *					(the rows it keeps - limit + offset - and, for each sort
 *					key in LO_PRIV_SORT's order, the column of LO_PRIV_ORD's
 *					index whose store holds it, 1-based, then whether the key
 *					is descending); NIL for a walk
 *
 * and custom_exprs holds five lists:
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
 */
#define LO_PRIV_MAGIC		0x4c4f5244	/* "LORD" */
#define LO_PRIV_SHAPE		0
#define LO_PRIV_ORD			1
#define LO_PRIV_INTS		2
#define LO_PRIV_ORDCOLS		3
#define LO_PRIV_TREE		4
#define LO_PRIV_LEAVES		5
#define LO_PRIV_SORT		6
#define LO_PRIV_STORE		7
#define LO_PRIV_NMEMBERS	8

/*
 * The fetch-and-sort switch (DESIGN.md §30.4, "When the walk is not paying"):
 * once the walk of this scan has met LO_SWITCH_RATIO index entries per member
 * it has not met yet - and at least LO_SWITCH_MIN_WALK entries - the members
 * left are fetched and sorted instead.  The ratio is what one heap fetch of a
 * member costs in index entries walked, measured (§30.10: 0.04 us an entry,
 * 1-2 us a fetch, warm).
 */
#define LO_SWITCH_RATIO		32
#define LO_SWITCH_MIN_WALK	10000

/*
 * The set evaluated lazily (DESIGN.md §30.4, "The set, lazily"), until its
 * probes have cost what building it would: a probe of a stream is a unit of
 * work for each of its posting sets, starting a stream again behind where it
 * stands LO_LAZY_RESTART_WORK more for each, and the build reads each of the
 * sets' containers once - the budget, never below LO_LAZY_MIN_WORK.  A walk
 * that has met LO_LAZY_MAX_WALK entries is built for too, so that the early
 * stop and the fetch-and-sort switch, which need the set's size, can act on
 * it; as is one whose memo has taken half of hash_mem.
 */
#define LO_LAZY_MIN_WORK		64
#define LO_LAZY_RESTART_WORK	4
#define LO_LAZY_MAX_WALK		(4 * LO_SWITCH_MIN_WALK)

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

#define LO_EXPR_LIONQUALS	0
#define LO_EXPR_ORDQUALS	1
#define LO_EXPR_LIONQUAL	2
#define LO_EXPR_ORDORIG		3
#define LO_EXPR_VALUEQUAL	4
#define LO_EXPR_NLISTS		5

#define LO_NODE_LEAF		0
#define LO_NODE_AND			1
#define LO_NODE_OR			2

#define LO_FLAG_LOSSY		0x0001	/* a lion index clause was lossy */
#define LO_FLAG_LIONWALK	0x0002	/* the order is a lion column's walk */
#define LO_FLAG_STORE		0x0004	/* the order is the store's (§40) */

/*
 * LionStoreScan's custom_private (DESIGN.md §40, "As built: the row gather")
 * is positional too, behind a marker of its own - it is another node, with
 * another layout, not a shape of LionOrdered's:
 *
 *	LS_PRIV_SHAPE	IntList (LS_PRIV_MAGIC, LS_PRIV_NMEMBERS)
 *	LS_PRIV_INTS	IntList (flags): LO_FLAG_LOSSY, which no plan sets today -
 *					a lossy lion clause is declined - but which the executor
 *					honours as LionOrdered's does, every piece rechecked
 *	LS_PRIV_TREE	the lion tree, as LO_PRIV_TREE; an AND streams its first
 *					LEAF child, its DRIVER, and holds the others as a set
 *	LS_PRIV_LEAVES	OidList: each leaf's lion index, in preorder
 *	LS_PRIV_SOURCES	OidList: the lion indexes whose window stores the columns
 *					are gathered from, each once, in the order of the first
 *					column each gives
 *	LS_PRIV_ATTNOS	List of IntList, one per source: the table's columns it
 *					gives, ascending
 *	LS_PRIV_COLS	List of IntList, one per source: the index column, 1-based,
 *					that stores each of them, aligned with LS_PRIV_ATTNOS
 *
 * and custom_exprs holds two lists:
 *
 *	LS_EXPR_LIONQUALS	every leaf's index quals, as LO_EXPR_LIONQUALS
 *	LS_EXPR_LIONQUAL	the lion side's ORIGINAL qual (implicit AND), the
 *						recheck of a heap row of an inexact piece
 *
 * The plan's own targetlist and qual read only columns it gathers: the scan
 * tuple of a row the store gives holds those and NULL for every other column,
 * which ls_begin() checks the plan for.
 */
#define LS_PRIV_MAGIC		0x4c535343	/* "LSSC" */
#define LS_PRIV_SHAPE		0
#define LS_PRIV_INTS		1
#define LS_PRIV_TREE		2
#define LS_PRIV_LEAVES		3
#define LS_PRIV_SOURCES		4
#define LS_PRIV_ATTNOS		5
#define LS_PRIV_COLS		6
#define LS_PRIV_NMEMBERS	7

#define LS_EXPR_LIONQUALS	0
#define LS_EXPR_LIONQUAL	1
#define LS_EXPR_NLISTS		2

/* ---------------------------------------------------------------------
 * The TID set
 * --------------------------------------------------------------------- */

/*
 * A set of heap TIDs as containers (§3) sorted by container key.  A NULL
 * container means "every TID of this key's 64 heap blocks": what the set
 * degrades to when it outgrows hash_mem (DESIGN.md §30.4).
 */
typedef struct LionTidSet
{
	int			n;
	int			cap;
	int			nmerged;		/* entries the last lo_set_finish() left */
	uint32	   *keys;
	LionContainer **conts;
	bool		sorted;			/* keys strictly ascending */
} LionTidSet;

/* Bytes of directory charged per container key, on top of the container. */
#define LO_ENTRY_BYTES		((Size) (sizeof(uint32) + sizeof(LionContainer *) + 4))

/*
 * A directory of pieces (lo_set_push()) is merged when it is full only from
 * this size on: below it the pieces cost less than the merges would.
 */
#define LO_MERGE_MIN		1024

/* ---------------------------------------------------------------------
 * Executor state
 * --------------------------------------------------------------------- */

/*
 * One scan key of a leaf, for the lazy set: its posting sets, located once
 * and let go of (no pin is held between rows, §30.5), the tree over them
 * (lion_scankey_sets()), and a stream of that tree's containers which is only
 * ever sought forward - started again when a probe is behind it.
 */
typedef struct LoProbeKey
{
	int			nsets;
	LionPostingSet *sets;
	LionKeyNode *tree;			/* NULL: the key selects nothing */
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

/* A member fetched by the fetch-and-sort switch, with its sort keys. */
typedef struct LoSortRow
{
	HeapTuple	tup;
	Datum	   *vals;
	bool	   *nulls;
} LoSortRow;

/*
 * The TIDs the store order keeps (DESIGN.md §40), by their code: a row that
 * two pieces of the set both hold - two arms of an OR, two entries of a walk
 * - is ranked once.
 */
typedef struct LoTidEnt
{
	uint64		code;
	char		status;
} LoTidEnt;

#define SH_PREFIX		lo_tids
#define SH_ELEMENT_TYPE	LoTidEnt
#define SH_KEY_TYPE		uint64
#define SH_KEY			code
#define SH_HASH_KEY(tb, key)	murmurhash64(key)
#define SH_EQUAL(tb, a, b)		((a) == (b))
#define SH_SCOPE		static inline
#define SH_DECLARE
#define SH_DEFINE
#include "lib/simplehash.h"

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
	Size		bytes;
	Size		limit;
	bool		degraded;
	bool		exact;
	uint64		members;		/* members of the set, when not degraded */

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

	/*
	 * This scan's walk (DESIGN.md §30.4): which members it has met, one
	 * bitmap per container key touched, so that a member counts once however
	 * often the walk meets its TID; and the fetch-and-sort switch.
	 */
	MemoryContext scancxt;		/* reset per scan */
	uint64	  **visited;		/* [set->n], NULL until touched */
	Size		visitedbytes;
	bool		novisit;		/* over budget: no early stop, no switch */
	uint64		distinct;		/* members met in this scan */
	uint64		scanwalked;		/* entries walked in this scan */
	int			nsort;			/* sort keys; 0: the walk never switches */
	AttrNumber *sortattnos;
	SortSupport sortkeys;
	bool		noswitch;		/* the switch gave up for this scan */
	bool		sorting;		/* switched: returning srt[] */
	MemoryContext sortcxt;		/* srt[] and its tuples, under scancxt */
	struct LoSortRow *srt;
	int			nsrt;
	int			srtpos;

	/*
	 * ... or the store order (DESIGN.md §40): no walk at all.  The ORDER BY's
	 * columns are gathered from LO_PRIV_ORD's window store for the rows the
	 * set keeps, under the §9 pin of the piece of the set they came from, and
	 * the best storek of them kept: a max-heap of candidate slots, the worst
	 * on top, while the set is folded, and the slots in order afterwards.
	 */
	bool		store;
	int			storek;			/* rows to keep: limit + offset */
	AttrNumber *storeattnos;	/* [nsort] the store index's columns */
	int16	   *sortlen;		/* [nsort] the sort keys' typlen */
	bool	   *sortbyval;		/* [nsort] ... and typbyval */
	LionStoreReader **readers;	/* [nsort] while the set is folded */
	bool		storenovm;		/* no interlock: every row from the heap */
	bool		storebuilt;		/* the candidates are this execution's */
	MemoryContext topcxt;		/* the candidates and their values */
	int			topcap;
	int			ntop;
	ItemPointerData *toptid;	/* [topcap] */
	Datum	   *topvals;		/* [topcap * nsort] */
	bool	   *topnulls;		/* [topcap * nsort] */
	int		   *topheap;		/* [topcap] slots: a max-heap, then sorted */
	lo_tids_hash *toptids;		/* the TIDs of the slots */
	int			toppos;			/* the next slot to return */
	uint16	   *los;			/* [LION_CONTAINER_RANGE] a piece's members */
	uint16	   *avlos;			/* ... the ones on all-visible heap pages */
	Datum	  **gvals;			/* [nsort][LION_CONTAINER_RANGE] gathered */
	bool	  **gnulls;
	Datum	   *rowvals;		/* [nsort] one row's */
	bool	   *rownulls;
	MemoryContext rowcxt;		/* one heap row's values */
	Buffer		vmbuf;

	/*
	 * ... or LionStoreScan (DESIGN.md §40, "As built: the row gather"): no
	 * order, every row of the set returned, with the ngcols columns the plan
	 * reads gathered from the stores of nsrc lion indexes - gcols
	 * srcfirst[i] .. srcfirst[i + 1] - 1 from source i, into readers[],
	 * gvals[] and gnulls[] above - a piece of the set at a time while the
	 * page it came from is pinned (struct LsCursor), and every other row
	 * fetched into heapslot and copied into the virtual scan slot.
	 */
	bool		gscan;
	int			nsrc;
	Oid		   *srcoids;		/* [nsrc] */
	Relation   *srcidx;			/* [nsrc] */
	int		   *srcfirst;		/* [nsrc + 1] */
	int			ngcols;
	AttrNumber *gattnos;		/* [ngcols] the table's column */
	AttrNumber *gindexcols;		/* [ngcols] its source's index column */
	int			natts;			/* the table's columns */
	int			maxatt;			/* the most of a heap row the plan reads */
	TupleTableSlot *heapslot;
	struct LsCursor *cur;		/* the set, while this scan streams it */
	bool		gstarted;		/* this scan's cursor is open */
	bool		gdone;			/* ... and has handed out its last row */
	BlockNumber nblocks;		/* the heap's, when the scan started */
	uint16	   *pinlos;			/* [LION_CONTAINER_RANGE] scratch */

	/* the piece being returned: los[ppos .. pn - 1] are left */
	uint32		pckey;
	uint32		pn;
	uint32		ppos;
	uint32		pnav;			/* avlos[] gathered ... */
	uint32		pavpos;			/* ... the next of them */
	uint64		pserved;		/* heap pages whose rows the store gave */
	const LionContainer *pall;	/* every member of the piece */
	const LionContainer *psure; /* its certain ones: == pall, NULL, or some */

	/* EXPLAIN ANALYZE */
	uint64		gstorerows;		/* rows the store gave */
	uint64		gheaprows;		/* rows the heap gave */
	uint64		gabsent;		/* all-visible heap pages the store left */

	/* EXPLAIN ANALYZE */
	uint64		walked;
	uint64		hits;
	uint64		storevals;		/* rows whose values the store gave */
	uint64		ranked;			/* rows the store order ranked */
	uint64		fetched;
	uint64		removed;
	uint64		builds;
	int			ncont;
	uint64		scans;			/* walks started */
	uint64		switches;		/* scans that switched */
	uint64		sortfetched;	/* members fetched by the switch */
} LionOrderedState;

static Plan *lo_plan_path(PlannerInfo *root, RelOptInfo *rel,
						  CustomPath *best_path, List *tlist,
						  List *clauses, List *custom_plans);
static Node *lo_create_state(CustomScan *cscan);
static void lo_begin(CustomScanState *node, EState *estate, int eflags);
static TupleTableSlot *lo_exec(CustomScanState *node);
static void lo_end(CustomScanState *node);
static void lo_rescan(CustomScanState *node);
static void lo_scan_reset(LionOrderedState *st);
static void lo_explain(CustomScanState *node, List *ancestors,
					   ExplainState *es);
static List *lo_sort_keys(RelOptInfo *rel, List *pathkeys);

static const CustomPathMethods lo_path_methods = {
	.CustomName = "LionOrdered",
	.PlanCustomPath = lo_plan_path,
};

static const CustomScanMethods lo_scan_methods = {
	.CustomName = "LionOrdered",
	.CreateCustomScanState = lo_create_state,
};

static const CustomExecMethods lo_exec_methods = {
	.CustomName = "LionOrdered",
	.BeginCustomScan = lo_begin,
	.ExecCustomScan = lo_exec,
	.EndCustomScan = lo_end,
	.ReScanCustomScan = lo_rescan,
	.ExplainCustomScan = lo_explain,
};

/* LionStoreScan (DESIGN.md §40, "As built: the row gather") */
static Plan *ls_plan_path(PlannerInfo *root, RelOptInfo *rel,
						  CustomPath *best_path, List *tlist,
						  List *clauses, List *custom_plans);
static Node *ls_create_state(CustomScan *cscan);
static void ls_begin(CustomScanState *node, EState *estate, int eflags);
static TupleTableSlot *ls_exec(CustomScanState *node);
static void ls_end(CustomScanState *node);
static void ls_rescan(CustomScanState *node);
static void ls_explain(CustomScanState *node, List *ancestors,
					   ExplainState *es);

static const CustomPathMethods ls_path_methods = {
	.CustomName = "LionStoreScan",
	.PlanCustomPath = ls_plan_path,
};

static const CustomScanMethods ls_scan_methods = {
	.CustomName = "LionStoreScan",
	.CreateCustomScanState = ls_create_state,
};

static const CustomExecMethods ls_exec_methods = {
	.CustomName = "LionStoreScan",
	.BeginCustomScan = ls_begin,
	.ExecCustomScan = ls_exec,
	.EndCustomScan = ls_end,
	.ReScanCustomScan = ls_rescan,
	.ExplainCustomScan = ls_explain,
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
 * Which restriction clauses the node must still evaluate on a fetched row
 * (DESIGN.md §30.2, step 4): not the ones the ordered walk answers (the
 * ordered index's non-lossy index clauses, or the range a lion column's walk
 * is bounded by), not the lion leaves' index clauses, not what the lion qual
 * implies.
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
		if (list_member_ptr(ordrinfos, rinfo) ||
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
 * The price of one (ordered path, lion access) pair (DESIGN.md §30.3), and the
 * size its set is expected to have.  rows is what the node returns, as lion's
 * estimates see it (lion_probe_rel_rows()).
 */
static void
lo_cost(PlannerInfo *root, RelOptInfo *rel, IndexPath *ord, Path *lion,
		List *lionrinfos, List *lionqual, List *residual, double rows,
		Cost *startup_p, Cost *total_p, double *setbytes)
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
	double		spc_random;
	double		spc_seq;
	double		max_io;
	double		min_io;
	double		pages_corr;
	double		corr = 0.0;
	QualCost	qcost;
	Cost		startup;
	Cost		run;

	cost_bitmap_tree_node(lion, &lioncost, &sel);
	members = clamp_row_est(sel * tuples);
	ncont = Min(ceil(pages / LION_BLOCKS_PER_CONTAINER), members);
	*setbytes = ncont * (LION_CONTAINER_HDRSZ + LO_ENTRY_BYTES) +
		Min(members * sizeof(uint16), ncont * LION_BITSET_BYTES);

	/* the lookups, and the copy of their answer into the set */
	startup = lioncost + ncont * cpu_operator_cost;

	/* the walk: the ordered index's own cost, and one test per entry */
	walked = clamp_row_est(ord->indexselectivity * tuples);
	run = ord->indextotalcost + walked * cpu_operator_cost;

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

		if (list_member_ptr(lionrinfos, iclause->rinfo))
			shared = lappend(shared, iclause->rinfo);
	}
	foreach(lc, lo_pred_implied(ord->indexinfo, lionqual))
	{
		Expr	   *clause = (Expr *) lfirst(lc);
		ListCell   *lc2;
		bool		dup = false;

		foreach(lc2, shared)
			if (equal(lfirst_node(RestrictInfo, lc2)->clause, clause))
				dup = true;
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
	get_tablespace_page_costs(rel->reltablespace, &spc_random, &spc_seq);
	{
		Cost		s;
		Cost		t;
		Selectivity isel;
		double		ipages;

		ord->indexinfo->amcostestimate(root, ord, 1.0, &s, &t, &isel, &corr,
									   &ipages);
	}
	max_io = index_pages_fetched(fetched, rel->pages,
								 (double) ord->indexinfo->pages, root) *
		spc_random;
	pages_corr = ceil(Min(fetched, walked / tuples * pages));
	min_io = (pages_corr > 0) ? spc_random + (pages_corr - 1) * spc_seq : 0;
	run += max_io + corr * corr * (min_io - max_io);

	cost_qual_eval(&qcost, residual, root);
	startup += qcost.startup + rel->reltarget->cost.startup;
	run += fetched * (cpu_tuple_cost + qcost.per_tuple) +
		rows * rel->reltarget->cost.per_tuple;

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
	if (rte->relkind != RELKIND_RELATION && rte->relkind != RELKIND_MATVIEW)
		return false;
	if (rte->securityQuals != NIL)
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

#if PG_VERSION_NUM < 180000
/*
 * create_index_paths() on the scratch relation, on 16 and 17.  18 counts
 * disabled nodes: a disabled kind of scan loses to an enabled one, and among
 * paths all disabled the prices decide.  16 and 17 add disable_cost (1e10)
 * to a disabled scan's price instead: the first holds, the second does not -
 * every price is then within add_path()'s 1% fuzz of 1e10, and one path
 * survives - so with both kinds off, as a query that forces lion's own paths
 * sets them, an AND of two indexes lost to a scan of one with the other as
 * its filter, and the store order, whose set must answer the whole WHERE,
 * found no access that did (2026-10-02).  The candidates are then priced
 * with both kinds on, which is what 18 compares, and the settings put back
 * whatever happens.  Also adds the paths 18's matching builds for an OR of
 * equalities, as the IN list it spells (lion_or_list_paths(), DESIGN.md
 * §29.11): the lion side of `k = 1 OR k = 7` is then the lion side of
 * `k IN (1, 7)`.
 */
static void
lo_scratch_index_paths(PlannerInfo *root, RelOptInfo *scratch)
{
	bool		save_indexscan = enable_indexscan;
	bool		save_bitmapscan = enable_bitmapscan;

	if (!enable_indexscan && !enable_bitmapscan)
	{
		enable_indexscan = true;
		enable_bitmapscan = true;
	}
	PG_TRY();
	{
		create_index_paths(root, scratch);
		scratch->pathlist = list_concat(scratch->pathlist,
										lion_or_list_paths(root, scratch,
														   scratch->indexlist));
	}
	PG_FINALLY();
	{
		enable_indexscan = save_indexscan;
		enable_bitmapscan = save_bitmapscan;
	}
	PG_END_TRY();
}
#endif

/*
 * create_index_paths() on the scratch relation of lo_lion_accesses(),
 * without index-only scans: the copy's paths compete in add_path() as a
 * relation's do, and an index-only scan of one of the indexes - a lion index
 * storing the columns - dominated the bitmap heap path over the AND of two,
 * which was then no candidate.  The row gather took one leaf with the other
 * column as its filter, 75,000 rows gathered for 11,000 kept, in twice the
 * time the AND took (DESIGN.md §40, "As built: the row gather, priced").
 * Not considered, an index-only scan is built as the plain index scan it
 * would have been (check_index_only()).  Before 19 that is
 * enable_indexonlyscan, read as the paths are built; from 19 the setting is
 * folded into the relation's pgs_mask when planning starts, and the copy's
 * own mask loses PGS_CONSIDER_INDEXONLY instead - setting the GUC here
 * changes nothing there.
 */
static void
lo_scratch_create_paths(PlannerInfo *root, RelOptInfo *scratch)
{
	bool		save_indexonlyscan = enable_indexonlyscan;

#if PG_VERSION_NUM >= 190000
	scratch->pgs_mask &= ~PGS_CONSIDER_INDEXONLY;
#endif
	enable_indexonlyscan = false;
	PG_TRY();
	{
#if PG_VERSION_NUM >= 180000
		create_index_paths(root, scratch);
#else
		lo_scratch_index_paths(root, scratch);
#endif
	}
	PG_FINALLY();
	{
		enable_indexonlyscan = save_indexonlyscan;
	}
	PG_END_TRY();
}

/*
 * The lion accesses core builds for rel's restriction clauses less `exclude`:
 * create_index_paths() run on a scratch copy of rel that sees only its lion
 * indexes, no join clauses and no parallelism, so that only unparameterized,
 * non-partial paths land - in the copy's own path list (DESIGN.md §30.2).
 * Each lion index is copied too, with `exclude` taken out of the clauses it
 * may match, which is how a lion column's walk leaves the range it is bounded
 * by out of the set (§30.11).
 */
static List *
lo_lion_accesses(PlannerInfo *root, RelOptInfo *rel, List *lionidx,
				 List *exclude, Oid lionam)
{
	RelOptInfo *scratch;
	List	   *cands = NIL;
	ListCell   *lc;

	scratch = makeNode(RelOptInfo);
	memcpy(scratch, rel, sizeof(RelOptInfo));
	scratch->indexlist = NIL;
	foreach(lc, lionidx)
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
	scratch->joininfo = NIL;
	scratch->has_eclass_joins = false;
	scratch->consider_parallel = false;
	if (scratch->baserestrictinfo == NIL)
		return NIL;

	lo_scratch_create_paths(root, scratch);

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
/* ... and the store order's (DESIGN.md §40) */
#define LO_STORE_MAGIC		0x4c53544f	/* "LSTO" */

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
 * column"): those that read no column but the walked one and call nothing
 * volatile, on an index whose stored keys are the rows' own values byte for
 * byte (lion_index_can_emit_value()).  Every row of an entry then holds one
 * value, so a clause is true of all of them or of none, and the first
 * visible row the walk fetches of an entry decides for the entry: one that
 * fails takes the rest of the entry with it, unread.  The clause is only
 * ever evaluated on a visible row, as the ordinary plan evaluates it - never
 * on a stored key alone, which may be a dead row's, and could make a function
 * fail that the query never calls on it (`1 / k` with k = 0 only in a
 * deleted row).  It stays in the node's filter as well.
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
		if (contain_volatile_functions((Node *) rinfo->clause) ||
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
 * The walks of rel's lion columns that give the query's ORDER BY (DESIGN.md
 * §30.11): an ordered scalar key column of a lion index, in its type's own
 * btree order under the column's collation, whose pathkey - the ascending
 * one, as build_expression_pathkey() finds it among the query's equivalence
 * classes (for a partition, through its member of the parent's class) - is
 * the first of the ORDER BY's.  The walk then gives that pathkey whichever
 * direction and NULLs placement it asks for: it runs either way, and takes
 * the column's NULL entry before its values or after them.  Only the first
 * pathkey: a lion column orders one column, and core sorts the rest
 * incrementally.  Only the ORDER BY's: a GROUP BY, a DISTINCT or a merge
 * join over a lion column wants every row, which the count pushdown counts
 * from the same entries and a hash or a Sort takes from the heap in its
 * order, where the walk would fetch them in key order.
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
			AttrNumber	attno = idx->indexkeys[c];
			LionState  *state;
			TypeCacheEntry *typentry;
			Oid			type;
			int32		typmod;
			Oid			coll;
			Var		   *var;
			List	   *pks;
			PathKey    *mine;
			LoWalk	   *w;
			ListCell   *lc2;
			bool		strict = false;

			if (attno <= 0)
				continue;		/* an expression: no Var to order by */
			get_atttypetypmodcoll(rte->relid, attno, &type, &typmod, &coll);
			if (idx->indexcollations[c] != coll)
				continue;

			if (indexrel == NULL)
				indexrel = index_open(idx->indexoid, AccessShareLock);
			state = lion_index_column_state(indexrel, (AttrNumber) (c + 1));
			if (state->multikey || !state->ordered || state->typid != type)
				continue;

			/*
			 * The directory is in the type's default btree order only when
			 * the column's comparison is that order's (the GROUP BY walk's
			 * rule, lion_index_orders_naturally()); its `<` names the
			 * operator family the pathkey is in.
			 */
			typentry = lookup_type_cache(type, TYPECACHE_CMP_PROC |
										 TYPECACHE_LT_OPR);
			if (!OidIsValid(typentry->cmp_proc) ||
				typentry->cmp_proc != state->cmpproc.fn_oid ||
				!OidIsValid(typentry->lt_opr))
				continue;

			var = makeVar(rel->relid, attno, type, typmod, coll, 0);
			pks = build_expression_pathkey(root, (Expr *) var,
										   typentry->lt_opr, rel->relids,
										   false);
			if (pks == NIL)
				continue;
			mine = (PathKey *) linitial(pks);
			if (mine->pk_eclass != want->pk_eclass ||
				mine->pk_opfamily != want->pk_opfamily)
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
			 Cost *total_p, double *setbytes)
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
	*setbytes = 0.0;
	if (lion != NULL)
	{
		Cost		lioncost;
		double		members;
		double		ncont;

		List	   *shared;

		cost_bitmap_tree_node(lion, &lioncost, &sel);
		members = clamp_row_est(sel * tuples);
		ncont = Min(ceil(pages / LION_BLOCKS_PER_CONTAINER), members);
		*setbytes = ncont * (LION_CONTAINER_HDRSZ + LO_ENTRY_BYTES) +
			Min(members * sizeof(uint16), ncont * LION_BITSET_BYTES);
		startup += lioncost + ncont * cpu_operator_cost;

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
	corr = lion_var_heap_correlation(root, rel->relid, w->var);
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
 * The store order (DESIGN.md §40, "The custom shapes", 2): the rows a LIMIT
 * takes of an ORDER BY of stored columns, ranked from the window store.
 *
 * The node then returns only the rows it keeps, limit + offset of them, and
 * so may be offered only where nothing between it and the LIMIT can want a
 * row more: the relation is the whole query - or a partition, or a member of
 * a UNION ALL, of the relation that is, whose Append or MergeAppend the
 * ORDER BY and the LIMIT are above - and core's limit_tuples, which it
 * computes only for a query with no grouping, aggregate, window, DISTINCT or
 * set-returning target, says how many rows that is.  FOR UPDATE is
 * declined: LockRows sits between the scan and the LIMIT and can drop a row
 * a concurrent update moved, after which the LIMIT pulls one more; so is
 * WITH TIES, whose LIMIT takes the rows that tie with the last; and so is a
 * LIMIT or OFFSET that is not a constant (limit_tuples is then -1).  The
 * ORDER BY must be the path's whole order: an Incremental Sort above it
 * would need every row that ties with the last.  Returns limit + offset, or
 * 0 where the shape does not hold.
 */
static double
lo_store_limit(PlannerInfo *root, RelOptInfo *rel)
{
	Relids		top;

	if (root->sort_pathkeys == NIL || root->parse->rowMarks != NIL ||
		root->parse->limitOption == LIMIT_OPTION_WITH_TIES)
		return 0.0;
	if (root->limit_tuples < 1.0 || root->limit_tuples > (double) (INT_MAX / 2))
		return 0.0;
	top = (rel->reloptkind == RELOPT_OTHER_MEMBER_REL) ?
		rel->top_parent_relids : rel->relids;
	if (top == NULL || !bms_equal(top, root->all_baserels))
		return 0.0;
	return floor(root->limit_tuples);
}

/*
 * A lion index of rel whose window store holds every column the ORDER BY
 * sorts by (DESIGN.md §40): each a plain column of the table (sortkeys, from
 * lo_sort_keys()), stored as the column's own datum in the table's type -
 * LionStoreCol.returnable, never a multi-key column, which is never stored -
 * in an index that has a slot for every row the query can return: one that
 * is not partial, or whose predicate the query implies; of several, the one
 * whose store reads the fewest pages a window and column, and of those one
 * the lion side `lion` reads, if it reads one - any index's store will do
 * (lion_count_int.h: one pin is enough), but EXPLAIN reads more easily so.
 * Fills the store's columns, their directions, and the store pages a
 * window of them reads.
 */
typedef struct LoStore
{
	IndexOptInfo *index;
	List	   *attnos;			/* the index's column of each sort key */
	List	   *descs;			/* each sort key descending */
	int			nkeys;
	double		pagesper;		/* store pages a window reads, all the keys */
	double		width;			/* bytes a candidate keeps */
} LoStore;

/* What a candidate costs in memory besides its values (lo_store_fold()). */
#define LO_TOP_ENTRY_BYTES	((double) (sizeof(ItemPointerData) + 2 * sizeof(int) + 16))

/* Does the lion side read index? */
static bool
lo_path_reads(Path *path, Oid indexoid)
{
	List	   *arms;
	ListCell   *lc;

	if (path == NULL)
		return false;
	if (IsA(path, IndexPath))
		return ((IndexPath *) path)->indexinfo->indexoid == indexoid;
	if (IsA(path, BitmapAndPath))
		arms = ((BitmapAndPath *) path)->bitmapquals;
	else if (IsA(path, BitmapOrPath))
		arms = ((BitmapOrPath *) path)->bitmapquals;
	else
		return false;
	foreach(lc, arms)
		if (lo_path_reads((Path *) lfirst(lc), indexoid))
			return true;
	return false;
}

static bool
lo_store_find(PlannerInfo *root, RelOptInfo *rel, RangeTblEntry *rte,
			  List *lionidx, List *sortkeys, Path *lion, LoStore *st)
{
	List	   *hattnos = (List *) linitial(sortkeys);
	bool		found_any = false;
	bool		bestreads = false;
	ListCell   *lc;

	foreach(lc, lionidx)
	{
		IndexOptInfo *idx = (IndexOptInfo *) lfirst(lc);
		Relation	indexrel;
		LionIndexState *ix;
		List	   *attnos = NIL;
		List	   *ords = NIL;
		double		width = LO_TOP_ENTRY_BYTES;
		bool		ok = true;
		ListCell   *lc2;

		if (idx->indpred != NIL && !idx->predOK)
			continue;
		indexrel = index_open(idx->indexoid, AccessShareLock);
		ix = lion_get_index_state(indexrel);
		if (ix->nstored == 0)
		{
			index_close(indexrel, AccessShareLock);
			continue;
		}
		foreach(lc2, hattnos)
		{
			AttrNumber	hattno = (AttrNumber) lfirst_int(lc2);
			Oid			type = get_atttype(rte->relid, hattno);
			int			found = 0;
			int			c;

			for (c = 0; c < idx->ncolumns && found == 0; c++)
			{
				int			ord;

				if (idx->indexkeys[c] != hattno)
					continue;
				ord = lion_store_ordinal(ix, (AttrNumber) (c + 1));
				if (ord >= 0 && ix->stored[ord].returnable &&
					ix->stored[ord].typid == type)
				{
					const LionStoreCol *col = &ix->stored[ord];
					int32		w = 0;

					found = c + 1;
					if (col->typlen > 0)
						w = col->typlen;
					else if (hattno >= rel->min_attr && hattno <= rel->max_attr)
						w = rel->attr_widths[hattno - rel->min_attr];
					if (w <= 0)
						w = get_typavgwidth(type, -1);
					width += (double) w + sizeof(Datum) + sizeof(bool) +
						(col->typbyval ? 0.0 : 16.0);
				}
			}
			if (found == 0)
			{
				ok = false;
				break;
			}
			attnos = lappend_int(attnos, found);
			ords = lappend_int(ords, lion_store_ordinal(ix, (AttrNumber) found));
		}
		if (ok)
		{
			LionMetaStore ms;
			double		windows = Max(1.0, ceil((double) rel->pages /
												LION_BLOCKS_PER_CONTAINER));
			double		pagesper = 0.0;
			double		allw = 0.0;
			bool		reads = lo_path_reads(lion, idx->indexoid);
			int			i;

			/*
			 * The store's pages a window, shared among its columns by the
			 * width of their slots - a gather reads only its column's pages
			 * - and at least one a column read.
			 */
			lion_read_meta_store(indexrel, &ix->meta, &ms);
			for (i = 0; i < ix->nstored; i++)
				allw += (double) Max(ix->stored[i].rawwidth, 1);
			foreach(lc2, ords)
			{
				double		w = (double) Max(ix->stored[lfirst_int(lc2)].rawwidth, 1);

				pagesper += Max(1.0, (double) ms.store_pages * w / allw /
								windows);
			}
			if (found_any &&
				(pagesper > st->pagesper ||
				 (pagesper == st->pagesper && (bestreads || !reads))))
			{
				index_close(indexrel, AccessShareLock);
				continue;		/* another store is as good */
			}
			found_any = true;
			bestreads = reads;
			st->index = idx;
			st->attnos = attnos;
			st->nkeys = list_length(attnos);
			st->width = width;
			st->pagesper = pagesper;
			st->descs = NIL;
			foreach(lc2, root->sort_pathkeys)
			{
				PathKey    *pk = (PathKey *) lfirst(lc2);

#if PG_VERSION_NUM >= 180000
				st->descs = lappend_int(st->descs, pk->pk_cmptype == COMPARE_GT);
#else
				st->descs = lappend_int(st->descs,
										pk->pk_strategy == BTGreaterStrategyNumber);
#endif
			}
		}
		index_close(indexrel, AccessShareLock);
	}
	return found_any;
}

/*
 * The price of the store order with one lion access (DESIGN.md §40, "Costs"),
 * the rows its set keeps, and the size the set would have.  It is §30.3's
 * set without the walk:
 *
 *	- start-up: the lion lookups and a container's work per container key,
 *	  as §30.3; the gather - LION_STORE_VALUE_COST a value of each sort key
 *	  for the rows kept on all-visible heap pages, LION_STORE_PAGE_COST a
 *	  store page each window reads, the store's pages over the heap's
 *	  windows, shared among the stored columns by their slots' widths, at
 *	  least one a sort key (lo_store_find()); the rows on the other pages
 *	  fetched from the heap, as §9 prices a recheck
 *	  (a heap page's read, lion_heap_page_cost(), and LION_RECHECK_TID_COST a
 *	  row); and the ranking, as core prices a bounded sort, two operator
 *	  costs a comparison and log2(2 n) comparisons a row;
 *	- run: the n rows fetched, at random, as cost_index() prices them, and
 *	  the target per row.
 *
 * Nothing is converted (§39, "Not converted at all"): LionOrdered competes
 * with the relation's own scans, and lo_add_path() offers it at the margin
 * as it offers the walks.
 */
static void
lo_cost_store(PlannerInfo *root, RelOptInfo *rel, Path *lion, LoStore *s,
			  double k, Cost *startup_p, Cost *total_p, double *kept_p,
			  double *setbytes)
{
	double		tuples = Max(rel->tuples, 1.0);
	double		pages = Max((double) rel->pages, 1.0);
	double		allvis = Min(Max(rel->allvisfrac, 0.0), 1.0);
	Cost		lioncost;
	Selectivity sel;
	double		members;
	double		ncont;
	double		stored;
	double		dirty;
	double		fetched;
	double		spc_random;
	double		spc_seq;
	Cost		startup;
	Cost		run;

	cost_bitmap_tree_node(lion, &lioncost, &sel);
	members = clamp_row_est(sel * tuples);
	ncont = Min(ceil(pages / LION_BLOCKS_PER_CONTAINER), members);
	*setbytes = ncont * (LION_CONTAINER_HDRSZ + LO_ENTRY_BYTES) +
		Min(members * sizeof(uint16), ncont * LION_BITSET_BYTES);
	*kept_p = members;

	/* the lookups, and a container's work for each key of the answer */
	startup = lioncost + ncont * cpu_operator_cost;

	/* the gather, on the all-visible pages, and the heap on the others */
	stored = members * allvis;
	dirty = members - stored;
	startup += stored * s->nkeys * LION_STORE_VALUE_COST +
		ncont * s->pagesper * LION_STORE_PAGE_COST;
	if (dirty > 0.0)
	{
		double		dirtypages = Min(dirty, ceil(pages * (1.0 - allvis)));

		startup += dirtypages * lion_heap_page_cost(root, rel, dirtypages,
													pages) +
			dirty * LION_RECHECK_TID_COST;
	}

	/* the bounded heap of the n best, as cost_sort() prices one */
	startup += 2.0 * cpu_operator_cost * members * (log(2.0 * k) / log(2.0));
	startup += rel->reltarget->cost.startup;

	/* the n rows, fetched in their order, which is no order of the heap's */
	fetched = Min(k, members);
	get_tablespace_page_costs(rel->reltablespace, &spc_random, &spc_seq);
	run = index_pages_fetched(fetched, rel->pages, 0.0, root) * spc_random +
		fetched * (cpu_tuple_cost + rel->reltarget->cost.per_tuple);

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
 * One LionOrdered path - or a LionStoreScan one, by its methods - offered
 * to add_path() (DESIGN.md §30.2, step 3) at `margin`, pg_lion.pushdown_margin
 * or none (§39): its price divided by the margin, startup
 * and total alike, so that whatever it is compared with - the btree scan it
 * walks, a Sort over another path, a LIMIT's fraction of either - it is
 * taken only by that margin.  LionOrdered's competitors are the relation's
 * scans, which it is not converted into (lo_cost_store()): it has no rate.
 * LionStoreScan's price comes converted (ls_cost()), with the margin of its
 * competitor (lion_units_for()).
 */
static void
lo_add_path(RelOptInfo *rel, List *pathkeys, List *priv, double rows,
			Cost startup, Cost total, double margin,
			const CustomPathMethods *methods)
{
	CustomPath *cp = makeNode(CustomPath);

	cp->path.pathtype = T_CustomScan;
	cp->path.parent = rel;
	cp->path.pathtarget = rel->reltarget;
	cp->path.param_info = NULL;
	cp->path.parallel_aware = false;
	cp->path.parallel_safe = false;
	cp->path.parallel_workers = 0;
	cp->path.rows = rows;
	cp->path.startup_cost = startup / margin;
	cp->path.total_cost = total / margin;
	cp->path.pathkeys = pathkeys;
	cp->flags = CUSTOMPATH_SUPPORT_PROJECTION;
	cp->custom_paths = NIL;
	cp->custom_private = priv;
	cp->methods = methods;
	add_path(rel, &cp->path);
}

/* ---------------------------------------------------------------------
 * LionStoreScan's paths (DESIGN.md §40, "As built: the row gather")
 * --------------------------------------------------------------------- */

/*
 * The columns of rel the scan must return: every Var of its target, a
 * placeholder's contents included, each a plain column of the table - or
 * NULL when the node cannot be the scan: a whole-row or system column (a row
 * mark's ctid among them: the rows of an UPDATE, a DELETE, a FOR UPDATE and
 * an EvalPlanQual recheck are never the node's), or no column at all, which
 * leaves nothing to gather - a count is LionCount's.
 */
static Bitmapset *
ls_target_attnos(PlannerInfo *root, RelOptInfo *rel)
{
	Bitmapset  *attnos = NULL;
	List	   *vars;
	ListCell   *lc;

	if (get_plan_rowmark(root->rowMarks, rel->relid) != NULL)
		return NULL;
	vars = pull_var_clause((Node *) rel->reltarget->exprs,
						   PVC_RECURSE_PLACEHOLDERS);
	foreach(lc, vars)
	{
		Var		   *var = (Var *) lfirst(lc);

		if (!IsA(var, Var) || var->varno != (int) rel->relid ||
			var->varlevelsup != 0 || var->varattno <= 0)
			return NULL;
		attnos = bms_add_member(attnos, var->varattno);
	}
	list_free(vars);
	return attnos;
}

/* Add the columns clauses read to *attnos; false for a whole-row or system one. */
static bool
ls_clause_attnos(List *rinfos, Index relid, Bitmapset **attnos)
{
	Bitmapset  *cols = NULL;
	int			x = -1;

	pull_varattnos((Node *) extract_actual_clauses(rinfos, false), relid,
				   &cols);
	while ((x = bms_next_member(cols, x)) >= 0)
	{
		AttrNumber	attno = (AttrNumber) (x + FirstLowInvalidHeapAttributeNumber);

		if (attno <= 0)
			return false;
		*attnos = bms_add_member(*attnos, attno);
	}
	return true;
}

/*
 * A column of rel that a lion index's window store holds as the column's own
 * datum - LionStoreCol.returnable, in the table's type - and the store pages
 * one window of it is (ls_store_cols()).
 */
typedef struct LsStoreCol
{
	IndexOptInfo *index;
	AttrNumber	attno;			/* the table's column */
	int			indexcol;		/* the index column storing it, 1-based */
	double		pagesper;		/* store pages a window of it */
} LsStoreCol;

/* The bytes of a store page its sub-arrays can fill, about (lion_store_fmt.h). */
#define LS_STORE_PAGE_BYTES		((double) (BLCKSZ - 1024))

/*
 * The bytes one window of stored column col takes as the store writes it
 * (lion_store_choose_mode(), DESIGN.md §40, "DICT or RAW"): a dictionary of
 * the window's distinct values and a code a row, or a RAW slot a row where
 * the column has one and the dictionary would not pay.  rows is the window's
 * rows, ndistinct the column's values, of which that many rows drawn at
 * random meet ndistinct (1 - e^(-rows / ndistinct)); width a value's bytes.
 */
static double
ls_window_bytes(const LionStoreCol *col, double rows, double ndistinct,
				double width)
{
	double		d;
	double		dict = -1.0;
	double		raw = -1.0;
	int			codebits;

	ndistinct = Max(ndistinct, 1.0);
	d = Max(1.0, Min(rows, ndistinct * (1.0 - exp(-rows / ndistinct))));
	codebits = lion_store_dict_width((uint32) Min(d, (double) PG_UINT32_MAX));
	if (codebits > 0)
		dict = d * (width + (col->typlen > 0 ? 0.0 : 2.0)) +
			rows * (double) codebits / 8.0;
	if (col->rawwidth > 0)
		raw = rows * (double) col->rawwidth + rows / 8.0;
	if (dict < 0.0)
		return (raw < 0.0) ? rows * width : raw;
	if (raw < 0.0)
		return dict;
	return Min(dict, raw);
}

/* Column attno's distinct values, as the planner's statistics have them. */
static double
ls_ndistinct(PlannerInfo *root, RelOptInfo *rel, Oid relid, AttrNumber attno)
{
	VariableStatData vardata;
	Oid			type;
	int32		typmod;
	Oid			coll;
	bool		isdefault;
	double		nd;

	get_atttypetypmodcoll(relid, attno, &type, &typmod, &coll);
	examine_variable(root,
					 (Node *) makeVar(rel->relid, attno, type, typmod, coll, 0),
					 0, &vardata);
	nd = get_variable_numdistinct(&vardata, &isdefault);
	ReleaseVariableStats(vardata);
	return Max(nd, 1.0);
}

/*
 * Whether idx can serve the scan's rows at all - it is not partial, or the
 * query implies its predicate - and has a column, key or INCLUDE, among
 * attnos.  The catalog's word only, before any page of the index is read:
 * what the index stores, and how, takes its state and its meta page
 * (ls_store_cols()).
 */
static bool
ls_index_covers_any(IndexOptInfo *idx, Bitmapset *attnos)
{
	int			c;

	if (idx->indpred != NIL && !idx->predOK)
		return false;
	for (c = 0; c < idx->ncolumns; c++)
		if (idx->indexkeys[c] > 0 &&
			bms_is_member(idx->indexkeys[c], attnos))
			return true;
	return false;
}

/*
 * Every column of rel some lion index stores so that the scan can return it
 * (LsStoreCol): a plain column of the table, stored as its own datum in its
 * own type, by an index that has a slot for every row the query can return -
 * one that is not partial, or whose predicate the query implies.  Each with
 * the store pages a window of it is: the meta record counts the index's store
 * pages, not each column's, so they are shared among its stored columns by
 * what a window of each takes (ls_window_bytes()), and one at least - a
 * column's chain starts on a page of its own in every window.
 */
static List *
ls_store_cols(PlannerInfo *root, RelOptInfo *rel, RangeTblEntry *rte,
			  List *lionidx, Bitmapset *want)
{
	List	   *result = NIL;
	double		pages = Max((double) rel->pages, 1.0);
	double		windows = Max(1.0, ceil(pages / LION_BLOCKS_PER_CONTAINER));
	double		perwindow = Max(1.0, Max(rel->tuples, 1.0) / windows);
	ListCell   *lc;

	foreach(lc, lionidx)
	{
		IndexOptInfo *idx = (IndexOptInfo *) lfirst(lc);
		Relation	indexrel;
		LionIndexState *ix;
		LionMetaPageData meta;
		LionMetaStore ms;
		double	   *modeled;
		double		sum = 0.0;
		double		scale = 1.0;
		int			o;
		int			c;

		if (!ls_index_covers_any(idx, want))
			continue;			/* nothing the scan could take from it */
		indexrel = index_open(idx->indexoid, AccessShareLock);
		ix = lion_get_index_state(indexrel);
		if (ix->nstored == 0)
		{
			index_close(indexrel, AccessShareLock);
			continue;
		}

		/* the count is kept exact on the meta page, not in the cached state */
		lion_read_meta(indexrel, &meta);
		lion_read_meta_store(indexrel, &meta, &ms);
		modeled = (double *) palloc(sizeof(double) * ix->nstored);
		for (o = 0; o < ix->nstored; o++)
		{
			const LionStoreCol *col = &ix->stored[o];
			AttrNumber	hattno = (col->attno >= 1 && col->attno <= idx->ncolumns) ?
				idx->indexkeys[col->attno - 1] : 0;
			double		nd = DEFAULT_NUM_DISTINCT;
			double		width = (col->typlen > 0) ? (double) col->typlen : 32.0;

			if (hattno > 0)
			{
				nd = ls_ndistinct(root, rel, rte->relid, hattno);
				if (col->typlen <= 0 && hattno >= rel->min_attr &&
					hattno <= rel->max_attr &&
					rel->attr_widths[hattno - rel->min_attr] > 0)
					width = (double) rel->attr_widths[hattno - rel->min_attr];
			}
			modeled[o] = Max(1.0, ls_window_bytes(col, perwindow, nd, width) /
							 LS_STORE_PAGE_BYTES);
			sum += modeled[o];
		}
		if (ms.store_pages > 0 && sum > 0.0)
			scale = (double) ms.store_pages / windows / sum;

		for (c = 0; c < idx->ncolumns; c++)
		{
			AttrNumber	hattno = idx->indexkeys[c];
			LsStoreCol *sc;
			int			ord;

			if (hattno <= 0)
				continue;		/* an expression's value is not the column */
			ord = lion_store_ordinal(ix, (AttrNumber) (c + 1));
			if (ord < 0 || !ix->stored[ord].returnable ||
				ix->stored[ord].typid != get_atttype(rte->relid, hattno))
				continue;
			sc = (LsStoreCol *) palloc(sizeof(LsStoreCol));
			sc->index = idx;
			sc->attno = hattno;
			sc->indexcol = c + 1;
			sc->pagesper = Max(1.0, modeled[ord] * scale);
			result = lappend(result, sc);
		}
		pfree(modeled);
		index_close(indexrel, AccessShareLock);
	}
	return result;
}

/*
 * The store each column of attnos is gathered from: of the lion indexes that
 * store it (storecols), the one with the fewest pages a window of it, and on
 * a tie - pages within LS_PAGES_TIE of each other, which is all the estimate
 * can tell apart - one an earlier column is gathered from already, fewer
 * indexes being fewer chains to find, then one the lion side reads
 * (lion_count_int.h: one pin is enough, any index's store will do, but
 * EXPLAIN reads more easily so).  Returns the chosen LsStoreCols, in column
 * order, or NIL when a column is stored nowhere.
 */
#define LS_PAGES_TIE	0.10

static List *
ls_choose_sources(List *storecols, Bitmapset *attnos, Path *lion)
{
	List	   *chosen = NIL;
	List	   *used = NIL;
	int			x = -1;

	while ((x = bms_next_member(attnos, x)) >= 0)
	{
		LsStoreCol *best = NULL;
		bool		bestused = false;
		bool		bestreads = false;
		ListCell   *lc;

		foreach(lc, storecols)
		{
			LsStoreCol *sc = (LsStoreCol *) lfirst(lc);
			bool		isused;
			bool		reads;

			if (sc->attno != (AttrNumber) x)
				continue;
			isused = list_member_ptr(used, sc->index);
			reads = lo_path_reads(lion, sc->index->indexoid);
			if (best != NULL)
			{
				if (fabs(sc->pagesper - best->pagesper) >
					LS_PAGES_TIE * Max(sc->pagesper, best->pagesper))
				{
					if (sc->pagesper > best->pagesper)
						continue;
				}
				else if (isused != bestused)
				{
					if (!isused)
						continue;
				}
				else if (!reads || bestreads)
					continue;	/* no better: the first one stays */
			}
			best = sc;
			bestused = isused;
			bestreads = reads;
		}
		if (best == NULL)
			return NIL;
		chosen = lappend(chosen, best);
		used = list_append_unique_ptr(used, best->index);
	}
	return chosen;
}

/* The bytes a lion access's set takes, as §30.3 estimates them (lo_cost()). */
static double
ls_set_bytes(Path *path, double tuples, double pages)
{
	Cost		cost;
	Selectivity sel;
	double		members;
	double		ncont;

	cost_bitmap_tree_node(path, &cost, &sel);
	members = clamp_row_est(sel * tuples);
	ncont = Min(ceil(pages / LION_BLOCKS_PER_CONTAINER), members);
	return ncont * (LION_CONTAINER_HDRSZ + LO_ENTRY_BYTES) +
		Min(members * sizeof(uint16), ncont * LION_BITSET_BYTES);
}

/* Does every leaf of path answer its quals exactly (lion_plain_scan_passes())? */
static bool
ls_tree_exact(PlannerInfo *root, Path *path)
{
	List	   *arms;
	ListCell   *lc;

	if (IsA(path, IndexPath))
	{
		bool		sorted;
		bool		exact;

		(void) lion_plain_scan_passes(root, (IndexPath *) path, &sorted,
									  &exact);
		return exact;
	}
	arms = IsA(path, BitmapAndPath) ? ((BitmapAndPath *) path)->bitmapquals :
		castNode(BitmapOrPath, path)->bitmapquals;
	foreach(lc, arms)
		if (!ls_tree_exact(root, (Path *) lfirst(lc)))
			return false;
	return true;
}

/*
 * Can LionStoreScan stream lion access path, and how (DESIGN.md §40, "As
 * built: the row gather")?
 *
 *	- every leaf answers its quals exactly (lion_plain_scan_passes()): a row
 *	  the store gives is a row the set holds, and nothing rechecks it;
 *	- an AND streams its first LEAF child, its DRIVER, as lo_store_node()
 *	  does, and builds the others into one set as §30.4 builds one, which
 *	  masks the driver's pieces and has to fit in hash_mem with the others
 *	  (*maskbytes sums them): a set that degraded would leave every row of
 *	  its keys to the heap and the recheck;
 *	- an OR merges its arms' pieces by container key, a row two of them hold
 *	  returned once, so each arm must hand out ONE ascending run of keys - a
 *	  leaf of a SETS source, or an AND whose driver is one.  An arm that would
 *	  not, a range or a long list, could only be built into a set without its
 *	  pins, and every row of it read from the heap: such an OR is declined.
 *
 * *passes is how many runs over the heap's windows the stream makes - a
 * WALK's entries, a long list's batches - and *sorted whether it is one.
 */
static bool
ls_tree_ok(PlannerInfo *root, Path *path, double tuples, double pages,
		   double *passes, bool *sorted, double *maskbytes)
{
	ListCell   *lc;

	if (IsA(path, IndexPath))
	{
		bool		exact;

		*passes = lion_plain_scan_passes(root, (IndexPath *) path, sorted,
										 &exact);
		return exact;
	}
	if (IsA(path, BitmapAndPath))
	{
		List	   *arms = ((BitmapAndPath *) path)->bitmapquals;
		Path	   *driver = NULL;

		foreach(lc, arms)
		{
			if (IsA(lfirst(lc), IndexPath))
			{
				driver = (Path *) lfirst(lc);
				break;
			}
		}
		if (driver == NULL)
			driver = (Path *) linitial(arms);
		foreach(lc, arms)
		{
			Path	   *arm = (Path *) lfirst(lc);

			if (arm == driver)
				continue;
			if (!ls_tree_exact(root, arm))
				return false;
			*maskbytes += ls_set_bytes(arm, tuples, pages);
		}
		return ls_tree_ok(root, driver, tuples, pages, passes, sorted,
						  maskbytes);
	}
	if (IsA(path, BitmapOrPath))
	{
		foreach(lc, ((BitmapOrPath *) path)->bitmapquals)
		{
			double		p;
			bool		s;

			if (!ls_tree_ok(root, (Path *) lfirst(lc), tuples, pages, &p, &s,
							maskbytes) || !s)
				return false;
		}
		*passes = 1.0;
		*sorted = true;
		return true;
	}
	return false;
}

/*
 * What LionStoreScan's own work costs, in lion's units (DESIGN.md §40, "As
 * built: the row gather, priced"): fitted on the quick benchmark's
 * 5,000,000-row table (release build, warm), the node forced, one to five
 * columns gathered over results of 231 to 250,000 rows - a store page
 * 1.1 to 1.4 us read and decoded, a value 0.05 us beyond the page it is on,
 * a piece - one window's members, its visibility-map probes and each
 * column's gather started - 1.8 us, at the 500 units a millisecond lion's
 * constants are fitted at (§10, "The reference").  A member's tuple is
 * cpu_tuple_cost, which it measured at (0.016 us).
 *
 *	LS_PAGE_DECODE_COST	a store page's decoding, beyond LION_STORE_PAGE_COST
 *						and what reaching it costs (ls_store_page_cost());
 *	LS_VALUE_COST		a value handed up, beyond LION_STORE_VALUE_COST;
 *	LS_PIECE_COST		a piece;
 *	LS_COLUMN_START_COST	a column's reader opened, its index's store
 *						found: 12 us a column, measured on
 *						store_gather.sql's table of 30,000 rows, where a
 *						scan of no rows took 0.07 ms gathering one column
 *						and 0.11 ms gathering five, and a dozen rows' scan
 *						was a bitmap heap scan's 0.17 ms at 0.22 ms.
 *
 * pg_lion.store_page_cost and pg_lion.store_value_cost still move the
 * node's price, as they move the count's gather and the index-only scan's;
 * those two are not refitted here, and the count, priced against aggregates
 * that pay their own hashing, has shown no mispick.
 */
#define LS_PAGE_DECODE_COST		(112.0 * cpu_operator_cost)
#define LS_VALUE_COST			(9.0 * cpu_operator_cost)
#define LS_PIECE_COST			(240.0 * cpu_operator_cost)
#define LS_COLUMN_START_COST	(2400.0 * cpu_operator_cost)

/*
 * The store pages a gather reads of one column's chain in one window, for m
 * members spread over a chain of P pages (`pagesper`): it walks the chain
 * from its head to the page of its last member, 1 + (P - 1) m / (m + 1) of
 * them.  The one place the node's price says where a window's members lie
 * in its chain.
 */
static double
ls_window_chain_pages(double pagesper, double m)
{
	return 1.0 + (Max(pagesper, 1.0) - 1.0) * m / (m + 1.0);
}

/*
 * What a page of a window store costs LionStoreScan to reach and decode,
 * when `pages` of the `total` pages of one column's store are read: reached
 * as lion's own index pages are (lion_index_page_cost(), DESIGN.md §39,
 * "Resident index pages") - a buffer hit, LION_RESIDENT_PAGE_COST, for a
 * store the cache holds, and otherwise the device's price, as
 * cost_bitmap_heap_scan() prices a heap page: random_page_cost among few of
 * the column's pages, moving to seq_page_cost by the root of the share read,
 * the index's tablespace's costs - then LION_STORE_PAGE_COST and
 * LS_PAGE_DECODE_COST to decode it.
 *
 * It was the device's price alone until the node was converted into its
 * competitor's units (ls_cost()): a store page stood in for the heap page
 * core charges as I/O however cached, so that the node competed with the
 * relation's scans in their units, unconverted.  That made a page dear
 * against the node's rows, which were near free, and a result of a few
 * rows a window was priced by its pages and one of many rows a window at a
 * third of its time (DESIGN.md §40, "As built: the row gather", "Measured").
 */
static Cost
ls_store_page_cost(PlannerInfo *root, IndexOptInfo *index, double pages,
				   double total)
{
	double		spc_random;
	double		spc_seq;
	double		share;
	Cost		device;

	if (pages <= 0.0)
		return 0.0;
	get_tablespace_page_costs(index->reltablespace, &spc_random, &spc_seq);
	share = Min(pages / Max(total, 1.0), 1.0);
	device = spc_random - (spc_random - spc_seq) * sqrt(share);
	return lion_index_page_cost(root, Max((double) index->pages, total),
								device) +
		LION_STORE_PAGE_COST + LS_PAGE_DECODE_COST;
}

/*
 * The node's own reading of the lion side, in lion's units: what its stream
 * and its AND's mask cost the executor (ls_cursor_open(), lo_build_leaf()),
 * which is not what core's bitmap paths are priced for.  cost_bitmap_tree_node()
 * is the AM's bitmap scan, a TID a member handed to a TIDBitmap and, for an
 * AND, core's BitmapAnd of them: priced so, the mask of `c20 IN (3, 4, 5)`
 * (750,000 members) was 8,700 units, and the AND of it with `c200 IN (17, 18,
 * 19)` lost to the one leaf with the other as a filter, which gathered seven
 * times the rows in twice the time (DESIGN.md §40, "As built: the row gather,
 * priced").  The node reads the same sets a container at a time, and for
 * each clause of each leaf, fitted on the quick benchmark's table (release
 * build, warm), it costs:
 *
 *	- a container's work in each window the clause's members meet,
 *	  LS_CONTAINER_COST (0.6 us);
 *	- for a list of n sets - an IN list, the values of `= ANY` - a step of
 *	  each set at each window, LS_SET_STEP_COST (0.015 us), and, where the
 *	  list is streamed (the leaf the node reads, or an AND's driver) and not
 *	  built into a mask, their union under the pins: a container of each set
 *	  merged, LS_UNION_CONTAINER_COST (1 us), or where the containers hold a
 *	  member or two, LS_UNION_MEMBER_COST (0.15 us) for each member the leaf
 *	  keeps - beside another column's clause in the same leaf, only the
 *	  members both keep come out of the stream.  `c200 IN (17, 18)` read 3 ms
 *	  slower than `c200 = 17` from the same store pages, a second set's 1,500
 *	  containers, and `c20k IN` 400 values 20 ms for 100,000 members of
 *	  as many containers; where the list is masked, `c20 IN (3, 4, 5)` -
 *	  750,000 members - cost a few;
 *	- every member of the clause, cpu_operator_cost.
 *
 * A multi-key clause is one set, as the count prices it (§22).  The
 * selectivity is still cost_bitmap_tree_node()'s, as the AM's paths have it.
 */
#define LS_CONTAINER_COST		(120.0 * cpu_operator_cost)
#define LS_SET_STEP_COST		(3.0 * cpu_operator_cost)
#define LS_UNION_CONTAINER_COST	(200.0 * cpu_operator_cost)
#define LS_UNION_MEMBER_COST	(30.0 * cpu_operator_cost)

static Cost
ls_lion_cost(PlannerInfo *root, RelOptInfo *rel, Path *path, double windows,
			 bool streamed)
{
	double		tuples = Max(rel->tuples, 1.0);
	Cost		cost = 0.0;
	ListCell   *lc;

	if (IsA(path, IndexPath))
	{
		Cost		leafcost;
		Selectivity leafsel;
		double		leafmembers;

		/* a union streams only the members the whole leaf keeps */
		cost_bitmap_tree_node(path, &leafcost, &leafsel);
		leafmembers = clamp_row_est(leafsel * tuples);
		foreach(lc, ((IndexPath *) path)->indexclauses)
		{
			IndexClause *iclause = lfirst_node(IndexClause, lc);
			ListCell   *lc2;

			foreach(lc2, iclause->indexquals)
			{
				RestrictInfo *rinfo = lfirst_node(RestrictInfo, lc2);
				Node	   *cl = (Node *) rinfo->clause;
				Selectivity sel = clause_selectivity(root, (Node *) rinfo, 0,
													 JOIN_INNER, NULL);
				double		members = clamp_row_est(sel * tuples);
				double		touched = Max(1.0, Min(members,
												   windows * (1.0 - exp(-members / windows))));
				double		n = 1.0;

				if (IsA(cl, ScalarArrayOpExpr) &&
					((ScalarArrayOpExpr *) cl)->useOr)
#if PG_VERSION_NUM >= 170000
					n = Max(1.0, (double) estimate_array_length(root,
																(Node *) lsecond(((ScalarArrayOpExpr *) cl)->args)));
#else
					n = Max(1.0, (double) estimate_array_length((Node *) lsecond(((ScalarArrayOpExpr *) cl)->args)));
#endif
				cost += touched * LS_CONTAINER_COST;
				if (n > 1.0)
					cost += n * windows * LS_SET_STEP_COST;
				cost += members * cpu_operator_cost;
				if (n > 1.0 && streamed)
				{
					double		conts = n * Max(1.0, Min(windows, members / n));

					cost += Min(conts * LS_UNION_CONTAINER_COST,
								Min(members, leafmembers) * LS_UNION_MEMBER_COST);
				}
			}
		}
		return cost;
	}
	if (IsA(path, BitmapAndPath))
	{
		List	   *arms = ((BitmapAndPath *) path)->bitmapquals;
		Path	   *driver = NULL;

		/* the driver as ls_tree_ok() takes it: the first LEAF child */
		foreach(lc, arms)
		{
			if (IsA(lfirst(lc), IndexPath))
			{
				driver = (Path *) lfirst(lc);
				break;
			}
		}
		if (driver == NULL)
			driver = (Path *) linitial(arms);
		foreach(lc, arms)
			cost += ls_lion_cost(root, rel, (Path *) lfirst(lc), windows,
								 streamed && lfirst(lc) == driver);
		return cost;
	}
	foreach(lc, castNode(BitmapOrPath, path)->bitmapquals)
		cost += ls_lion_cost(root, rel, (Path *) lfirst(lc), windows,
							 streamed);
	return cost;
}

/*
 * The price of LionStoreScan over lion access `lion` (DESIGN.md §40, "As
 * built: the row gather, priced"), gathering the columns `chosen`
 * (LsStoreCol) and filtering by residual; rows is what it returns
 * (lion_probe_rel_rows()) and passes the runs its stream makes over the
 * heap's windows (ls_tree_ok()).  In lion's units:
 *
 *	- start-up: the lion side as the node reads it (ls_lion_cost()), the
 *	  AND's sets built among it, and each column's reader opened
 *	  (LS_COLUMN_START_COST);
 *	- a piece for each window each run meets a member in (Cardenas's count,
 *	  as the count's gather), LS_PIECE_COST;
 *	- for the members on all-visible pages - the share rel->allvisfrac says -
 *	  a value of each column (LION_STORE_VALUE_COST and LS_VALUE_COST), and
 *	  for each column the store pages each piece reads
 *	  (ls_window_chain_pages()), and before them the page of the store's map
 *	  that names the chain's head (lion_storemap_head()), read again by every
 *	  gather: each at ls_store_page_cost();
 *	- the members on the other pages fetched from the heap, as §9 prices a
 *	  recheck (a heap page's read, lion_heap_page_cost(), and
 *	  LION_RECHECK_TID_COST a row);
 *	- a tuple's work for every member, the filter's for each, and the
 *	  target's for every row returned.
 *
 * Then converted into the units of the relation's cheapest scan of core's,
 * as LionCount is into its aggregate's (DESIGN.md §39; u from
 * lion_units_for()): the rate of an index scan, an index-only scan, a bitmap
 * heap scan of one index or of an AND or OR of them, or a sequential scan.
 * Its competitors are those scans, which core prices with a cached heap page
 * charged as a read, so that they run at 1,000 to 3,000 units a millisecond
 * where the node, priced at lion's reference, runs at about 500: compared
 * unconverted, the node took results it is two to three times slower on.
 * The margin is lo_add_path()'s to apply, the competitor's (u->margin).
 */
static void
ls_cost(PlannerInfo *root, RelOptInfo *rel, Path *lion, List *chosen,
		List *residual, double rows, double passes, const LionUnits *u,
		Cost *startup_p, Cost *total_p)
{
	double		tuples = Max(rel->tuples, 1.0);
	double		pages = Max((double) rel->pages, 1.0);
	double		allvis = Min(Max(rel->allvisfrac, 0.0), 1.0);
	double		windows = Max(1.0, ceil(pages / LION_BLOCKS_PER_CONTAINER));
	double		runs;
	Cost		lioncost;
	Selectivity sel;
	double		members;
	double		pieces;
	double		stored;
	double		dirty;
	QualCost	qcost;
	Cost		startup;
	Cost		run = 0.0;
	ListCell   *lc;

	cost_bitmap_tree_node(lion, &lioncost, &sel);
	members = clamp_row_est(sel * tuples);
	runs = Max(passes, 1.0) * windows;

	startup = ls_lion_cost(root, rel, lion, windows, true) +
		list_length(chosen) * LS_COLUMN_START_COST;
	pieces = Max(1.0, Min(members, runs * (1.0 - exp(-members / runs))));
	run += pieces * LS_PIECE_COST;

	stored = members * allvis;
	dirty = members - stored;
	if (stored >= 1.0)
	{
		double		touched = Max(1.0, Min(stored,
										   runs * (1.0 - exp(-stored / runs))));
		double		m = stored / touched;

		foreach(lc, chosen)
		{
			LsStoreCol *sc = (LsStoreCol *) lfirst(lc);
			double		total = windows * sc->pagesper;
			double		reads = touched * ls_window_chain_pages(sc->pagesper, m);

			run += stored * (LION_STORE_VALUE_COST + LS_VALUE_COST);
			run += reads * ls_store_page_cost(root, sc->index, reads, total);
			/* the map, in the windows' order */
			run += touched * ls_store_page_cost(root, sc->index, touched,
												touched);
		}
	}
	if (dirty > 0.0)
	{
		double		dirtypages = Min(dirty, ceil(pages * (1.0 - allvis)));

		run += dirtypages * lion_heap_page_cost(root, rel, dirtypages, pages) +
			dirty * LION_RECHECK_TID_COST;
	}

	cost_qual_eval(&qcost, residual, root);
	startup += qcost.startup + rel->reltarget->cost.startup;
	run += members * (cpu_tuple_cost + qcost.per_tuple) +
		rows * rel->reltarget->cost.per_tuple;

	*startup_p = startup * u->rate;
	*total_p = (startup + run) * u->rate;
}

/*
 * LionStoreScan's paths (DESIGN.md §40, "As built: the row gather"): one per
 * lion access whose set answers the WHERE exactly but for a residual filter
 * on columns it gathers, when every column the scan returns - attnos, from
 * ls_target_attnos() - and every one that filter reads is stored by some
 * lion index of the relation (ls_choose_sources()), and the access can be
 * streamed (ls_tree_ok()).  The price decides against core's scans
 * (ls_cost()).
 */
static void
ls_add_paths(PlannerInfo *root, RelOptInfo *rel, RangeTblEntry *rte,
			 List *lionidx, List *cands, Bitmapset *attnos, double rows,
			 Size limit)
{
	double		tuples = Max(rel->tuples, 1.0);
	double		pages = Max((double) rel->pages, 1.0);
	List	   *storecols = NIL;
	Bitmapset  *want = bms_copy(attnos);
	bool		looked = false;
	bool		priced = false;
	LionUnits	units;
	ListCell   *lc;
	int			x = -1;

	/*
	 * Every column the scan returns must be a column of some lion index that
	 * serves its rows, or no source can be found for it whatever the access:
	 * decided from the catalog alone, so that a query returning a column no
	 * lion index has - most queries - reads no meta page for this node.  The
	 * stores looked at are those that have a column the scan could take, one
	 * it returns or one a restriction clause reads (a residual filter's).  A
	 * clause on a system column or the whole row is no lion clause, so it is
	 * every access's residual, which the scan cannot evaluate.
	 */
	while ((x = bms_next_member(attnos, x)) >= 0)
	{
		Bitmapset  *one = bms_make_singleton(x);
		bool		found = false;

		foreach(lc, lionidx)
		{
			if (ls_index_covers_any((IndexOptInfo *) lfirst(lc), one))
			{
				found = true;
				break;
			}
		}
		bms_free(one);
		if (!found)
			return;
	}
	if (!ls_clause_attnos(rel->baserestrictinfo, rel->relid, &want))
		return;

	foreach(lc, cands)
	{
		Path	   *lion = (Path *) lfirst(lc);
		List	   *lionrinfos = NIL;
		bool		lossy = false;
		List	   *lionqual = lo_lion_qual(lion, &lionrinfos, &lossy);
		List	   *residual;
		Bitmapset  *need;
		List	   *chosen;
		List	   *srcs = NIL;
		List	   *srcattnos = NIL;
		List	   *srccols = NIL;
		double		passes = 1.0;
		double		maskbytes = 0.0;
		bool		sorted;
		Cost		startup;
		Cost		total;
		ListCell   *lc2;

		if (lossy)
			continue;
		residual = lo_residual(rel->baserestrictinfo, NIL, lionrinfos,
							   lionqual);
		need = bms_copy(attnos);
		if (!ls_clause_attnos(residual, rel->relid, &need))
			continue;
		if (!ls_tree_ok(root, lion, tuples, pages, &passes, &sorted,
						&maskbytes) ||
			maskbytes > (double) limit)
			continue;
		if (!looked)
		{
			storecols = ls_store_cols(root, rel, rte, lionidx, want);
			looked = true;
		}
		chosen = ls_choose_sources(storecols, need, lion);
		if (chosen == NIL)
			continue;

		/* the sources in the order of their first column, columns ascending */
		foreach(lc2, chosen)
		{
			LsStoreCol *sc = (LsStoreCol *) lfirst(lc2);
			int			i = 0;
			ListCell   *lc3;

			foreach(lc3, srcs)
			{
				if (lfirst_oid(lc3) == sc->index->indexoid)
					break;
				i++;
			}
			if (i == list_length(srcs))
			{
				srcs = lappend_oid(srcs, sc->index->indexoid);
				srcattnos = lappend(srcattnos, NIL);
				srccols = lappend(srccols, NIL);
			}
			lfirst(list_nth_cell(srcattnos, i)) =
				lappend_int((List *) list_nth(srcattnos, i), sc->attno);
			lfirst(list_nth_cell(srccols, i)) =
				lappend_int((List *) list_nth(srccols, i), sc->indexcol);
		}

		if (!priced)
		{
			/*
			 * The units, found before the first path is added, which can
			 * free the scan of core's the next one would be priced against
			 * (lion_units_pin(), DESIGN.md §39).
			 */
			lion_units_for(rel, &units);
			priced = true;
		}
		ls_cost(root, rel, lion, chosen, residual, rows, passes, &units,
				&startup, &total);
		lo_add_path(rel, NIL, list_make4(lion, srcs, srcattnos, srccols),
					rows, startup, total, units.margin, &ls_path_methods);
	}
}

static void
lion_ordered_set_rel_pathlist(PlannerInfo *root, RelOptInfo *rel, Index rti,
							  RangeTblEntry *rte)
{
	Oid			lionam;
	List	   *ordpaths = NIL;
	List	   *lionidx = NIL;
	List	   *cands = NIL;
	List	   *walks;
	ListCell   *lc;
	ListCell   *lc2;
	Size		limit;
	double		rows;
	double		storek = 0.0;
	List	   *storesort = NIL;
	LoStore		store;
	Bitmapset  *gattnos = NULL;

	if (lion_prev_set_rel_pathlist_hook != NULL)
		lion_prev_set_rel_pathlist_hook(root, rel, rti, rte);

	if (!lion_enable_ordered_scan && !lion_enable_store_scan)
		return;
	/* no lion index is read under 16's old_snapshot_threshold (§9) */
	if (lion_old_snapshot_threshold_active())
		return;
	if (!lo_rel_ok(root, rel, rte))
		return;

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

	/*
	 * LionStoreScan (DESIGN.md §40, "As built: the row gather"): the columns
	 * it would return, when it can be the scan at all.
	 */
	if (lion_enable_store_scan && rel->baserestrictinfo != NIL)
		gattnos = ls_target_attnos(root, rel);

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
			 * pathkeys claim (DESIGN.md §30.4).
			 */
			if (ipath->indexorderbys != NIL ||
				ipath->indexinfo->relam != BTREE_AM_OID ||
				ipath->indexinfo->hypothetical)
				continue;
			if (!lo_indexpath_ok(ipath))
				continue;
			ordpaths = lappend(ordpaths, ipath);
		}
	}

	/* ... and the walks of lion's own ordered columns (§30.11) */
	walks = lion_enable_ordered_scan ?
		lo_lion_walks(root, rel, rte, lionidx) : NIL;

	/*
	 * ... or no walk at all: the ORDER BY's columns from a window store, for
	 * the rows a LIMIT takes (DESIGN.md §40), when the set answers the WHERE.
	 */
	memset(&store, 0, sizeof(store));
	if (lion_enable_ordered_scan && lion_enable_ordered_store &&
		rel->baserestrictinfo != NIL &&
		(storek = lo_store_limit(root, rel)) > 0.0)
	{
		storesort = lo_sort_keys(rel, root->sort_pathkeys);
		if (storesort == NIL ||
			!lo_store_find(root, rel, rte, lionidx, storesort, NULL, &store))
			storek = 0.0;
	}
	if (ordpaths == NIL && walks == NIL && storek == 0.0 && gattnos == NULL)
		return;

	/* the heap table AM only (§2); ambuild refused every other */
	{
		Relation	relation = table_open(rte->relid, NoLock);
		bool		supported = lion_table_am_supported(relation);

		table_close(relation, NoLock);
		if (!supported)
			return;
	}

	limit = get_hash_memory_limit();

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
	 * The lion side of a btree walk, of the store order and of LionStoreScan,
	 * as core would build it for every restriction clause.
	 */
	if (ordpaths != NIL || storek > 0.0 || gattnos != NULL)
		cands = lo_lion_accesses(root, rel, lionidx, NIL, lionam);

	if (gattnos != NULL)
		ls_add_paths(root, rel, rte, lionidx, cands, gattnos, rows, limit);

	foreach(lc, ordpaths)
	{
		IndexPath  *ord = (IndexPath *) lfirst(lc);
		List	   *ordrinfos = lo_ord_rinfos(ord);

		foreach(lc2, cands)
		{
			Path	   *lion = (Path *) lfirst(lc2);
			List	   *lionrinfos = NIL;
			bool		lossy = false;
			List	   *lionqual = lo_lion_qual(lion, &lionrinfos, &lossy);
			List	   *residual = lo_residual(rel->baserestrictinfo, ordrinfos,
											   lionrinfos, lionqual);
			Cost		startup;
			Cost		total;
			double		setbytes;

			lo_cost(root, rel, ord, lion, lionrinfos, lionqual, residual, rows,
					&startup, &total, &setbytes);
			if (setbytes > (double) limit)
				continue;		/* the set would not fit (§30.3) */
			lo_add_path(rel, ord->path.pathkeys, list_make2(ord, lion), rows,
						startup, total, lo_margin, &lo_path_methods);
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
			double		setbytes;
			List	   *priv;

			lo_cost_walk(root, rel, w, lion, lionqual, residual, rows,
						 &startup, &total, &setbytes);
			if (setbytes > (double) limit)
				continue;		/* the set would not fit (§30.3) */
			priv = list_make5(walkinfo, w->index, w->rinfos, w->quals,
							  list_make1(w->var));
			priv = lappend(priv, lion);
			priv = lappend(priv, w->vrinfos);
			lo_add_path(rel, list_make1(w->pathkey), priv, rows, startup,
						total, lo_margin, &lo_path_methods);
		}
	}

	/*
	 * The store order (DESIGN.md §40): one path per lion access whose set is
	 * the whole WHERE - exact, with nothing left for a filter, since a row
	 * the node ranks is a row it returns - and only while the rows it keeps
	 * are a small share of the set's, at most a quarter, and fit in
	 * work_mem; past that the walks and core's Sort are what the query
	 * gets.  Its order is the ORDER BY's, so that core puts no Sort above
	 * it, and its rows are the ones it keeps.
	 */
	if (storek > 0.0)
	{
		foreach(lc, cands)
		{
			Path	   *lion = (Path *) lfirst(lc);
			List	   *lionrinfos = NIL;
			bool		lossy = false;
			List	   *lionqual = lo_lion_qual(lion, &lionrinfos, &lossy);
			List	   *storeinfo;
			Cost		startup;
			Cost		total;
			double		setbytes;
			double		kept;

			if (lossy ||
				lo_residual(rel->baserestrictinfo, NIL, lionrinfos,
							lionqual) != NIL)
				continue;
			if (!lo_store_find(root, rel, rte, lionidx, storesort, lion,
							   &store))
				continue;		/* cannot happen: it was found above */
			storeinfo = list_make3_int(LO_STORE_MAGIC, (int) storek,
									   store.nkeys);
			storeinfo = list_concat(storeinfo, store.attnos);
			storeinfo = list_concat(storeinfo, store.descs);
			lo_cost_store(root, rel, lion, &store, storek, &startup, &total,
						  &kept, &setbytes);
			/* only an AND's other sides are ever held as sets */
			if (!IsA(lion, IndexPath) && setbytes > (double) limit)
				continue;		/* the set would not fit (§30.3) */
			if (storek > kept / 4.0 ||
				storek * store.width > (double) work_mem * 1024.0)
				continue;		/* too many rows to rank */
			lo_add_path(rel, root->sort_pathkeys,
						list_make3(storeinfo, store.index, lion),
						clamp_row_est(Min(storek, rows)), startup, total,
						lo_margin, &lo_path_methods);
		}
	}
}

/*
 * The path's pathkeys as sort keys over plain columns of rel, for the
 * fetch-and-sort switch; NIL when one of them is not a plain column (an
 * expression index's order: its functions would be evaluated where the
 * ordinary plan calls none, which EXECUTE could tell apart, §30.6).
 */
static List *
lo_sort_keys(RelOptInfo *rel, List *pathkeys)
{
	List	   *attnos = NIL;
	List	   *ops = NIL;
	List	   *colls = NIL;
	List	   *nulls = NIL;
	ListCell   *lc;

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
			return NIL;
#if PG_VERSION_NUM >= 180000
		desc = (pk->pk_cmptype == COMPARE_GT);
#else
		desc = (pk->pk_strategy == BTGreaterStrategyNumber);
#endif
		op = get_opfamily_member(pk->pk_opfamily, type, type,
								 desc ? BTGreaterStrategyNumber : BTLessStrategyNumber);
		if (!OidIsValid(op))
			return NIL;
		attnos = lappend_int(attnos, var->varattno);
		ops = lappend_oid(ops, op);
		colls = lappend_oid(colls, ec->ec_collation);
		nulls = lappend_int(nulls, pk->pk_nulls_first ? 1 : 0);
	}
	return list_make4(attnos, ops, colls, nulls);
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
	int			dir;
	int			flags = 0;
	int			nulls = LION_ORDER_NULLS_NONE;
	int			walkattno = 0;
	Oid			ordoid;
	List	   *store = NIL;
	ListCell   *lc;

	if (IsA(linitial(best_path->custom_private), IndexPath))
	{
		/* a btree's walk (DESIGN.md §30.2) */
		IndexPath  *ord = (IndexPath *) linitial(best_path->custom_private);

		lion = (Path *) lsecond(best_path->custom_private);
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
	}
	else if (linitial_int((List *) linitial(best_path->custom_private)) ==
			 LO_STORE_MAGIC)
	{
		/*
		 * The store order (DESIGN.md §40): the index whose store holds the
		 * ORDER BY's columns, the rows to keep and those columns; no walk,
		 * and so no quals of its own.
		 */
		List	   *storeinfo = (List *) linitial(best_path->custom_private);
		IndexOptInfo *idx = (IndexOptInfo *) lsecond(best_path->custom_private);

		lion = (Path *) lthird(best_path->custom_private);
		store = list_copy_tail(storeinfo, 1);
		ordoid = idx->indexoid;
		dir = (int) ForwardScanDirection;
		flags |= LO_FLAG_STORE;
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
	}

	if (lion != NULL)
	{
		lionqual = lo_lion_qual(lion, &lionrinfos, &lossy);
		lo_lion_tree(lion, &tree, &leaves, &lionquals);
		if (lossy)
			flags |= LO_FLAG_LOSSY;
	}
	residual = lo_residual(clauses, ordrinfos, lionrinfos, lionqual);

	/* a row the store order ranks is a row it returns: nothing filters it */
	if (store != NIL && (residual != NIL || lion == NULL || lossy))
		elog(ERROR, "LionOrdered: a store order with a filter");

	cscan->scan.plan.targetlist = tlist;
	cscan->scan.plan.qual = extract_actual_clauses(residual, false);
	cscan->scan.scanrelid = rel->relid;
	cscan->flags = best_path->flags;
	cscan->custom_plans = NIL;
	cscan->custom_scan_tlist = NIL;
	cscan->custom_exprs = list_make5(lionquals, ordquals, lionqual, ordorig,
									 extract_actual_clauses(vrinfos, false));
	cscan->custom_private =
		list_make5(list_make2_int(LO_PRIV_MAGIC, LO_PRIV_NMEMBERS),
				   list_make1_oid(ordoid),
				   list_make4_int(dir, flags, nulls, walkattno),
				   ordcols,
				   tree);
	cscan->custom_private = lappend(cscan->custom_private, leaves);
	cscan->custom_private = lappend(cscan->custom_private,
									lo_sort_keys(rel, best_path->path.pathkeys));
	cscan->custom_private = lappend(cscan->custom_private, store);
	cscan->methods = &lo_scan_methods;

	return &cscan->scan.plan;
}

/*
 * LionStoreScan's plan (DESIGN.md §40, "As built: the row gather"): the lion
 * tree as LionOrdered's, the stores and their columns, and the restriction
 * clauses the set does not answer as its filter - ls_add_paths() made sure
 * the columns they read are gathered.
 */
static Plan *
ls_plan_path(PlannerInfo *root, RelOptInfo *rel, CustomPath *best_path,
			 List *tlist, List *clauses, List *custom_plans)
{
	CustomScan *cscan = makeNode(CustomScan);
	Path	   *lion = (Path *) linitial(best_path->custom_private);
	List	   *lionrinfos = NIL;
	bool		lossy = false;
	List	   *lionqual;
	List	   *residual;
	List	   *tree = NIL;
	List	   *leaves = NIL;
	List	   *lionquals = NIL;

	lionqual = lo_lion_qual(lion, &lionrinfos, &lossy);
	lo_lion_tree(lion, &tree, &leaves, &lionquals);
	if (lossy || tree == NIL)
		elog(ERROR, "LionStoreScan: a lion access it cannot stream");
	residual = lo_residual(clauses, NIL, lionrinfos, lionqual);

	cscan->scan.plan.targetlist = tlist;
	cscan->scan.plan.qual = extract_actual_clauses(residual, false);
	cscan->scan.scanrelid = rel->relid;
	cscan->flags = best_path->flags;
	cscan->custom_plans = NIL;
	cscan->custom_scan_tlist = NIL;
	cscan->custom_exprs = list_make2(lionquals, lionqual);
	cscan->custom_private =
		list_make5(list_make2_int(LS_PRIV_MAGIC, LS_PRIV_NMEMBERS),
				   list_make1_int(0),
				   tree,
				   leaves,
				   lsecond(best_path->custom_private));
	cscan->custom_private = lappend(cscan->custom_private,
									lthird(best_path->custom_private));
	cscan->custom_private = lappend(cscan->custom_private,
									lfourth(best_path->custom_private));
	cscan->methods = &ls_scan_methods;

	return &cscan->scan.plan;
}

/* ---------------------------------------------------------------------
 * The TID set
 * --------------------------------------------------------------------- */

static LionTidSet *
lo_set_new(LionOrderedState *st)
{
	MemoryContext oldcxt = MemoryContextSwitchTo(st->setcxt);
	LionTidSet *set = (LionTidSet *) palloc0(sizeof(LionTidSet));

	set->cap = 16;
	set->keys = (uint32 *) palloc(sizeof(uint32) * set->cap);
	set->conts = (LionContainer **) palloc(sizeof(LionContainer *) * set->cap);
	set->sorted = true;
	st->live = lappend(st->live, set);
	MemoryContextSwitchTo(oldcxt);
	return set;
}

static void
lo_set_free(LionOrderedState *st, LionTidSet *set)
{
	int			i;

	for (i = 0; i < set->n; i++)
	{
		if (set->conts[i] != NULL)
		{
			st->bytes -= lion_container_size(set->conts[i]);
			pfree(set->conts[i]);
		}
	}
	st->bytes -= set->n * LO_ENTRY_BYTES;
	pfree(set->keys);
	pfree(set->conts);
	st->live = list_delete_ptr(st->live, set);
	pfree(set);
}

/*
 * The set outgrew hash_mem: keep the container KEYS of every live set and
 * drop their containers.  From here on every key means "all of its heap
 * blocks", and every fetched member is rechecked (DESIGN.md §30.4).
 */
static void
lo_degrade(LionOrderedState *st)
{
	ListCell   *lc;

	foreach(lc, st->live)
	{
		LionTidSet *set = (LionTidSet *) lfirst(lc);
		int			i;

		for (i = 0; i < set->n; i++)
		{
			if (set->conts[i] != NULL)
			{
				st->bytes -= lion_container_size(set->conts[i]);
				pfree(set->conts[i]);
				set->conts[i] = NULL;
			}
		}
	}
	st->degraded = true;
	st->exact = false;
}

static void lo_set_finish(LionOrderedState *st, LionTidSet *set);

/*
 * Append (ckey, a copy of c, or nothing when degraded).
 *
 * A WALK or a LIST source hands out a container key once per ENTRY or per
 * batch, not once (§29.3): a range over 400,000 distinct values brings
 * 400,000 one-TID pieces for 34 container keys.  The first version merged the
 * pieces only when the source was done, so every piece held a directory entry
 * and a container copy until then - the set degraded although its merged
 * form was a hundredth of hash_mem, and once degraded its directory went on
 * growing one entry per piece, past any bound (52 MB at a 1 MB hash_mem for a
 * range over 4M values; 2026-09-25 second review).  The pieces are merged
 * whenever the directory is full - it doubles only if it is still more than
 * half full after that - and before their bytes would degrade the set, once
 * there are at least as many pieces as merged entries (each merge then pays
 * for itself); only a set whose merged form does not fit degrades (§30.4).
 */
static void
lo_set_push(LionOrderedState *st, LionTidSet *set, uint32 ckey,
			const LionContainer *c)
{
	Size		size = (c != NULL && !st->degraded) ? lion_container_size(c) : 0;

	if (set->n == set->cap)
	{
		if (!set->sorted && set->cap >= LO_MERGE_MIN)
			lo_set_finish(st, set);
		if (set->n > set->cap / 2)
		{
			set->cap *= 2;
			set->keys = (uint32 *) repalloc(set->keys, sizeof(uint32) * set->cap);
			set->conts = (LionContainer **)
				repalloc(set->conts, sizeof(LionContainer *) * set->cap);
		}
	}
	if (size > 0 && !set->sorted &&
		st->bytes + LO_ENTRY_BYTES + size > st->limit &&
		set->n - set->nmerged >= set->nmerged)
		lo_set_finish(st, set);

	if (set->n > 0 && ckey <= set->keys[set->n - 1])
		set->sorted = false;
	st->bytes += LO_ENTRY_BYTES;
	set->keys[set->n] = ckey;
	set->conts[set->n] = NULL;
	if (c != NULL && !st->degraded)
	{
		if (st->bytes + size > st->limit)
			lo_degrade(st);
		else
		{
			LionContainer *copy = (LionContainer *)
				MemoryContextAlloc(st->setcxt, size);

			memcpy(copy, c, size);
			st->bytes += size;
			set->conts[set->n] = copy;
		}
	}
	set->n++;
}

static int
lo_cmp_slot(const void *a, const void *b, void *arg)
{
	const uint32 *keys = (const uint32 *) arg;
	uint32		ka = keys[*(const int *) a];
	uint32		kb = keys[*(const int *) b];

	if (ka != kb)
		return (ka < kb) ? -1 : 1;
	return (*(const int *) a < *(const int *) b) ? -1 : 1;
}

/*
 * A WALK or LIST source's containers came entry by entry (§29.3): sort them
 * by key and OR the containers of equal keys into one - when the source is
 * done, and whenever lo_set_push() finds the pieces piling up.  The pieces of
 * one key are ORed into a bitset image and turned into a container once:
 * ORing them pairwise cost a pass over the growing container per piece, 4.2 s
 * for a range over 250,000 distinct values against 0.2 s for the lion bitmap
 * scan of the same range (2026-09-25 second review).
 */
static void
lo_set_finish(LionOrderedState *st, LionTidSet *set)
{
	int		   *order;
	uint32	   *keys;
	LionContainer **conts;
	uint64	   *img;
	LionContainer *out;
	int			n = 0;
	int			i;

	if (set->sorted || set->n < 2)
	{
		set->sorted = true;
		set->nmerged = set->n;
		return;
	}

	order = (int *) MemoryContextAlloc(st->buildcxt, sizeof(int) * set->n);
	for (i = 0; i < set->n; i++)
		order[i] = i;
	qsort_arg(order, set->n, sizeof(int), lo_cmp_slot, set->keys);

	keys = (uint32 *) MemoryContextAlloc(st->setcxt, sizeof(uint32) * set->cap);
	conts = (LionContainer **)
		MemoryContextAlloc(st->setcxt, sizeof(LionContainer *) * set->cap);
	img = (uint64 *) MemoryContextAlloc(st->buildcxt, LION_BITSET_BYTES);
	out = (LionContainer *) MemoryContextAlloc(st->buildcxt,
											   LION_CONTAINER_MAX_SIZE);

	for (i = 0; i < set->n;)
	{
		uint32		key = set->keys[order[i]];
		int			j = i + 1;

		while (j < set->n && set->keys[order[j]] == key)
			j++;

		if (j == i + 1 || st->degraded)
		{
			/* one container (or only keys): moved as it is */
			conts[n] = set->conts[order[i]];
			set->conts[order[i]] = NULL;
		}
		else
		{
			int			m;
			Size		size;

			memset(img, 0, LION_BITSET_BYTES);
			for (m = i; m < j; m++)
				lion_container_or_into_bitset(set->conts[order[m]], img);
			lion_bits_to_container(img, key, out);
			size = lion_container_size(out);
			conts[n] = (LionContainer *) MemoryContextAlloc(st->setcxt, size);
			memcpy(conts[n], out, size);
			st->bytes += size;
		}
		keys[n] = key;
		n++;
		i = j;
	}

	/* what was merged away */
	for (i = 0; i < set->n; i++)
	{
		if (set->conts[i] != NULL)
		{
			st->bytes -= lion_container_size(set->conts[i]);
			pfree(set->conts[i]);
		}
	}
	st->bytes -= (set->n - n) * LO_ENTRY_BYTES;
	pfree(set->keys);
	pfree(set->conts);
	pfree(order);
	pfree(img);
	pfree(out);
	set->keys = keys;
	set->conts = conts;
	set->n = n;
	set->nmerged = n;
	set->sorted = true;

	if (!st->degraded && st->bytes > st->limit)
		lo_degrade(st);
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
		if (st->degraded || a->conts[i] == NULL || b->conts[j] == NULL)
		{
			/*
			 * Only a degraded set has keys without containers, and then the
			 * result keeps the key: "every TID of these blocks may be one".
			 */
			if (!isand || (a->conts[i] == NULL && b->conts[j] == NULL))
				lo_set_push(st, r, a->keys[i], NULL);
			else
				lo_set_push(st, r, a->keys[i],
							a->conts[i] != NULL ? a->conts[i] : b->conts[j]);
		}
		else if ((isand ? lion_container_and(a->conts[i], b->conts[j], buf) :
				  lion_container_or(a->conts[i], b->conts[j], buf)) > 0)
			lo_set_push(st, r, a->keys[i], buf);
		i++;
		j++;
	}
	pfree(buf);
	lo_set_free(st, a);
	lo_set_free(st, b);
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
			return (set->conts[mid] == NULL ||
					lion_container_contains(set->conts[mid],
											lion_code_lo(code))) ? mid : -1;
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

static LionTidSet *
lo_build_leaf(LionOrderedState *st, LoLeaf *leaf)
{
	LionTidSet *set = lo_set_new(st);
	LionSource *src;
	const LionContainer *c;

	/*
	 * LionStoreScan's mask trusts what a multi-key column's sets answer
	 * exactly (lion_source_open_ext()), as its streamed leaves do: a row of
	 * the store is checked against nothing.
	 */
	src = lion_source_open_ext(leaf->index, leaf->keys, leaf->nkeys, false,
							   st->gscan, st->buildcxt);
	pgstat_count_index_scan(leaf->index);
	if (!lion_source_exact(src))
		st->exact = false;
	while ((c = lion_source_next(src)) != NULL)
	{
		CHECK_FOR_INTERRUPTS();
		if (c->cardinality > 0)
			lo_set_push(st, set, c->ckey, c);
	}
	lion_source_close(src);
	lo_set_finish(st, set);
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

static void
lo_build_set(LionOrderedState *st)
{
	int			i;

	MemoryContextReset(st->setcxt);
	st->live = NIL;
	st->bytes = 0;
	st->degraded = false;
	st->exact = !st->lossyqual;
	st->limit = get_hash_memory_limit();

	lo_leaf_keys(st);
	st->set = lo_build_node(st, st->tree);
	MemoryContextReset(st->buildcxt);

	st->ncont = st->set->n;
	st->members = 0;
	for (i = 0; i < st->set->n; i++)
	{
		if (st->set->conts[i] != NULL)
			st->members += st->set->conts[i]->cardinality;
	}
	st->builds++;
}

/* ---------------------------------------------------------------------
 * The set, lazily (DESIGN.md §30.4, "The set, lazily")
 * --------------------------------------------------------------------- */

/*
 * Can every key of every leaf be sought in: a set tree - an equality or a
 * list - on a scalar column?  A range is a walk of entries, and a multi-key
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

		if (leaf->nkeys == 0)
			return false;
		for (k = 0; k < leaf->nkeys; k++)
		{
			ScanKey		sk = &leaf->keys[k];
			LionState  *col = lion_index_column_state(leaf->index,
													  sk->sk_attno);

			if (col->multikey || sk->sk_strategy != LION_STRAT_EQUAL ||
				(sk->sk_flags & (SK_SEARCHNULL | SK_SEARCHNOTNULL |
								 SK_ROW_HEADER)) != 0)
				return false;
		}
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
			if (node->leaf->pkeys[i].tree == NULL)
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
		for (i = 0; i < node->leaf->nkeys; i++)
		{
			LoProbeKey *pk = &node->leaf->pkeys[i];
			double		m = 0.0;

			if (pk->tree != NULL)
				for (s = 0; s < pk->nsets; s++)
					m += (double) pk->sets[s].ntids;
			n = (i == 0) ? m : Min(n, m);
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

		for (i = 0; i < leaf->nkeys; i++)
		{
			const LionContainer *c = lo_probe_at(st, &leaf->pkeys[i], ckey);
			LionContainer *r;

			if (c == NULL)
				return NULL;
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
 * The memo's entry for tid's container key if tid is a member, else NULL: a
 * key met for the first time is evaluated and remembered, members or not.
 */
static LoMemoEnt *
lo_lazy_find(LionOrderedState *st, ItemPointer tid)
{
	uint64		code = lion_tid_to_code(tid);
	uint32		ckey = lion_code_ckey(code);
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
 * the switch count those members as met.  A member the build does not have
 * cannot be - it is the same answer - but should one be missing, the scan
 * stops counting (novisit) rather than let the switch fetch it again.
 *
 * The lazy set goes first: the build evaluates the leaves' run-time keys
 * again, and the values the lazy sets were located from go with them.  What
 * the walk met lives in scancxt, and outlives it.
 */
static void
lo_lazy_convert(LionOrderedState *st)
{
	lo_memo_iterator it;
	LoMemoEnt  *e;
	uint32	   *mkeys;
	uint64	  **mvisited;
	int			nmet = 0;
	uint64	  **visited = NULL;
	bool		lost = false;
	int			i;

	mkeys = (uint32 *) MemoryContextAlloc(st->scancxt,
										  sizeof(uint32) * Max(st->memo->members, 1));
	mvisited = (uint64 **) MemoryContextAlloc(st->scancxt,
											  sizeof(uint64 *) * Max(st->memo->members, 1));
	lo_memo_start_iterate(st->memo, &it);
	while ((e = lo_memo_iterate(st->memo, &it)) != NULL)
	{
		if (e->visited == NULL)
			continue;
		mkeys[nmet] = e->ckey;
		mvisited[nmet++] = e->visited;
	}
	lo_lazy_end(st);

	lo_build_set(st);
	if (!st->degraded && st->set->n > 0)
		visited = (uint64 **) MemoryContextAllocZero(st->scancxt,
													 sizeof(uint64 *) * st->set->n);
	for (i = 0; i < nmet; i++)
	{
		int			idx = (visited != NULL) ?
			lo_set_key_index(st->set, mkeys[i]) : -1;

		if (idx < 0)
		{
			lost = true;
			continue;
		}
		visited[idx] = mvisited[i];
		st->visitedbytes += LION_BITSET_BYTES;
	}
	st->visited = visited;
	if (lost)
		st->novisit = true;
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

	st->css.methods = &lo_exec_methods;
	/* heap tuples, fetched through the table AM into a buffer slot */
	st->css.slotOps = &TTSOpsBufferHeapTuple;
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
	List	   *shape;
	List	   *ints;
	List	   *ordcols;
	List	   *tree;
	List	   *leafoids;
	List	   *lionquals;
	List	   *ordquals;
	List	   *fixed = NIL;
	ListCell   *pos;
	int			leafno = 0;
	int			qualno = 0;
	int			i;

	shape = (list_length(cscan->custom_private) == LO_PRIV_NMEMBERS) ?
		(List *) list_nth(cscan->custom_private, LO_PRIV_SHAPE) : NIL;
	if (shape == NIL || !IsA(shape, IntList) || list_length(shape) != 2 ||
		linitial_int(shape) != LO_PRIV_MAGIC ||
		lsecond_int(shape) != LO_PRIV_NMEMBERS ||
		list_length(cscan->custom_exprs) != LO_EXPR_NLISTS)
		elog(ERROR, "LionOrdered: unrecognized custom_private shape (%d members)",
			 list_length(cscan->custom_private));

	st->ordoid = linitial_oid((List *) list_nth(cscan->custom_private, LO_PRIV_ORD));
	ints = (List *) list_nth(cscan->custom_private, LO_PRIV_INTS);
	st->dir = (ScanDirection) linitial_int(ints);
	st->lossyqual = (lsecond_int(ints) & LO_FLAG_LOSSY) != 0;
	st->lionwalk = (lsecond_int(ints) & LO_FLAG_LIONWALK) != 0;
	st->store = (lsecond_int(ints) & LO_FLAG_STORE) != 0;
	st->walknulls = lthird_int(ints);
	st->walkattno = (AttrNumber) lfourth_int(ints);
	ordcols = (List *) list_nth(cscan->custom_private, LO_PRIV_ORDCOLS);
	tree = (List *) list_nth(cscan->custom_private, LO_PRIV_TREE);
	leafoids = (List *) list_nth(cscan->custom_private, LO_PRIV_LEAVES);
	lionquals = (List *) list_nth(cscan->custom_exprs, LO_EXPR_LIONQUALS);
	ordquals = (List *) list_nth(cscan->custom_exprs, LO_EXPR_ORDQUALS);

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
	if (st->noset && !st->lionwalk)
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

	/*
	 * The store order (DESIGN.md §40): the rows it keeps, and the store
	 * index's column of each sort key, which are LO_PRIV_SORT's.
	 */
	if (st->store)
	{
		List	   *sl = (List *) list_nth(cscan->custom_private, LO_PRIV_STORE);
		List	   *sk = (List *) list_nth(cscan->custom_private, LO_PRIV_SORT);
		int			n;

		if (sl == NIL || !IsA(sl, IntList) || list_length(sl) < 2 ||
			sk == NIL || st->noset || st->lionwalk)
			elog(ERROR, "LionOrdered: malformed store order");
		st->storek = linitial_int(sl);
		n = lsecond_int(sl);
		if (st->storek < 1 || n < 1 || list_length(sl) != 2 + 2 * n ||
			list_length((List *) linitial(sk)) != n)
			elog(ERROR, "LionOrdered: malformed store order");
		st->storeattnos = (AttrNumber *) palloc(sizeof(AttrNumber) * n);
		for (i = 0; i < n; i++)
			st->storeattnos[i] = (AttrNumber) list_nth_int(sl, 2 + i);
	}

	/* EXPLAIN without ANALYZE opens nothing (it names indexes by Oid). */
	if (eflags & EXEC_FLAG_EXPLAIN_ONLY)
		return;

	lion_check_table_am(node->ss.ss_currentRelation);
	if (!IsMVCCSnapshot(estate->es_snapshot))
		elog(ERROR, "LionOrdered: requires an MVCC snapshot");

	st->setcxt = AllocSetContextCreate(estate->es_query_cxt,
									   "LionOrdered set",
									   ALLOCSET_DEFAULT_SIZES);
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

	/* the fetch-and-sort switch's sort keys (§30.4), when there are any */
	{
		List	   *sk = (List *) list_nth(cscan->custom_private, LO_PRIV_SORT);

		if (sk != NIL)
		{
			List	   *attnos = (List *) linitial(sk);
			List	   *ops = (List *) lsecond(sk);
			List	   *colls = (List *) lthird(sk);
			List	   *nulls = (List *) lfourth(sk);

			st->nsort = list_length(attnos);
			st->sortattnos = (AttrNumber *) palloc(sizeof(AttrNumber) * st->nsort);
			st->sortkeys = (SortSupport) palloc0(sizeof(SortSupportData) * st->nsort);
			for (i = 0; i < st->nsort; i++)
			{
				SortSupport ssup = &st->sortkeys[i];

				st->sortattnos[i] = (AttrNumber) list_nth_int(attnos, i);
				ssup->ssup_cxt = CurrentMemoryContext;
				ssup->ssup_collation = list_nth_oid(colls, i);
				ssup->ssup_nulls_first = list_nth_int(nulls, i) != 0;
				ssup->ssup_attno = st->sortattnos[i];
				ssup->abbreviate = false;
				PrepareSortSupportFromOrderingOp(list_nth_oid(ops, i), ssup);
			}
		}
	}
	st->lrtcxt = CreateExprContext(estate);

	/* the ordered index and its scan keys */
	st->ordidx = index_open(st->ordoid, AccessShareLock);
	i = 0;
	foreach(pos, ordquals)
		fixed = lappend(fixed, lo_fix_qual((Expr *) lfirst(pos),
										   list_nth_int(ordcols, i++)));
	ExecIndexBuildScanKeys(&node->ss.ps, st->ordidx, fixed, false,
						   &st->okeys, &st->nokeys,
						   &st->ortkeys, &st->nortkeys, NULL, NULL);
	if (st->lionwalk || st->store)
	{
		/*
		 * The walk reads the lion index without index_beginscan(), as the
		 * set does: a relation predicate lock first (§30.5), and the heap
		 * fetch state of an index scan for what it meets.  So does the store
		 * order, whose rows are fetched by their TIDs as the walk's are.
		 */
		PredicateLockRelation(st->ordidx, estate->es_snapshot);
#if PG_VERSION_NUM >= 200000
#elif PG_VERSION_NUM >= 190000
		st->fetch = table_index_fetch_begin(node->ss.ss_currentRelation,
											SO_NONE);
#else
		st->fetch = table_index_fetch_begin(node->ss.ss_currentRelation);
#endif
	}

	/*
	 * Every lion index, and a relation predicate lock on each before any
	 * lookup (DESIGN.md §30.5; the AM has no ampredlocks, and the node reads
	 * it without index_beginscan()).
	 */
	for (i = 0; i < st->nleaves; i++)
	{
		LoLeaf	   *leaf = &st->leaves[i];

		leaf->index = index_open(leaf->indexoid, AccessShareLock);
		ExecIndexBuildScanKeys(&node->ss.ps, leaf->index, leaf->quals, false,
							   &leaf->keys, &leaf->nkeys,
							   &leaf->rtkeys, &leaf->nrtkeys, NULL, NULL);
		PredicateLockRelation(leaf->index, estate->es_snapshot);
	}

	/* which Params a rescan has to rebuild the set for */
	st->lionparams = pull_paramids((Expr *) lionquals);

	/*
	 * The store order's candidates, and what a fold needs (DESIGN.md §40).
	 * Its sort keys are LO_PRIV_SORT's, the relation's own columns; their
	 * values are copied as the table's tuple descriptor says.
	 */
	if (st->store)
	{
		TupleDesc	desc = RelationGetDescr(node->ss.ss_currentRelation);

		if (st->nsort < 1)
			elog(ERROR, "LionOrdered: a store order without sort keys");
		st->sortlen = (int16 *) palloc(sizeof(int16) * st->nsort);
		st->sortbyval = (bool *) palloc(sizeof(bool) * st->nsort);
		st->readers = (LionStoreReader **) palloc0(sizeof(LionStoreReader *) * st->nsort);
		st->gvals = (Datum **) palloc(sizeof(Datum *) * st->nsort);
		st->gnulls = (bool **) palloc(sizeof(bool *) * st->nsort);
		st->rowvals = (Datum *) palloc(sizeof(Datum) * st->nsort);
		st->rownulls = (bool *) palloc(sizeof(bool) * st->nsort);
		for (i = 0; i < st->nsort; i++)
		{
			Form_pg_attribute att = TupleDescAttr(desc, st->sortattnos[i] - 1);

			st->sortlen[i] = att->attlen;
			st->sortbyval[i] = att->attbyval;
			st->gvals[i] = (Datum *) palloc(sizeof(Datum) * LION_CONTAINER_RANGE);
			st->gnulls[i] = (bool *) palloc(sizeof(bool) * LION_CONTAINER_RANGE);
		}
		st->los = (uint16 *) palloc(sizeof(uint16) * LION_CONTAINER_RANGE);
		st->avlos = (uint16 *) palloc(sizeof(uint16) * LION_CONTAINER_RANGE);
		st->topcxt = AllocSetContextCreate(estate->es_query_cxt,
										   "LionOrdered store order",
										   ALLOCSET_DEFAULT_SIZES);
		st->rowcxt = AllocSetContextCreate(estate->es_query_cxt,
										   "LionOrdered store row",
										   ALLOCSET_SMALL_SIZES);
		st->vmbuf = InvalidBuffer;
	}

	/* the set evaluated lazily, when every leaf's keys can be sought (§30.4) */
	st->lazyok = lion_enable_lazy_set && !st->store && lo_lazy_ok(st);
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
		return lion_order_walk_next(st->owalk, &st->walktid) ?
			&st->walktid : NULL;
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
lo_fetch_tid(LionOrderedState *st, ItemPointer root, TupleTableSlot *slot)
{
	Snapshot	snapshot = st->css.ss.ps.state->es_snapshot;
	ItemPointerData tid = *root;
	bool		all_dead = false;
#if PG_VERSION_NUM >= 200000
	Relation	heap = st->css.ss.ss_currentRelation;

	if (!table_fetch_tid(heap, &tid, snapshot, &all_dead))
		return false;
	return table_tuple_fetch_row_version(heap, &tid, snapshot, slot);
#else
	bool		call_again = false;

	return table_index_fetch_tuple(st->fetch, &tid, snapshot, slot,
								   &call_again, &all_dead);
#endif
}

static bool
lo_fetch_walk(LionOrderedState *st, TupleTableSlot *slot)
{
	return lo_fetch_tid(st, &st->walktid, slot);
}

static bool
lo_fetch(LionOrderedState *st, TupleTableSlot *slot)
{
	return st->lionwalk ? lo_fetch_walk(st, slot) : lo_fetch_btree(st, slot);
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
								   st->nokeys, 0, SO_NONE);
#elif PG_VERSION_NUM >= 190000
		st->scan = index_beginscan(st->css.ss.ss_currentRelation, st->ordidx,
								   estate->es_snapshot, NULL,
								   st->nokeys, 0, SO_NONE);
#elif PG_VERSION_NUM >= 180000
		st->scan = index_beginscan(st->css.ss.ss_currentRelation, st->ordidx,
								   estate->es_snapshot, NULL, st->nokeys, 0);
#else
		st->scan = index_beginscan(st->css.ss.ss_currentRelation, st->ordidx,
								   estate->es_snapshot, st->nokeys, 0);
#endif
	}
	index_rescan(st->scan, st->okeys, st->nokeys, NULL, 0);
	st->scans++;
	st->started = true;
	st->done = false;
}

/* Forget this scan's walk: what it met, and a switch it made. */
static void
lo_scan_reset(LionOrderedState *st)
{
	/* what the lazy set's memo says this scan met goes with scancxt */
	if (st->memo != NULL)
	{
		lo_memo_iterator it;
		LoMemoEnt  *e;

		lo_memo_start_iterate(st->memo, &it);
		while ((e = lo_memo_iterate(st->memo, &it)) != NULL)
		{
			if (e->visited != NULL)
				st->memobytes -= LION_BITSET_BYTES;
			e->visited = NULL;
		}
	}
	if (st->scancxt != NULL)
		MemoryContextReset(st->scancxt);
	st->visited = NULL;
	st->visitedbytes = 0;
	st->novisit = false;
	st->distinct = 0;
	st->scanwalked = 0;
	st->noswitch = false;
	st->sorting = false;
	st->sortcxt = NULL;
	st->srt = NULL;
	st->nsrt = 0;
	st->srtpos = 0;
}

static inline bool
lo_visited(LionOrderedState *st, int idx, uint16 lo)
{
	return st->visited != NULL && st->visited[idx] != NULL &&
		(st->visited[idx][lo >> 6] & (UINT64CONST(1) << (lo & 63))) != 0;
}

/*
 * Note that this scan's walk met member tid (in container idx), and say
 * whether it is the first time (DESIGN.md §30.4, "Stopping early").  One
 * bitmap per container key the walk touches, within hash_mem; past that the
 * scan stops tracking - and with it no longer stops early or switches - and
 * every meeting counts as a first, which is harmless: a TID met twice is a
 * recycled slot, never visible to the snapshot.
 */
static bool
lo_mark(LionOrderedState *st, int idx, ItemPointer tid)
{
	uint16		lo = lion_code_lo(lion_tid_to_code(tid));

	if (st->novisit)
		return true;
	if (st->visited == NULL)
		st->visited = (uint64 **)
			MemoryContextAllocZero(st->scancxt, sizeof(uint64 *) * st->set->n);
	if (st->visited[idx] == NULL)
	{
		if (st->visitedbytes + LION_BITSET_BYTES > get_hash_memory_limit())
		{
			st->novisit = true;
			return true;
		}
		st->visited[idx] = (uint64 *)
			MemoryContextAllocZero(st->scancxt, LION_BITSET_BYTES);
		st->visitedbytes += LION_BITSET_BYTES;
	}
	if (lo_visited(st, idx, lo))
		return false;
	st->visited[idx][lo >> 6] |= UINT64CONST(1) << (lo & 63);
	return true;
}

static int
lo_cmp_rows(const void *a, const void *b, void *arg)
{
	const LoSortRow *ra = (const LoSortRow *) a;
	const LoSortRow *rb = (const LoSortRow *) b;
	LionOrderedState *st = (LionOrderedState *) arg;
	int			i;

	for (i = 0; i < st->nsort; i++)
	{
		int			c = ApplySortComparator(ra->vals[i], ra->nulls[i],
											rb->vals[i], rb->nulls[i],
											&st->sortkeys[i]);

		if (c != 0)
			return c;
	}
	return 0;
}

/*
 * The fetch-and-sort switch (DESIGN.md §30.4, "When the walk is not paying"):
 * fetch every member this scan's walk has not met, keep the visible ones that
 * pass the lion recheck (for an inexact set) and the ordered index's own
 * clauses (they did not come through the index), and sort them by the
 * pathkeys.  Every row the walk has not returned yet is among them - a
 * visible member's index entry lies where the walk has not been - and none
 * it has returned is.  Gives up, and lets the walk go on, if the rows do not
 * fit in work_mem.
 */
static bool
lo_switch(LionOrderedState *st)
{
	Relation	heap = st->css.ss.ss_currentRelation;
	TupleDesc	desc = RelationGetDescr(heap);
	Snapshot	snapshot = st->css.ss.ps.state->es_snapshot;
	TupleTableSlot *slot = st->css.ss.ss_ScanTupleSlot;
	ExprContext *econtext = st->css.ss.ps.ps_ExprContext;
	Size		budget = (Size) work_mem * 1024;
	Size		used = 0;
	int			cap = 64;
	uint16	   *los;
	MemoryContext oldcxt;
	int			i;

	st->sortcxt = AllocSetContextCreate(st->scancxt, "LionOrdered sort",
										ALLOCSET_DEFAULT_SIZES);
	oldcxt = MemoryContextSwitchTo(st->sortcxt);
	los = (uint16 *) palloc(sizeof(uint16) * LION_CONTAINER_RANGE);
	st->srt = (LoSortRow *) palloc(sizeof(LoSortRow) * cap);
	st->nsrt = 0;

	for (i = 0; i < st->set->n; i++)
	{
		uint32		n = lion_container_to_array(st->set->conts[i], los);
		uint32		j;

		for (j = 0; j < n; j++)
		{
			ItemPointerData tid;
			bool		all_dead = false;
			LoSortRow  *row;
			int			k;

			if (lo_visited(st, i, los[j]))
				continue;
			CHECK_FOR_INTERRUPTS();
			lion_code_to_tid(lion_make_code(st->set->keys[i], los[j]), &tid);
			st->sortfetched++;
			if (!lion_table_fetch_tid(heap, &tid, snapshot, &all_dead) ||
				!table_tuple_fetch_row_version(heap, &tid, snapshot, slot))
				continue;
			st->fetched++;
			ResetExprContext(econtext);
			econtext->ecxt_scantuple = slot;
			if (!st->exact && !ExecQual(st->lionrecheck, econtext))
			{
				st->removed++;
				continue;
			}
			if (!ExecQual(st->ordrecheck, econtext))
				continue;

			if (st->nsrt == cap)
			{
				cap *= 2;
				st->srt = (LoSortRow *) repalloc(st->srt, sizeof(LoSortRow) * cap);
			}
			row = &st->srt[st->nsrt++];
			row->tup = ExecCopySlotHeapTuple(slot);
			row->vals = (Datum *) palloc(sizeof(Datum) * st->nsort);
			row->nulls = (bool *) palloc(sizeof(bool) * st->nsort);
			for (k = 0; k < st->nsort; k++)
				row->vals[k] = heap_getattr(row->tup, st->sortattnos[k], desc,
											&row->nulls[k]);
			used += HEAPTUPLESIZE + row->tup->t_len +
				st->nsort * (sizeof(Datum) + sizeof(bool)) + sizeof(LoSortRow);
			if (used > budget)
			{
				/* too big to sort here: the walk goes on (§30.4) */
				MemoryContextSwitchTo(oldcxt);
				MemoryContextDelete(st->sortcxt);
				st->sortcxt = NULL;
				st->srt = NULL;
				st->nsrt = 0;
				st->noswitch = true;
				ExecClearTuple(slot);
				return false;
			}
		}
	}
	MemoryContextSwitchTo(oldcxt);
	ExecClearTuple(slot);

	qsort_arg(st->srt, st->nsrt, sizeof(LoSortRow), lo_cmp_rows, st);
	st->sorting = true;
	st->srtpos = 0;
	st->switches++;
	return true;
}

static TupleTableSlot *
lo_sort_next(LionOrderedState *st, TupleTableSlot *slot)
{
	HeapTuple	tup;

	if (st->srtpos >= st->nsrt)
	{
		st->done = true;
		return ExecClearTuple(slot);
	}
	tup = st->srt[st->srtpos++].tup;
	ExecForceStoreHeapTuple(tup, slot, false);
	slot->tts_tid = tup->t_self;
	slot->tts_tableOid = RelationGetRelid(st->css.ss.ss_currentRelation);
	return slot;
}

/* ---------------------------------------------------------------------
 * The store order (DESIGN.md §40, "The custom shapes", 2)
 * --------------------------------------------------------------------- */

#define LO_TOPV(st, s)	(&(st)->topvals[(Size) (s) * (st)->nsort])
#define LO_TOPN(st, s)	(&(st)->topnulls[(Size) (s) * (st)->nsort])

/*
 * Order two rows' keys as the ORDER BY does: the pathkeys' own sort
 * operators, collations and NULLS placement, through SortSupport, as core's
 * Sort compares them (lo_cmp_rows()).
 */
static inline int
lo_top_cmp(LionOrderedState *st, const Datum *va, const bool *na,
		   const Datum *vb, const bool *nb)
{
	int			i;

	for (i = 0; i < st->nsort; i++)
	{
		int			c = ApplySortComparator(va[i], na[i], vb[i], nb[i],
											&st->sortkeys[i]);

		if (c != 0)
			return c;
	}
	return 0;
}

static inline int
lo_top_cmp_slots(LionOrderedState *st, int a, int b)
{
	return lo_top_cmp(st, LO_TOPV(st, a), LO_TOPN(st, a),
					  LO_TOPV(st, b), LO_TOPN(st, b));
}

static int
lo_top_qcmp(const void *a, const void *b, void *arg)
{
	return lo_top_cmp_slots((LionOrderedState *) arg, *(const int *) a,
							*(const int *) b);
}

/* The heap of candidates keeps its worst - the last in order - on top. */
static void
lo_top_sift_down(LionOrderedState *st, int i)
{
	int		   *h = st->topheap;

	for (;;)
	{
		int			l = 2 * i + 1;
		int			m = i;
		int			t;

		if (l < st->ntop && lo_top_cmp_slots(st, h[l], h[m]) > 0)
			m = l;
		if (l + 1 < st->ntop && lo_top_cmp_slots(st, h[l + 1], h[m]) > 0)
			m = l + 1;
		if (m == i)
			return;
		t = h[i];
		h[i] = h[m];
		h[m] = t;
		i = m;
	}
}

static void
lo_top_sift_up(LionOrderedState *st, int i)
{
	int		   *h = st->topheap;

	while (i > 0)
	{
		int			p = (i - 1) / 2;
		int			t;

		if (lo_top_cmp_slots(st, h[i], h[p]) <= 0)
			return;
		t = h[i];
		h[i] = h[p];
		h[p] = t;
		i = p;
	}
}

/* Room for more candidates, up to the rows the node keeps. */
static void
lo_top_grow(LionOrderedState *st)
{
	int			cap = (int) Min((int64) st->topcap * 2, (int64) st->storek);
	Size		n = (Size) cap * st->nsort;

	st->toptid = (ItemPointerData *)
		repalloc_huge(st->toptid, sizeof(ItemPointerData) * cap);
	st->topvals = (Datum *) repalloc_huge(st->topvals, sizeof(Datum) * n);
	st->topnulls = (bool *) repalloc_huge(st->topnulls, sizeof(bool) * n);
	st->topheap = (int *) repalloc_huge(st->topheap, sizeof(int) * cap);
	st->topcap = cap;
}

/*
 * Rank one row the set keeps: tid, a TID the set holds - the root of its HOT
 * chain, which is what is fetched again at the end - and its sort keys, a
 * row the snapshot sees (lo_store_piece()).  It is kept when fewer than
 * storek are, or when it comes strictly before the worst of them, which it
 * then replaces; a row that ties with the worst is not taken, so a row that
 * has left the heap, or never entered it, can never come back, and a TID met
 * again only has to be looked for among the rows kept.
 */
static void
lo_store_fold(LionOrderedState *st, ItemPointer tid, const Datum *vals,
			  const bool *nulls)
{
	uint64		code = lion_tid_to_code(tid);
	MemoryContext oldcxt;
	bool		found;
	bool		added;
	int			slot;
	int			k;

	st->ranked++;
	if (st->ntop >= st->storek)
	{
		slot = st->topheap[0];
		if (lo_top_cmp(st, vals, nulls, LO_TOPV(st, slot), LO_TOPN(st, slot)) >= 0)
			return;
	}
	if (lo_tids_lookup(st->toptids, code) != NULL)
		return;					/* kept already, through another piece */

	oldcxt = MemoryContextSwitchTo(st->topcxt);
	added = (st->ntop < st->storek);
	if (added)
	{
		if (st->ntop == st->topcap)
			lo_top_grow(st);
		slot = st->ntop;
		st->topheap[st->ntop++] = slot;
	}
	else
	{
		/* the worst goes, with its values */
		slot = st->topheap[0];
		for (k = 0; k < st->nsort; k++)
			if (!st->sortbyval[k] && !LO_TOPN(st, slot)[k])
				pfree(DatumGetPointer(LO_TOPV(st, slot)[k]));
		lo_tids_delete(st->toptids, lion_tid_to_code(&st->toptid[slot]));
	}
	st->toptid[slot] = *tid;
	for (k = 0; k < st->nsort; k++)
	{
		LO_TOPN(st, slot)[k] = nulls[k];
		LO_TOPV(st, slot)[k] = nulls[k] ? (Datum) 0 :
			datumCopy(vals[k], st->sortbyval[k], st->sortlen[k]);
	}
	(void) lo_tids_insert(st->toptids, code, &found);
	MemoryContextSwitchTo(oldcxt);

	if (added)
		lo_top_sift_up(st, st->ntop - 1);
	else
		lo_top_sift_down(st, 0);
}

/*
 * A row of the set from the heap (DESIGN.md §40, "Reads"): one on a page the
 * visibility map does not call all-visible, one of a heap page the store left
 * to the heap (ABSENT, or no store yet), or one of a piece without the §9
 * interlock.  The fetch settles its visibility and its values together; a
 * row of an inexact piece is also tested against the lion qual, as §30.4's
 * recheck tests a member.
 */
static void
lo_store_heap(LionOrderedState *st, ItemPointer tid, bool exact)
{
	TupleTableSlot *slot = st->css.ss.ss_ScanTupleSlot;
	ExprContext *econtext = st->css.ss.ps.ps_ExprContext;
	MemoryContext oldcxt;
	int			k;

	if (!lo_fetch_tid(st, tid, slot))
		return;
	st->fetched++;
	if (!exact)
	{
		ResetExprContext(econtext);
		econtext->ecxt_scantuple = slot;
		if (!ExecQual(st->lionrecheck, econtext))
		{
			st->removed++;
			ExecClearTuple(slot);
			return;
		}
	}

	/* the values, detoasted, as the store would give them */
	MemoryContextReset(st->rowcxt);
	oldcxt = MemoryContextSwitchTo(st->rowcxt);
	for (k = 0; k < st->nsort; k++)
	{
		Datum		v = slot_getattr(slot, st->sortattnos[k], &st->rownulls[k]);

		if (!st->rownulls[k] && st->sortlen[k] == -1)
			v = PointerGetDatum(PG_DETOAST_DATUM_PACKED(v));
		st->rowvals[k] = v;
	}
	MemoryContextSwitchTo(oldcxt);
	lo_store_fold(st, tid, st->rowvals, st->rownulls);
	ExecClearTuple(slot);
}

/*
 * One piece of the set: container c, whose members all belong to the set, at
 * a moment when the page every member of it came from is still PINNED, if
 * `exact` - the caller lets go of that pin only after this returns.  That is
 * the §9 interlock, and it is what lets the store's value stand for a row:
 * the visibility map is asked about c's heap pages under the pin, and on a
 * page it calls all-visible a member is a row every snapshot sees, whose
 * slot its own insert wrote and VACUUM cannot have cleared or given to
 * another row (DESIGN.md §40, "Why it is safe", lion_store.h).  Those rows'
 * values are gathered from the store, and the others' come from the heap
 * (lo_store_heap()): every row of a page that is not all-visible, of a heap
 * page the store leaves to the heap, and every row of a piece that is not
 * exact - it has no pin, or comes from a superset that must be rechecked -
 * or of a hot standby where the pin interlocks nothing (§9, "Hot standby").
 * Under SERIALIZABLE the heap pages served from the store are predicate-
 * locked, as an index-only scan locks the pages it does not visit.
 */
static void
lo_store_piece(LionOrderedState *st, const LionContainer *c, bool exact)
{
	Relation	heap = st->css.ss.ss_currentRelation;
	BlockNumber firstblk = lion_ckey_first_block(c->ckey);
	uint64		allvis = 0;
	uint64		served = 0;
	uint32		n;
	uint32		i;
	int			k;

	st->ncont++;
	if (!exact)
		st->exact = false;
	n = lion_container_to_array(c, st->los);

	if (exact && !st->storenovm)
	{
		uint64		m = lion_container_block_mask(c);

		while (m != 0)
		{
			int			b = pg_rightmost_one_pos64(m);

			m &= m - 1;
			if (VM_ALL_VISIBLE(heap, firstblk + (BlockNumber) b, &st->vmbuf))
				allvis |= UINT64CONST(1) << b;
		}
	}

	if (allvis != 0)
	{
		uint64		absent = 0;
		int			nav = 0;

		for (i = 0; i < n; i++)
			if (allvis & (UINT64CONST(1) << (st->los[i] >> LION_OFFSET_BITS)))
				st->avlos[nav++] = st->los[i];
		for (k = 0; k < st->nsort; k++)
		{
			uint64		a;

			lion_store_gather(st->readers[k], c->ckey, st->avlos, nav,
							  st->gvals[k], st->gnulls[k], &a);
			absent |= a;
		}
		served = allvis & ~absent;
		for (i = 0; i < (uint32) nav; i++)
		{
			ItemPointerData tid;

			if ((served & (UINT64CONST(1) << (st->avlos[i] >> LION_OFFSET_BITS))) == 0)
				continue;
			for (k = 0; k < st->nsort; k++)
			{
				st->rowvals[k] = st->gvals[k][i];
				st->rownulls[k] = st->gnulls[k][i];
			}
			lion_code_to_tid(lion_make_code(c->ckey, st->avlos[i]), &tid);
			st->storevals++;
			lo_store_fold(st, &tid, st->rowvals, st->rownulls);
		}
		for (k = 0; k < st->nsort; k++)
			lion_store_gather_reset(st->readers[k]);

		if (served != 0 && IsolationIsSerializable())
		{
			Snapshot	snapshot = st->css.ss.ps.state->es_snapshot;
			uint64		m = served;

			while (m != 0)
			{
				int			b = pg_rightmost_one_pos64(m);

				m &= m - 1;
				PredicateLockPage(heap, firstblk + (BlockNumber) b, snapshot);
			}
		}
	}

	for (i = 0; i < n; i++)
	{
		ItemPointerData tid;

		if (served & (UINT64CONST(1) << (st->los[i] >> LION_OFFSET_BITS)))
			continue;
		CHECK_FOR_INTERRUPTS();
		lion_code_to_tid(lion_make_code(c->ckey, st->los[i]), &tid);
		lo_store_heap(st, &tid, exact);
	}
}

/*
 * Fold node's answer into the candidates, a piece at a time while the page
 * each piece came from is pinned (DESIGN.md §40: §30's finished set holds no
 * pin, so the fold has to happen while the set is being made).
 *
 * The set's own build cannot give that: each leaf is read whole, pinless
 * (§30.4), and a container key's answer is final only once every leaf has
 * been read and combined.  So the store order reads the tree differently:
 *
 *	- a LEAF is opened with keeppins, and each container it hands out is
 *	  folded before the source moves past it - its pin is still held -,
 *	  ANDed first with the masks the ANDs above it made (below).  A source
 *	  that is not exact (a UNION, which pins nothing, a set located past the
 *	  pin budget, a multi-key recheck) has its pieces folded from the heap
 *	  with the recheck;
 *	- an OR folds each of its children: its answer is their union, and a row
 *	  two of them hold is ranked once (lo_store_fold());
 *	- an AND reads one child the pinned way, the first leaf among them - the
 *	  DRIVER - and builds the others into a set as §30.4 builds one, pinless,
 *	  which masks the driver's pieces.  Every row of the AND is a row of the
 *	  driver's piece, so the driver's pin is the interlock for all of them
 *	  (lion_count_int.h: one pin is enough).  A mask that degraded (§30.4)
 *	  keeps the driver's piece whole at its keys, and the piece is rechecked.
 */
static void
lo_store_node(LionOrderedState *st, LoNode *node, List *masks, bool maskexact)
{
	int			i;

	if (node->kind == LO_NODE_LEAF)
	{
		LoLeaf	   *leaf = node->leaf;
		LionSource *src;
		LionContainer *buf[2] = {NULL, NULL};
		const LionContainer *c;

		src = lion_source_open(leaf->index, leaf->keys, leaf->nkeys, true,
							   st->buildcxt);
		pgstat_count_index_scan(leaf->index);
		if (masks != NIL)
		{
			buf[0] = (LionContainer *) MemoryContextAlloc(st->buildcxt,
														  LION_CONTAINER_MAX_SIZE);
			buf[1] = (LionContainer *) MemoryContextAlloc(st->buildcxt,
														  LION_CONTAINER_MAX_SIZE);
		}
		while ((c = lion_source_next(src)) != NULL)
		{
			const LionContainer *piece = c;
			bool		exact = maskexact && !st->lossyqual &&
				lion_source_exact(src);
			bool		skip = false;
			int			b = 0;
			ListCell   *lc;

			CHECK_FOR_INTERRUPTS();
			if (c->cardinality == 0)
				continue;
			foreach(lc, masks)
			{
				LionTidSet *m = (LionTidSet *) lfirst(lc);
				int			idx = lo_set_key_index(m, c->ckey);

				if (idx < 0)
				{
					skip = true;
					break;
				}
				if (m->conts[idx] == NULL)
				{
					exact = false;	/* degraded: any TID may be a member */
					continue;
				}
				if (lion_container_and(piece, m->conts[idx], buf[b]) == 0)
				{
					skip = true;
					break;
				}
				piece = buf[b];
				b = 1 - b;
			}
			if (!skip)
				lo_store_piece(st, piece, exact);
		}
		lion_source_close(src);
		if (buf[0] != NULL)
		{
			pfree(buf[0]);
			pfree(buf[1]);
		}
		return;
	}

	if (node->kind == LO_NODE_OR)
	{
		for (i = 0; i < node->nchild; i++)
			lo_store_node(st, node->child[i], masks, maskexact);
		return;
	}

	/* an AND: one child driven under its pins, the others its mask */
	{
		int			driver = 0;
		LionTidSet *rest = NULL;
		bool		saved = st->exact;
		bool		restexact;
		List	   *all;
		MemoryContext oldcxt;

		for (i = 0; i < node->nchild; i++)
		{
			if (node->child[i]->kind == LO_NODE_LEAF)
			{
				driver = i;
				break;
			}
		}
		st->exact = true;
		for (i = 0; i < node->nchild; i++)
		{
			LionTidSet *s;

			if (i == driver)
				continue;
			s = lo_build_node(st, node->child[i]);
			rest = (rest == NULL) ? s : lo_set_combine(st, rest, s, true);
			if (rest->n == 0)
				break;			/* an empty AND stays empty */
		}
		restexact = st->exact;
		st->exact = saved;
		Assert(rest != NULL);
		if (rest->n > 0)
		{
			oldcxt = MemoryContextSwitchTo(st->buildcxt);
			all = lappend(list_copy(masks), rest);
			MemoryContextSwitchTo(oldcxt);
			lo_store_node(st, node->child[driver], all,
						  maskexact && restexact);
		}
		lo_set_free(st, rest);
	}
}

/* Is every index the store order reads in rmgr mode (§9, "Hot standby")? */
static bool
lo_store_all_rmgr(LionOrderedState *st)
{
	int			i;

	if (lion_wal_mode(st->ordidx) != LION_WAL_MODE_RMGR)
		return false;
	for (i = 0; i < st->nleaves; i++)
		if (lion_wal_mode(st->leaves[i].index) != LION_WAL_MODE_RMGR)
			return false;
	return true;
}

/*
 * Fold the whole set into the candidates and put them in order (DESIGN.md
 * §40): at the first fetch after a start, or after a rescan whose Params
 * changed the lion quals - a rescan that did not change them returns the
 * same rows again, under the same snapshot (§30.4, "Rescans").
 */
static void
lo_store_build(LionOrderedState *st)
{
	LionIndexState *ix;
	MemoryContext oldcxt;
	int			cap = Min(st->storek, 1024);
	int			k;

	MemoryContextReset(st->topcxt);
	MemoryContextReset(st->setcxt);
	st->live = NIL;
	st->bytes = 0;
	st->degraded = false;
	st->exact = !st->lossyqual;
	st->limit = get_hash_memory_limit();
	st->ncont = 0;

	oldcxt = MemoryContextSwitchTo(st->topcxt);
	st->topcap = cap;
	st->toptid = (ItemPointerData *) palloc(sizeof(ItemPointerData) * cap);
	st->topvals = (Datum *) palloc(sizeof(Datum) * cap * st->nsort);
	st->topnulls = (bool *) palloc(sizeof(bool) * cap * st->nsort);
	st->topheap = (int *) palloc(sizeof(int) * cap);
	st->toptids = lo_tids_create(st->topcxt, cap, NULL);
	st->ntop = 0;
	st->toppos = 0;
	MemoryContextSwitchTo(oldcxt);

	lo_leaf_keys(st);

	/*
	 * A reader of each sort key's column, over the index's state as it is
	 * now; the plan was made for an index that stored them, and one rebuilt
	 * since without them is an error rather than a wrong answer.
	 */
	ix = lion_get_index_state(st->ordidx);
	for (k = 0; k < st->nsort; k++)
	{
		int			ord = lion_store_ordinal(ix, st->storeattnos[k]);

		if (ord < 0 || !ix->stored[ord].returnable)
			elog(ERROR, "LionOrdered: index \"%s\" does not store column %d",
				 RelationGetRelationName(st->ordidx), st->storeattnos[k]);
		st->readers[k] = lion_store_open(st->ordidx, ix, ord, st->buildcxt);
	}

	/*
	 * On a hot standby a generic-mode index's pins interlock nothing (§9,
	 * "Hot standby"): every row then comes from the heap, as the count
	 * rechecks every TID there.
	 */
	st->storenovm = RecoveryInProgress() && !lo_store_all_rmgr(st);

	lo_store_node(st, st->tree, NIL, true);

	if (BufferIsValid(st->vmbuf))
		ReleaseBuffer(st->vmbuf);
	st->vmbuf = InvalidBuffer;
	for (k = 0; k < st->nsort; k++)
	{
		lion_store_close(st->readers[k]);
		st->readers[k] = NULL;
	}
	MemoryContextReset(st->buildcxt);
	MemoryContextReset(st->setcxt);
	st->live = NIL;
	st->bytes = 0;

	/* the candidates in order; rows that tie come in any order */
	qsort_arg(st->topheap, st->ntop, sizeof(int), lo_top_qcmp, st);
	st->storebuilt = true;
	st->builds++;
}

/*
 * The next row of the store order: the candidates in order, each fetched
 * again by its TID.  Each is a row the snapshot sees - on a page the
 * visibility map called all-visible under the pin, or found visible in the
 * heap - so it is still there, and still the row ranked; anything else is
 * an error, never a row left out.
 */
static TupleTableSlot *
lo_store_next(LionOrderedState *st, TupleTableSlot *slot)
{
	if (!st->storebuilt)
		lo_store_build(st);

	while (st->toppos < st->ntop)
	{
		int			s = st->topheap[st->toppos++];

		CHECK_FOR_INTERRUPTS();
		if (!lo_fetch_tid(st, &st->toptid[s], slot))
			elog(ERROR, "LionOrdered: the row at (%u,%u) the store order kept is not visible",
				 ItemPointerGetBlockNumber(&st->toptid[s]),
				 ItemPointerGetOffsetNumber(&st->toptid[s]));
		st->fetched++;
#ifdef USE_ASSERT_CHECKING
		{
			int			k;

			for (k = 0; k < st->nsort; k++)
			{
				bool		isnull;
				Datum		v = slot_getattr(slot, st->sortattnos[k], &isnull);

				Assert(ApplySortComparator(v, isnull, LO_TOPV(st, s)[k],
										   LO_TOPN(st, s)[k],
										   &st->sortkeys[k]) == 0);
			}
		}
#endif
		return slot;
	}
	st->done = true;
	return ExecClearTuple(slot);
}

/* ExecScan's access method: the next member with a visible version. */
static TupleTableSlot *
lo_next(ScanState *ss)
{
	LionOrderedState *st = (LionOrderedState *) ss;
	TupleTableSlot *slot = ss->ss_ScanTupleSlot;
	ExprContext *econtext = ss->ps.ps_ExprContext;

	/* the store order has no walk (DESIGN.md §40) */
	if (st->store)
		return lo_store_next(st, slot);

	/*
	 * The set: lazily, evaluated only where the walk goes, when its keys
	 * can be sought (§30.4, "The set, lazily"); built otherwise.
	 */
	if (st->set == NULL && !st->noset && !st->lazy &&
		!(st->lazyok && lo_lazy_begin(st)))
		lo_build_set(st);
	if (!st->started)
		lo_start_walk(st);

	/* an empty set selects nothing, and needs no walk */
	if (st->done ||
		(!st->noset && (st->lazy ? st->lazyempty : st->set->n == 0)))
		return ExecClearTuple(slot);
	if (st->sorting)
		return lo_sort_next(st, slot);

	for (;;)
	{
		ItemPointer tid;
		int			idx;
		bool		counted;
		bool		ordrecheck;

		CHECK_FOR_INTERRUPTS();

		/*
		 * The walk does not go in heap order, or the lazy set's probes have
		 * cost what building it would, or the walk has gone as far as the
		 * switch below would let it go were the set as big as its entries'
		 * counts say - or far enough anyway that the early stop and the
		 * switch should have the set's size to go by: build it (§30.4, "The
		 * set, lazily").  Whether the switch comes is decided on the set
		 * built, so a count that is off moves only when the set is built.
		 */
		if (st->lazy &&
			((st->lazyevals >= LO_LAZY_ORDER_KEYS &&
			  st->lazyback * LO_LAZY_BACK_SHARE >= st->lazyevals) ||
			 st->lazywork >= st->lazybudget ||
			 (st->nsort > 0 && !st->noswitch &&
			  st->scanwalked >= LO_SWITCH_MIN_WALK &&
			  (double) st->scanwalked >=
			  LO_SWITCH_RATIO * (st->lazymembers - (double) st->distinct)) ||
			 st->scanwalked >= LO_LAZY_MAX_WALK ||
			 st->memobytes > st->limit / 2))
			lo_lazy_convert(st);
		counted = !st->noset && !st->lazy && !st->degraded && !st->novisit;

		/*
		 * Every member met: nothing further along can be one (§30.4,
		 * "Stopping early").  A member counts once however often the walk
		 * meets its TID; not knowable once the set has degraded.
		 */
		if (counted && st->distinct >= st->members)
			break;

		/*
		 * The walk has cost what fetching the members it has not met would:
		 * fetch and sort those instead (§30.4, "When the walk is not
		 * paying").
		 */
		if (counted && st->nsort > 0 && !st->noswitch &&
			st->scanwalked >= LO_SWITCH_MIN_WALK &&
			st->scanwalked >= LO_SWITCH_RATIO * (st->members - st->distinct) &&
			lo_switch(st))
			return lo_sort_next(st, slot);

		tid = lo_next_tid(st);
		if (tid == NULL)
			break;
		st->walked++;
		st->scanwalked++;
		if (st->lazy)
		{
			LoMemoEnt  *e = lo_lazy_find(st, tid);

			if (e == NULL)
				continue;
			if (!lo_lazy_mark(st, e, tid))
				continue;		/* met before: a recycled slot (§30.4) */
			st->distinct++;
			st->hits++;
		}
		else if (!st->noset)
		{
			idx = lo_set_find(st->set, tid);
			if (idx < 0)
				continue;
			if (!lo_mark(st, idx, tid))
				continue;		/* met before: a recycled slot (§30.4) */
			st->distinct++;
			st->hits++;
		}
		if (!lo_fetch(st, slot))
			continue;
		st->fetched++;

		/*
		 * A lion column's walk is exact: its entries are tested against the
		 * range with the column's own comparison (§28).  Without a set there
		 * is nothing to recheck either: the clauses are the filter.
		 */
		ordrecheck = !st->lionwalk && st->scan->xs_recheck;
		if ((!st->noset && !st->exact) || ordrecheck)
		{
			ResetExprContext(econtext);
			econtext->ecxt_scantuple = slot;
			if (!st->noset && !st->exact &&
				!ExecQual(st->lionrecheck, econtext))
			{
				st->removed++;
				continue;
			}
			if (ordrecheck && !ExecQual(st->ordrecheck, econtext))
				continue;
		}

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

	/* the walk restarts, with its keys evaluated again */
	st->started = false;
	st->done = false;
	lo_scan_reset(st);

	/* the store order returns the rows it ranked again (DESIGN.md §40) */
	st->toppos = 0;

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
		st->storebuilt = false;
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
	if (st->owalk != NULL)
		lo_walk_end(st);
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
	if (BufferIsValid(st->vmbuf))
		ReleaseBuffer(st->vmbuf);
	st->vmbuf = InvalidBuffer;
	if (st->topcxt != NULL)
		MemoryContextDelete(st->topcxt);
	if (st->rowcxt != NULL)
		MemoryContextDelete(st->rowcxt);
	st->topcxt = NULL;
	st->rowcxt = NULL;
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
							 list_length(es->rtable) > 1 || es->verbose,
							 false);
	ExplainPropertyText(label, str, es);
}

/*
 * The store order's lines (DESIGN.md §40, EXPLAIN): the columns gathered from
 * the store, and the order they are ranked in, every key with its direction
 * and NULLS placement spelled out, and its collation where it is not the
 * column's own.
 */
static void
lo_explain_store(CustomScanState *node, ExplainState *es)
{
	CustomScan *cscan = (CustomScan *) node->ss.ps.plan;
	List	   *sk = (List *) list_nth(cscan->custom_private, LO_PRIV_SORT);
	List	   *sl = (List *) list_nth(cscan->custom_private, LO_PRIV_STORE);
	List	   *attnos = (List *) linitial(sk);
	List	   *colls = (List *) lthird(sk);
	List	   *nulls = (List *) lfourth(sk);
	Oid			relid = RelationGetRelid(node->ss.ss_currentRelation);
	int			n = list_length(attnos);
	StringInfoData cols;
	StringInfoData order;
	int			i;

	initStringInfo(&cols);
	initStringInfo(&order);
	for (i = 0; i < n; i++)
	{
		AttrNumber	attno = (AttrNumber) list_nth_int(attnos, i);
		const char *name = quote_identifier(get_attname(relid, attno, false));
		Oid			coll = list_nth_oid(colls, i);
		Oid			type;
		int32		typmod;
		Oid			attcoll;

		if (i > 0)
		{
			appendStringInfoString(&cols, ", ");
			appendStringInfoString(&order, ", ");
		}
		appendStringInfoString(&cols, name);
		appendStringInfo(&order, "%s %s NULLS %s", name,
						 list_nth_int(sl, 2 + n + i) ? "DESC" : "ASC",
						 list_nth_int(nulls, i) ? "FIRST" : "LAST");
		get_atttypetypmodcoll(relid, attno, &type, &typmod, &attcoll);
		if (OidIsValid(coll) && coll != attcoll)
			appendStringInfo(&order, " COLLATE %s",
							 generate_collation_name(coll));
	}
	ExplainPropertyText("Store", cols.data, es);
	ExplainPropertyText("Order", order.data, es);
	pfree(cols.data);
	pfree(order.data);
}

static void
lo_explain(CustomScanState *node, List *ancestors, ExplainState *es)
{
	LionOrderedState *st = (LionOrderedState *) node;
	CustomScan *cscan = (CustomScan *) node->ss.ps.plan;
	StringInfoData buf;
	int			i;

	initStringInfo(&buf);
	if (st->store)
	{
		lo_explain_store(node, es);
		goto quals;
	}
	appendStringInfoString(&buf, get_rel_name(st->ordoid));
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
	else if (ScanDirectionIsBackward(st->dir))
		appendStringInfoString(&buf, " (backward)");
	ExplainPropertyText("Ordered By", buf.data, es);

quals:
	lo_explain_qual(node, (List *) list_nth(cscan->custom_exprs, LO_EXPR_ORDORIG),
					"Index Cond", ancestors, es);
	lo_explain_qual(node, (List *) list_nth(cscan->custom_exprs, LO_EXPR_LIONQUAL),
					"Lion Cond", ancestors, es);
	lo_explain_qual(node, (List *) list_nth(cscan->custom_exprs,
											LO_EXPR_VALUEQUAL),
					"Filter per Value", ancestors, es);

	resetStringInfo(&buf);
	for (i = 0; i < st->nleaves; i++)
	{
		const char *name = get_rel_name(st->leaves[i].indexoid);
		int			j;

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

	/* the store order reads its store's index too, when the set does not */
	if (st->store)
	{
		for (i = 0; i < st->nleaves; i++)
			if (st->leaves[i].indexoid == st->ordoid)
				break;
		if (i == st->nleaves)
		{
			if (buf.len > 0)
				appendStringInfoString(&buf, ", ");
			appendStringInfoString(&buf, get_rel_name(st->ordoid));
		}
	}
	/* a lion column's walk without a set reads no other lion index */
	if (buf.len > 0)
		ExplainPropertyText("Lion Indexes", buf.data, es);

	if (es->analyze && st->store)
	{
		/*
		 * The store order (DESIGN.md §40): the rows it ranked, those whose
		 * values the store gave, and every heap fetch - the rows of pages the
		 * store did not answer for, and the rows returned.
		 */
		ExplainPropertyInteger("Rows Ranked", NULL, (int64) st->ranked, es);
		ExplainPropertyInteger("Store Values", NULL, (int64) st->storevals,
							   es);
		ExplainPropertyInteger("Heap Fetches", NULL, (int64) st->fetched, es);
		if (st->removed > 0 || (st->builds > 0 && !st->exact))
			ExplainPropertyInteger("Rows Removed by Lion Recheck", NULL,
								   (int64) st->removed, es);
		if (st->builds > 0)
		{
			resetStringInfo(&buf);
			appendStringInfo(&buf, "%d containers, %s", st->ncont,
							 st->degraded ? "degraded" :
							 st->exact ? "exact" : "rechecked");
			if (st->builds > 1)
				appendStringInfo(&buf, ", %llu builds",
								 (unsigned long long) st->builds);
			ExplainPropertyText("Lion Set", buf.data, es);
		}
	}
	else if (es->analyze)
	{
		ExplainPropertyInteger("Index Entries Walked", NULL,
							   (int64) st->walked, es);
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
			ExplainPropertyInteger("Lion Set Hits", NULL, (int64) st->hits,
								   es);
		ExplainPropertyInteger("Heap Fetches", NULL, (int64) st->fetched, es);
		/* a node that never made its set has no exactness to report */
		if (st->removed > 0 ||
			((st->builds > 0 || st->lazykeys > 0) && !st->exact))
			ExplainPropertyInteger("Rows Removed by Lion Recheck", NULL,
								   (int64) st->removed, es);
		if (st->builds > 0)
		{
			resetStringInfo(&buf);
			appendStringInfo(&buf, "%d containers, %s", st->ncont,
							 st->degraded ? "degraded" :
							 st->exact ? "exact" : "rechecked");
			if (st->builds > 1)
				appendStringInfo(&buf, ", %llu builds",
								 (unsigned long long) st->builds);
			if (st->lazykeys > 0)
				appendStringInfo(&buf, ", after %llu keys probed",
								 (unsigned long long) st->lazykeys);
			ExplainPropertyText("Lion Set", buf.data, es);
		}
		else if (st->lazykeys > 0)
		{
			/* the set was never built: only the keys the walk met (§30.4) */
			resetStringInfo(&buf);
			appendStringInfo(&buf, "lazy, %llu keys probed, %s",
							 (unsigned long long) st->lazykeys,
							 st->exact ? "exact" : "rechecked");
			ExplainPropertyText("Lion Set", buf.data, es);
		}
		if (st->switches > 0)
		{
			resetStringInfo(&buf);
			appendStringInfo(&buf, "%llu of %llu scans, %llu members fetched",
							 (unsigned long long) st->switches,
							 (unsigned long long) st->scans,
							 (unsigned long long) st->sortfetched);
			ExplainPropertyText("Switched to Fetch and Sort", buf.data, es);
		}
	}
	pfree(buf.data);
}

/* ---------------------------------------------------------------------
 * LionStoreScan: the row gather (DESIGN.md §40, "As built: the row gather")
 * --------------------------------------------------------------------- */

/*
 * The set, streamed a piece at a time: the members at one container key, and
 * which of them the scan may take from the window store.
 *
 *	all		every member
 *	sure	the members certainly in the set: == all when every one is, NULL
 *			when none is (a superset to recheck - a source that is not exact,
 *			a set that degraded), or a container of its own
 *	pin		the members in sure whose container page is still pinned, the
 *			§9 interlock that lets the store's value stand for a row on a
 *			page the visibility map calls all-visible: NULL, == sure, or a
 *			container of its own
 *
 * The containers stay valid until the cursor that handed the piece out is
 * asked for the next one, and the pins with them.
 */
typedef struct LsPiece
{
	uint32		ckey;
	const LionContainer *all;
	const LionContainer *sure;
	const LionContainer *pin;
} LsPiece;

#define LS_CUR_SET		3		/* beside LO_NODE_LEAF, _AND and _OR */

/*
 * A cursor over one node of the lion tree, handing out its answer piece by
 * piece (ls_cursor_next()), and the pins of each piece with it:
 *
 *	LEAF	a lion source opened with keeppins, whose containers are handed out
 *			as they come; pinned when the source is exact and still has the
 *			§9 pin of the page each came from (lion_source_interlocked());
 *	AND		its DRIVER - its first LEAF child, as lo_store_node() takes it -
 *			streamed, and the other children built into one set, pinless, as
 *			§30.4 builds one (lo_build_node()), which masks each of the
 *			driver's pieces.  Every member of the AND is a member of the
 *			driver's piece, so the driver's pin is the interlock for all of
 *			them (lion_count_int.h: one pin is enough).  A mask that degraded
 *			keeps the driver's piece whole at its keys, to be rechecked;
 *	OR		its children merged by container key, each of which must hand out
 *			one ascending run of keys: a piece's members are the union of the
 *			children's at its key, so that a row two of them hold is returned
 *			once, and a member is pinned when a child that holds it held its
 *			pin (for an OR every contributing child's pin counts, each for
 *			its own members);
 *	SET		a child of an OR that would not come in one run (a WALK, a long
 *			list - which ls_tree_ok() declines, but whose shape is the
 *			source's to decide at run time): built pinless into a set and
 *			handed out key by key, every member from the heap.  A key whose
 *			container the set dropped when it degraded is every TID of the
 *			window's heap pages, rechecked.
 */
typedef struct LsCursor
{
	int			kind;			/* LO_NODE_LEAF, _AND, _OR, or LS_CUR_SET */
	bool		sorted;			/* one ascending run of container keys */
	LsPiece		piece;			/* the piece handed out last */
	LionContainer *buf[3];		/* a piece's all, sure and pin, when built */

	/* LEAF */
	LionSource *src;

	/* AND */
	struct LsCursor *driver;
	LionTidSet *mask;			/* NULL: the AND is empty */
	bool		maskexact;

	/* OR */
	int			nchild;
	struct LsCursor **child;
	bool	   *live;			/* [nchild] the child has a piece in hand */
	bool	   *taken;			/* [nchild] ... which the last piece used */

	/* SET */
	LionTidSet *set;
	int			pos;
	bool		setexact;
} LsCursor;

static LsCursor *ls_cursor_open(LionOrderedState *st, LoNode *node);
static bool ls_cursor_next(LionOrderedState *st, LsCursor *cur);
static void ls_cursor_close(LsCursor *cur);

static LionContainer *
ls_buf(LionOrderedState *st)
{
	return (LionContainer *) MemoryContextAlloc(st->buildcxt,
												LION_CONTAINER_MAX_SIZE);
}

/* A child of an OR read as a set, pinless (LsCursor, SET). */
static LsCursor *
ls_set_cursor(LionOrderedState *st, LoNode *node)
{
	LsCursor   *cur = (LsCursor *) MemoryContextAllocZero(st->buildcxt,
														  sizeof(LsCursor));
	bool		saved = st->exact;

	cur->kind = LS_CUR_SET;
	cur->sorted = true;
	st->exact = true;
	cur->set = lo_build_node(st, node);
	cur->setexact = st->exact;
	st->exact = saved;
	cur->buf[0] = ls_buf(st);
	return cur;
}

static LsCursor *
ls_cursor_open(LionOrderedState *st, LoNode *node)
{
	LsCursor   *cur = (LsCursor *) MemoryContextAllocZero(st->buildcxt,
														  sizeof(LsCursor));
	int			i;

	cur->kind = node->kind;
	if (node->kind == LO_NODE_LEAF)
	{
		LoLeaf	   *leaf = node->leaf;

		cur->src = lion_source_open_ext(leaf->index, leaf->keys, leaf->nkeys,
										true, true, st->buildcxt);
		pgstat_count_index_scan(leaf->index);
		cur->sorted = lion_source_sorted(cur->src);
		return cur;
	}

	if (node->kind == LO_NODE_AND)
	{
		int			driver = 0;
		LionTidSet *rest = NULL;
		bool		saved = st->exact;

		for (i = 0; i < node->nchild; i++)
		{
			if (node->child[i]->kind == LO_NODE_LEAF)
			{
				driver = i;
				break;
			}
		}

		/* the mask first, pinless, as lo_store_node() builds it */
		st->exact = true;
		for (i = 0; i < node->nchild; i++)
		{
			LionTidSet *s;

			if (i == driver)
				continue;
			s = lo_build_node(st, node->child[i]);
			rest = (rest == NULL) ? s : lo_set_combine(st, rest, s, true);
			if (rest->n == 0)
				break;			/* an empty AND stays empty */
		}
		cur->maskexact = st->exact;
		st->exact = saved;
		Assert(rest != NULL);
		cur->sorted = true;
		if (rest->n == 0)
			return cur;			/* cur->mask NULL: nothing to hand out */
		cur->mask = rest;
		cur->driver = ls_cursor_open(st, node->child[driver]);
		cur->sorted = cur->driver->sorted;
		for (i = 0; i < 3; i++)
			cur->buf[i] = ls_buf(st);
		return cur;
	}

	Assert(node->kind == LO_NODE_OR);
	cur->nchild = node->nchild;
	cur->child = (LsCursor **) MemoryContextAlloc(st->buildcxt,
												  sizeof(LsCursor *) * node->nchild);
	cur->live = (bool *) MemoryContextAllocZero(st->buildcxt,
												sizeof(bool) * node->nchild);
	cur->taken = (bool *) MemoryContextAllocZero(st->buildcxt,
												 sizeof(bool) * node->nchild);
	for (i = 0; i < node->nchild; i++)
	{
		LsCursor   *c = ls_cursor_open(st, node->child[i]);

		if (!c->sorted)
		{
			/* not one run of keys: the child is read whole, pinless */
			ls_cursor_close(c);
			c = ls_set_cursor(st, node->child[i]);
		}
		cur->child[i] = c;
		cur->taken[i] = true;	/* to be read for its first piece */
	}
	cur->sorted = true;
	for (i = 0; i < 3; i++)
		cur->buf[i] = ls_buf(st);
	return cur;
}

/* Let go of every source of the cursor, and so of every pin it holds. */
static void
ls_cursor_close(LsCursor *cur)
{
	int			i;

	if (cur == NULL)
		return;
	if (cur->src != NULL)
		lion_source_close(cur->src);
	cur->src = NULL;
	ls_cursor_close(cur->driver);
	cur->driver = NULL;
	for (i = 0; i < cur->nchild; i++)
		ls_cursor_close(cur->child[i]);
	cur->nchild = 0;
}

/*
 * Every TID of window ckey's heap pages: what a key of a set that degraded
 * stands for (§30.4), as a BITSET, whose bit for lo is lo - each page's line
 * pointers as the page has them now, as a bitmap heap scan reads a lossy
 * page.  A row the snapshot sees was on its page, under its line pointer,
 * before the scan began, and a line pointer is never taken back while a row
 * still lies under it; the pages are the ones the heap had when the scan
 * started, which a VACUUM cannot truncate while the scan's lock is held.
 */
static const LionContainer *
ls_full_container(LionOrderedState *st, uint32 ckey, LionContainer *out)
{
	Relation	heap = st->css.ss.ss_currentRelation;
	BlockNumber first = lion_ckey_first_block(ckey);
	uint64	   *w;
	uint32		b;

	lion_container_bitset_init(out, ckey);
	w = LION_BITSET_DATA(out);
	for (b = 0; b < LION_BLOCKS_PER_CONTAINER && first + b < st->nblocks; b++)
	{
		Buffer		buf = ReadBuffer(heap, first + b);
		OffsetNumber maxoff;
		uint32		off;

		LockBuffer(buf, BUFFER_LOCK_SHARE);
		maxoff = PageGetMaxOffsetNumber(BufferGetPage(buf));
		UnlockReleaseBuffer(buf);
		maxoff = Min(maxoff, LION_MAX_OFFSET);
		for (off = FirstOffsetNumber; off <= maxoff; off++)
		{
			uint32		lo = (b << LION_OFFSET_BITS) | off;

			w[lo >> 6] |= UINT64CONST(1) << (lo & 63);
		}
	}
	(void) lion_container_bitset_recount(out);
	return out;
}

/* AND's next piece: the driver's, masked by the other children's set. */
static bool
ls_and_next(LionOrderedState *st, LsCursor *cur)
{
	if (cur->mask == NULL)
		return false;
	for (;;)
	{
		const LsPiece *p;
		const LionContainer *m;
		int			idx;

		if (!ls_cursor_next(st, cur->driver))
			return false;
		p = &cur->driver->piece;
		idx = lo_set_key_index(cur->mask, p->ckey);
		if (idx < 0)
			continue;
		m = cur->mask->conts[idx];
		cur->piece.ckey = p->ckey;
		if (m == NULL)
		{
			/* the mask degraded: any TID of the key may be a member */
			cur->piece.all = p->all;
			cur->piece.sure = NULL;
			cur->piece.pin = NULL;
			return true;
		}
		if (lion_container_and(p->all, m, cur->buf[0]) == 0)
			continue;
		cur->piece.all = cur->buf[0];
		if (!cur->maskexact || p->sure == NULL)
			cur->piece.sure = NULL;
		else if (p->sure == p->all)
			cur->piece.sure = cur->buf[0];
		else
		{
			(void) lion_container_and(p->sure, m, cur->buf[1]);
			cur->piece.sure = cur->buf[1];
		}
		if (cur->piece.sure == NULL || p->pin == NULL)
			cur->piece.pin = NULL;
		else if (p->pin == p->all)
			cur->piece.pin = cur->buf[0];
		else if (p->pin == p->sure)
			cur->piece.pin = cur->piece.sure;
		else
		{
			(void) lion_container_and(p->pin, m, cur->buf[2]);
			cur->piece.pin = cur->buf[2];
		}
		return true;
	}
}

/*
 * OR's next piece: the union of its children's pieces at the lowest key any
 * of them is at.  A key only one child is at is that child's piece as it is.
 */
static bool
ls_or_next(LionOrderedState *st, LsCursor *cur)
{
	uint32		ckey = 0;
	bool		any = false;
	int			nat = 0;
	int			first = -1;
	bool		allsure = true;
	bool		allpin = true;
	bool		anysure = false;
	bool		anypin = false;
	int			i;

	for (i = 0; i < cur->nchild; i++)
	{
		if (cur->taken[i])
		{
			cur->live[i] = ls_cursor_next(st, cur->child[i]);
			cur->taken[i] = false;
		}
		if (cur->live[i] && (!any || cur->child[i]->piece.ckey < ckey))
		{
			ckey = cur->child[i]->piece.ckey;
			any = true;
		}
	}
	if (!any)
		return false;

	for (i = 0; i < cur->nchild; i++)
	{
		if (cur->live[i] && cur->child[i]->piece.ckey == ckey)
		{
			const LsPiece *p = &cur->child[i]->piece;

			cur->taken[i] = true;
			if (first < 0)
				first = i;
			nat++;
			if (p->sure != p->all)
				allsure = false;
			if (p->pin != p->all)
				allpin = false;
			if (p->sure != NULL)
				anysure = true;
			if (p->pin != NULL)
				anypin = true;
		}
	}
	if (nat == 1)
	{
		cur->piece = cur->child[first]->piece;
		return true;
	}

	lion_container_bitset_init(cur->buf[0], ckey);
	if (!allsure && anysure)
		lion_container_bitset_init(cur->buf[1], ckey);
	if (!allpin && anypin)
		lion_container_bitset_init(cur->buf[2], ckey);
	for (i = 0; i < cur->nchild; i++)
	{
		const LsPiece *p;

		if (!cur->taken[i])
			continue;
		p = &cur->child[i]->piece;
		lion_container_or_into_bitset(p->all, LION_BITSET_DATA(cur->buf[0]));
		if (!allsure && p->sure != NULL)
			lion_container_or_into_bitset(p->sure,
										  LION_BITSET_DATA(cur->buf[1]));
		if (!allpin && p->pin != NULL)
			lion_container_or_into_bitset(p->pin,
										  LION_BITSET_DATA(cur->buf[2]));
	}
	(void) lion_container_bitset_recount(cur->buf[0]);
	cur->piece.ckey = ckey;
	cur->piece.all = cur->buf[0];
	if (allsure)
		cur->piece.sure = cur->buf[0];
	else if (anysure)
	{
		(void) lion_container_bitset_recount(cur->buf[1]);
		cur->piece.sure = cur->buf[1];
	}
	else
		cur->piece.sure = NULL;
	if (allpin)
		cur->piece.pin = cur->buf[0];
	else if (anypin)
	{
		(void) lion_container_bitset_recount(cur->buf[2]);
		cur->piece.pin = cur->buf[2];
	}
	else
		cur->piece.pin = NULL;
	return true;
}

/* The cursor's next piece into cur->piece; false when it has none left. */
static bool
ls_cursor_next(LionOrderedState *st, LsCursor *cur)
{
	CHECK_FOR_INTERRUPTS();

	switch (cur->kind)
	{
		case LO_NODE_LEAF:
			for (;;)
			{
				const LionContainer *c = lion_source_next(cur->src);
				bool		exact;

				if (c == NULL)
					return false;
				if (c->cardinality == 0)
					continue;
				exact = !st->lossyqual && lion_source_exact(cur->src);
				cur->piece.ckey = c->ckey;
				cur->piece.all = c;
				cur->piece.sure = exact ? c : NULL;
				cur->piece.pin = (exact && lion_source_interlocked(cur->src)) ?
					c : NULL;
				return true;
			}
		case LO_NODE_AND:
			return ls_and_next(st, cur);
		case LO_NODE_OR:
			return ls_or_next(st, cur);
		case LS_CUR_SET:
			while (cur->pos < cur->set->n)
			{
				int			i = cur->pos++;
				const LionContainer *c = cur->set->conts[i];

				cur->piece.ckey = cur->set->keys[i];
				if (c == NULL)
				{
					c = ls_full_container(st, cur->piece.ckey, cur->buf[0]);
					if (c->cardinality == 0)
						continue;
					cur->piece.all = c;
					cur->piece.sure = NULL;
					cur->piece.pin = NULL;
					return true;
				}
				if (c->cardinality == 0)
					continue;
				cur->piece.all = c;
				cur->piece.sure = (cur->setexact && !st->lossyqual) ? c : NULL;
				cur->piece.pin = NULL;
				return true;
			}
			return false;
	}
	elog(ERROR, "LionStoreScan: unexpected cursor kind %d", cur->kind);
	return false;				/* keep compiler quiet */
}

static Node *
ls_create_state(CustomScan *cscan)
{
	LionOrderedState *st = (LionOrderedState *)
		newNode(sizeof(LionOrderedState), T_CustomScanState);

	st->css.methods = &ls_exec_methods;

	/*
	 * A virtual scan slot: a row the store gives is its gathered values, and
	 * a heap row is copied in from heapslot, so that the plan's expressions,
	 * which are compiled for the scan slot's one kind, read both alike.
	 */
	st->css.slotOps = &TTSOpsVirtual;
	return (Node *) st;
}

static void
ls_begin(CustomScanState *node, EState *estate, int eflags)
{
	LionOrderedState *st = (LionOrderedState *) node;
	CustomScan *cscan = (CustomScan *) node->ss.ps.plan;
	Relation	heap = node->ss.ss_currentRelation;
	Index		scanrelid = cscan->scan.scanrelid;
	List	   *shape;
	List	   *tree;
	List	   *leafoids;
	List	   *lionquals;
	List	   *lionqual;
	List	   *srcs;
	List	   *srcattnos;
	List	   *srccols;
	Bitmapset  *gathered = NULL;
	Bitmapset  *read = NULL;
	ListCell   *pos;
	int			leafno = 0;
	int			qualno = 0;
	int			x;
	int			i;
	int			k;

	shape = (list_length(cscan->custom_private) == LS_PRIV_NMEMBERS) ?
		(List *) list_nth(cscan->custom_private, LS_PRIV_SHAPE) : NIL;
	if (shape == NIL || !IsA(shape, IntList) || list_length(shape) != 2 ||
		linitial_int(shape) != LS_PRIV_MAGIC ||
		lsecond_int(shape) != LS_PRIV_NMEMBERS ||
		list_length(cscan->custom_exprs) != LS_EXPR_NLISTS)
		elog(ERROR, "LionStoreScan: unrecognized custom_private shape (%d members)",
			 list_length(cscan->custom_private));

	st->gscan = true;
	st->lossyqual = (linitial_int((List *) list_nth(cscan->custom_private,
													LS_PRIV_INTS)) &
					 LO_FLAG_LOSSY) != 0;
	tree = (List *) list_nth(cscan->custom_private, LS_PRIV_TREE);
	leafoids = (List *) list_nth(cscan->custom_private, LS_PRIV_LEAVES);
	srcs = (List *) list_nth(cscan->custom_private, LS_PRIV_SOURCES);
	srcattnos = (List *) list_nth(cscan->custom_private, LS_PRIV_ATTNOS);
	srccols = (List *) list_nth(cscan->custom_private, LS_PRIV_COLS);
	lionquals = (List *) list_nth(cscan->custom_exprs, LS_EXPR_LIONQUALS);
	lionqual = (List *) list_nth(cscan->custom_exprs, LS_EXPR_LIONQUAL);

	/* the recheck, initialised - and so checked for EXECUTE - every time */
	st->lionrecheck = ExecInitQual(lionqual, &node->ss.ps);

	st->nleaves = list_length(leafoids);
	if (tree == NIL || st->nleaves < 1)
		elog(ERROR, "LionStoreScan: malformed lion tree");
	st->leaves = (LoLeaf *) palloc0(sizeof(LoLeaf) * st->nleaves);
	pos = list_head(tree);
	st->tree = lo_decode_tree(st, tree, &pos, leafoids, lionquals, &leafno,
							  &qualno);
	if (pos != NULL || leafno != st->nleaves ||
		qualno != list_length(lionquals))
		elog(ERROR, "LionStoreScan: malformed lion tree");

	/* the stores, and the columns each gives */
	st->nsrc = list_length(srcs);
	if (st->nsrc < 1 || list_length(srcattnos) != st->nsrc ||
		list_length(srccols) != st->nsrc)
		elog(ERROR, "LionStoreScan: malformed store list");
	st->natts = RelationGetDescr(heap)->natts;
	st->srcoids = (Oid *) palloc(sizeof(Oid) * st->nsrc);
	st->srcidx = (Relation *) palloc0(sizeof(Relation) * st->nsrc);
	st->srcfirst = (int *) palloc(sizeof(int) * (st->nsrc + 1));
	st->ngcols = 0;
	for (i = 0; i < st->nsrc; i++)
	{
		List	   *a = (List *) list_nth(srcattnos, i);
		List	   *c = (List *) list_nth(srccols, i);

		if (a == NIL || !IsA(a, IntList) || c == NIL || !IsA(c, IntList) ||
			list_length(a) != list_length(c))
			elog(ERROR, "LionStoreScan: malformed store list");
		st->ngcols += list_length(a);
	}
	st->gattnos = (AttrNumber *) palloc(sizeof(AttrNumber) * st->ngcols);
	st->gindexcols = (AttrNumber *) palloc(sizeof(AttrNumber) * st->ngcols);
	k = 0;
	for (i = 0; i < st->nsrc; i++)
	{
		ListCell   *la;
		ListCell   *lc;

		st->srcoids[i] = list_nth_oid(srcs, i);
		st->srcfirst[i] = k;
		forboth(la, (List *) list_nth(srcattnos, i),
				lc, (List *) list_nth(srccols, i))
		{
			AttrNumber	attno = (AttrNumber) lfirst_int(la);

			if (attno < 1 || attno > st->natts ||
				bms_is_member(attno, gathered))
				elog(ERROR, "LionStoreScan: malformed store list");
			gathered = bms_add_member(gathered, attno);
			st->gattnos[k] = attno;
			st->gindexcols[k] = (AttrNumber) lfirst_int(lc);
			k++;
		}
	}
	st->srcfirst[st->nsrc] = k;

	/*
	 * A row the store gives has the gathered columns and NULL for the rest:
	 * the plan must read no other (ls_add_paths() offered it so).  A heap row
	 * is copied up to the last column the plan or the recheck reads.
	 */
	pull_varattnos((Node *) cscan->scan.plan.targetlist, scanrelid, &read);
	pull_varattnos((Node *) cscan->scan.plan.qual, scanrelid, &read);
	x = -1;
	while ((x = bms_next_member(read, x)) >= 0)
	{
		AttrNumber	attno = (AttrNumber) (x + FirstLowInvalidHeapAttributeNumber);

		if (!bms_is_member(attno, gathered))
			elog(ERROR, "LionStoreScan: the plan reads column %d, which it does not gather",
				 (int) attno);
	}
	pull_varattnos((Node *) lionqual, scanrelid, &read);
	st->maxatt = 0;
	for (k = 0; k < st->ngcols; k++)
		st->maxatt = Max(st->maxatt, (int) st->gattnos[k]);
	x = -1;
	while ((x = bms_next_member(read, x)) >= 0)
	{
		AttrNumber	attno = (AttrNumber) (x + FirstLowInvalidHeapAttributeNumber);

		st->maxatt = (attno <= 0) ? st->natts : Max(st->maxatt, (int) attno);
	}
	st->maxatt = Min(st->maxatt, st->natts);

	/* EXPLAIN without ANALYZE opens nothing (it names indexes by Oid). */
	if (eflags & EXEC_FLAG_EXPLAIN_ONLY)
		return;

	lion_check_table_am(heap);
	if (!IsMVCCSnapshot(estate->es_snapshot))
		elog(ERROR, "LionStoreScan: requires an MVCC snapshot");

	st->setcxt = AllocSetContextCreate(estate->es_query_cxt,
									   "LionStoreScan set",
									   ALLOCSET_DEFAULT_SIZES);
	st->buildcxt = AllocSetContextCreate(estate->es_query_cxt,
										 "LionStoreScan stream",
										 ALLOCSET_DEFAULT_SIZES);
	st->lrtcxt = CreateExprContext(estate);
#if PG_VERSION_NUM >= 200000
#elif PG_VERSION_NUM >= 190000
	st->fetch = table_index_fetch_begin(heap, SO_NONE);
#else
	st->fetch = table_index_fetch_begin(heap);
#endif
	st->heapslot = ExecInitExtraTupleSlot(estate, RelationGetDescr(heap),
										  &TTSOpsBufferHeapTuple);

	/*
	 * Every lion index it reads, the set's and the stores', with a relation
	 * predicate lock on each before any lookup (DESIGN.md §30.5).
	 */
	for (i = 0; i < st->nleaves; i++)
	{
		LoLeaf	   *leaf = &st->leaves[i];

		leaf->index = index_open(leaf->indexoid, AccessShareLock);
		ExecIndexBuildScanKeys(&node->ss.ps, leaf->index, leaf->quals, false,
							   &leaf->keys, &leaf->nkeys,
							   &leaf->rtkeys, &leaf->nrtkeys, NULL, NULL);
		PredicateLockRelation(leaf->index, estate->es_snapshot);
	}
	for (i = 0; i < st->nsrc; i++)
	{
		st->srcidx[i] = index_open(st->srcoids[i], AccessShareLock);
		PredicateLockRelation(st->srcidx[i], estate->es_snapshot);
	}
	st->lionparams = pull_paramids((Expr *) lionquals);

	st->readers = (LionStoreReader **)
		palloc0(sizeof(LionStoreReader *) * st->ngcols);
	st->gvals = (Datum **) palloc(sizeof(Datum *) * st->ngcols);
	st->gnulls = (bool **) palloc(sizeof(bool *) * st->ngcols);
	for (k = 0; k < st->ngcols; k++)
	{
		st->gvals[k] = (Datum *) palloc(sizeof(Datum) * LION_CONTAINER_RANGE);
		st->gnulls[k] = (bool *) palloc(sizeof(bool) * LION_CONTAINER_RANGE);
	}
	st->los = (uint16 *) palloc(sizeof(uint16) * LION_CONTAINER_RANGE);
	st->avlos = (uint16 *) palloc(sizeof(uint16) * LION_CONTAINER_RANGE);
	st->pinlos = (uint16 *) palloc(sizeof(uint16) * LION_CONTAINER_RANGE);
	st->vmbuf = InvalidBuffer;
}

/* Is every index the scan reads in rmgr mode (§9, "Hot standby")? */
static bool
ls_all_rmgr(LionOrderedState *st)
{
	int			i;

	for (i = 0; i < st->nleaves; i++)
		if (lion_wal_mode(st->leaves[i].index) != LION_WAL_MODE_RMGR)
			return false;
	for (i = 0; i < st->nsrc; i++)
		if (lion_wal_mode(st->srcidx[i]) != LION_WAL_MODE_RMGR)
			return false;
	return true;
}

/*
 * Start this scan's stream: the leaves' keys for its Param values, a reader
 * of each gathered column over its index's state as it is now - the plan was
 * made for indexes that stored them, and one rebuilt since without them is an
 * error rather than a wrong answer - and the cursors, whose ANDs build their
 * masks here.
 */
static void
ls_start(LionOrderedState *st)
{
	Relation	heap = st->css.ss.ss_currentRelation;
	TupleDesc	desc = RelationGetDescr(heap);
	int			i;
	int			k;

	MemoryContextReset(st->setcxt);
	MemoryContextReset(st->buildcxt);
	st->live = NIL;
	st->bytes = 0;
	st->degraded = false;
	st->exact = !st->lossyqual;
	st->limit = get_hash_memory_limit();
	st->ncont = 0;

	lo_leaf_keys(st);
	for (i = 0; i < st->nsrc; i++)
	{
		LionIndexState *ix = lion_get_index_state(st->srcidx[i]);

		for (k = st->srcfirst[i]; k < st->srcfirst[i + 1]; k++)
		{
			int			ord = lion_store_ordinal(ix, st->gindexcols[k]);

			if (ord < 0 || !ix->stored[ord].returnable ||
				ix->stored[ord].typid !=
				TupleDescAttr(desc, st->gattnos[k] - 1)->atttypid)
				elog(ERROR, "LionStoreScan: index \"%s\" does not store column %d",
					 RelationGetRelationName(st->srcidx[i]),
					 (int) st->gattnos[k]);
			st->readers[k] = lion_store_open(st->srcidx[i], ix, ord,
											 st->buildcxt);
		}
	}

	/*
	 * On a hot standby a generic-mode index's pins interlock nothing (§9,
	 * "Hot standby"): every row then comes from the heap.
	 */
	st->storenovm = RecoveryInProgress() && !ls_all_rmgr(st);
	st->nblocks = RelationGetNumberOfBlocks(heap);
	st->pn = st->ppos = 0;
	st->pnav = st->pavpos = 0;
	st->cur = ls_cursor_open(st, st->tree);
	st->gstarted = true;
	st->gdone = false;
	st->builds++;
}

/*
 * End this scan's stream, and let go of everything it holds: every source's
 * pins, the readers, the visibility map's page and the heap's.
 */
static void
ls_stop(LionOrderedState *st)
{
	int			k;

	ls_cursor_close(st->cur);
	st->cur = NULL;
	for (k = 0; k < st->ngcols; k++)
	{
		if (st->readers[k] != NULL)
			lion_store_close(st->readers[k]);
		st->readers[k] = NULL;
	}
	if (BufferIsValid(st->vmbuf))
		ReleaseBuffer(st->vmbuf);
	st->vmbuf = InvalidBuffer;
	ExecClearTuple(st->heapslot);
#if PG_VERSION_NUM < 200000
	table_index_fetch_reset(st->fetch);
#endif
	MemoryContextReset(st->buildcxt);
	MemoryContextReset(st->setcxt);
	st->live = NIL;
	st->bytes = 0;
	st->pn = st->ppos = 0;
	st->pnav = st->pavpos = 0;
	st->gstarted = false;
}

/*
 * Take up the next piece (LsPiece): its members in order into los[], and,
 * under the pin its cursor still holds, the visibility map's word on the
 * heap pages of its pinned members - on an all-visible one such a member is
 * a row every snapshot sees, whose slot its own insert wrote and VACUUM
 * cannot have cleared or given to another row (DESIGN.md §40, "Why it is
 * safe", lion_store.h) - and the gathered columns of those members, from
 * every store, into gvals[]: lo_store_piece()'s reading, for every row of
 * the piece rather than the best few.  The heap pages a store leaves to the
 * heap (ABSENT, or no store yet) are taken out of the ones it serves: their
 * rows, and every other member, are fetched as ls_next() reaches them.
 * Under SERIALIZABLE the heap pages served are predicate-locked, as an
 * index-only scan locks the pages it does not visit.
 */
static void
ls_piece(LionOrderedState *st, const LsPiece *p)
{
	Relation	heap = st->css.ss.ss_currentRelation;
	BlockNumber firstblk = lion_ckey_first_block(p->ckey);
	uint64		allvis = 0;
	uint32		nav = 0;
	int			k;

	st->ncont++;
	if (p->sure != p->all)
		st->exact = false;
	st->pckey = p->ckey;
	st->pall = p->all;
	st->psure = p->sure;
	st->pn = lion_container_to_array(p->all, st->los);
	st->ppos = 0;
	st->pavpos = 0;
	st->pserved = 0;

	/* the last piece's values are no longer anybody's */
	for (k = 0; k < st->ngcols; k++)
		lion_store_gather_reset(st->readers[k]);

	if (p->pin != NULL && p->pin->cardinality > 0 && !st->storenovm)
	{
		uint64		m = lion_container_block_mask(p->pin);

		while (m != 0)
		{
			int			b = pg_rightmost_one_pos64(m);

			m &= m - 1;
			if (VM_ALL_VISIBLE(heap, firstblk + (BlockNumber) b, &st->vmbuf))
				allvis |= UINT64CONST(1) << b;
		}
	}

	if (allvis != 0)
	{
		const uint16 *pl = st->los;
		uint32		npl = st->pn;
		uint64		absent = 0;
		uint32		i;

		if (p->pin != p->all)
		{
			npl = lion_container_to_array(p->pin, st->pinlos);
			pl = st->pinlos;
		}
		for (i = 0; i < npl; i++)
			if (allvis & (UINT64CONST(1) << (pl[i] >> LION_OFFSET_BITS)))
				st->avlos[nav++] = pl[i];
		for (k = 0; k < st->ngcols; k++)
		{
			uint64		a = 0;

			lion_store_gather(st->readers[k], p->ckey, st->avlos, (int) nav,
							  st->gvals[k], st->gnulls[k], &a);
			absent |= a;
		}
		absent &= allvis;
		st->pserved = allvis & ~absent;
		st->gabsent += pg_popcount64(absent);

		if (st->pserved != 0 && IsolationIsSerializable())
		{
			Snapshot	snapshot = st->css.ss.ps.state->es_snapshot;
			uint64		m = st->pserved;

			while (m != 0)
			{
				int			b = pg_rightmost_one_pos64(m);

				m &= m - 1;
				PredicateLockPage(heap, firstblk + (BlockNumber) b, snapshot);
			}
		}
	}
	st->pnav = nav;
}

/* A row the store gives: its gathered values, NULL for every other column. */
static void
ls_store_row(LionOrderedState *st, TupleTableSlot *slot, uint32 j,
			 ItemPointer tid)
{
	int			k;

	ExecClearTuple(slot);
	memset(slot->tts_isnull, true, sizeof(bool) * st->natts);
	for (k = 0; k < st->ngcols; k++)
	{
		int			a = st->gattnos[k] - 1;

		slot->tts_values[a] = st->gvals[k][j];
		slot->tts_isnull[a] = st->gnulls[k][j];
	}
	ExecStoreVirtualTuple(slot);
	slot->tts_tid = *tid;
}

/*
 * A row the heap gives: the version of member tid (the root of its HOT
 * chain, as an index's TID is) that the snapshot sees, if any, and tested
 * against the lion qual when the piece is not sure of it, as §30.4's recheck
 * tests a member.  Its columns are copied into the scan slot, which holds the
 * heap tuple's values - by reference into heapslot's, which keeps them until
 * the next fetch.
 */
static bool
ls_heap_row(LionOrderedState *st, TupleTableSlot *slot, ItemPointer tid,
			uint16 lo)
{
	TupleTableSlot *hs = st->heapslot;
	bool		sure = (st->psure == st->pall) ||
		(st->psure != NULL && lion_container_contains(st->psure, lo));

	if (!lo_fetch_tid(st, tid, hs))
		return false;
	st->fetched++;
	ExecClearTuple(slot);
	if (st->maxatt > 0)
	{
		slot_getsomeattrs(hs, st->maxatt);
		memcpy(slot->tts_values, hs->tts_values, sizeof(Datum) * st->maxatt);
		memcpy(slot->tts_isnull, hs->tts_isnull, sizeof(bool) * st->maxatt);
	}
	memset(slot->tts_isnull + st->maxatt, true,
		   sizeof(bool) * (st->natts - st->maxatt));
	ExecStoreVirtualTuple(slot);
	slot->tts_tid = hs->tts_tid;
	if (!sure)
	{
		ExprContext *econtext = st->css.ss.ps.ps_ExprContext;

		ResetExprContext(econtext);
		econtext->ecxt_scantuple = slot;
		if (!ExecQual(st->lionrecheck, econtext))
		{
			st->removed++;
			ExecClearTuple(slot);
			return false;
		}
	}
	st->gheaprows++;
	return true;
}

/*
 * ExecScan's access method: the next row of the set, a piece at a time, in
 * the piece's TID order.  The cursor holds the pins of the piece being
 * returned until the next piece is asked for, as an index-only scan holds its
 * leaf's while it returns the leaf's rows; once the last row is out the scan
 * lets go of everything (ls_stop()).
 */
static TupleTableSlot *
ls_next(ScanState *ss)
{
	LionOrderedState *st = (LionOrderedState *) ss;
	TupleTableSlot *slot = ss->ss_ScanTupleSlot;

	if (st->gdone)
		return ExecClearTuple(slot);
	if (!st->gstarted)
		ls_start(st);

	for (;;)
	{
		while (st->ppos < st->pn)
		{
			uint16		lo = st->los[st->ppos++];
			ItemPointerData tid;

			CHECK_FOR_INTERRUPTS();
			lion_code_to_tid(lion_make_code(st->pckey, lo), &tid);
			if (st->pavpos < st->pnav && st->avlos[st->pavpos] == lo)
			{
				uint32		j = st->pavpos++;

				if (st->pserved & (UINT64CONST(1) << (lo >> LION_OFFSET_BITS)))
				{
					ls_store_row(st, slot, j, &tid);
					st->gstorerows++;
					return slot;
				}
			}
			Assert(st->pavpos >= st->pnav || st->avlos[st->pavpos] > lo);
			if (ls_heap_row(st, slot, &tid, lo))
				return slot;
		}

		ExecClearTuple(slot);
		if (st->cur == NULL || !ls_cursor_next(st, st->cur))
		{
			ls_stop(st);
			st->gdone = true;
			return slot;
		}
		ls_piece(st, &st->cur->piece);
	}
}

/*
 * ExecScan's recheck method.  EvalPlanQual never reaches the node: a row of
 * an UPDATE, a DELETE, a MERGE or a FOR UPDATE, and every row of a query that
 * locks one, has a row mark whose ctid or whole row the node does not gather,
 * and ls_target_attnos() declined it.
 */
static bool
ls_recheck(ScanState *ss, TupleTableSlot *slot)
{
	elog(ERROR, "LionStoreScan: EvalPlanQual recheck is not supported");
	return false;				/* keep compiler quiet */
}

static TupleTableSlot *
ls_exec(CustomScanState *node)
{
	return ExecScan(&node->ss, (ExecScanAccessMtd) ls_next,
					(ExecScanRecheckMtd) ls_recheck);
}

/*
 * A rescan streams the set again from its start, with the leaves' keys
 * evaluated again: nothing of the last scan is kept, which is what bounds the
 * node's memory to a piece's rows.
 */
static void
ls_rescan(CustomScanState *node)
{
	LionOrderedState *st = (LionOrderedState *) node;

	if (st->gstarted)
		ls_stop(st);
	st->gdone = false;
	ExecScanReScan(&node->ss);
}

static void
ls_end(CustomScanState *node)
{
	LionOrderedState *st = (LionOrderedState *) node;
	int			i;

	if (st->gstarted)
		ls_stop(st);
	if (st->heapslot != NULL)
		ExecClearTuple(st->heapslot);
#if PG_VERSION_NUM < 200000
	if (st->fetch != NULL)
		table_index_fetch_end(st->fetch);
	st->fetch = NULL;
#endif
	for (i = 0; i < st->nleaves; i++)
	{
		if (st->leaves[i].index != NULL)
			index_close(st->leaves[i].index, AccessShareLock);
		st->leaves[i].index = NULL;
	}
	for (i = 0; i < st->nsrc; i++)
	{
		if (st->srcidx[i] != NULL)
			index_close(st->srcidx[i], AccessShareLock);
		st->srcidx[i] = NULL;
	}
	if (st->setcxt != NULL)
		MemoryContextDelete(st->setcxt);
	if (st->buildcxt != NULL)
		MemoryContextDelete(st->buildcxt);
	st->setcxt = NULL;
	st->buildcxt = NULL;
}

/*
 * EXPLAIN (DESIGN.md §40, "As built: the row gather"): the lion qual and the
 * lion indexes read, as LionOrdered shows them; a `Store` line per store,
 * its columns and its index; and with ANALYZE where the rows came from, in
 * the count's words (lion_store_explain()): the rows the store gave, the rows
 * the heap gave, the all-visible heap pages the store left to the heap - and
 * the heap fetches, the rows the recheck removed, and the set's pieces.
 */
static void
ls_explain(CustomScanState *node, List *ancestors, ExplainState *es)
{
	LionOrderedState *st = (LionOrderedState *) node;
	CustomScan *cscan = (CustomScan *) node->ss.ps.plan;
	List	   *srcs = (List *) list_nth(cscan->custom_private, LS_PRIV_SOURCES);
	List	   *srcattnos = (List *) list_nth(cscan->custom_private,
											  LS_PRIV_ATTNOS);
	Oid			relid = RelationGetRelid(node->ss.ss_currentRelation);
	List	   *stores = NIL;
	StringInfoData buf;
	int			i;

	lo_explain_qual(node, (List *) list_nth(cscan->custom_exprs,
											LS_EXPR_LIONQUAL),
					"Lion Cond", ancestors, es);

	/* each index once: the set's in the order the tree names them, then the stores' */
	initStringInfo(&buf);
	for (i = 0; i < st->nleaves + list_length(srcs); i++)
	{
		Oid			oid = (i < st->nleaves) ? st->leaves[i].indexoid :
			list_nth_oid(srcs, i - st->nleaves);
		int			j;

		for (j = 0; j < i; j++)
			if (((j < st->nleaves) ? st->leaves[j].indexoid :
				 list_nth_oid(srcs, j - st->nleaves)) == oid)
				break;
		if (j < i)
			continue;
		if (buf.len > 0)
			appendStringInfoString(&buf, ", ");
		appendStringInfoString(&buf, get_rel_name(oid));
	}
	ExplainPropertyText("Lion Indexes", buf.data, es);

	for (i = 0; i < list_length(srcs); i++)
	{
		ListCell   *lc;

		resetStringInfo(&buf);
		foreach(lc, (List *) list_nth(srcattnos, i))
		{
			if (buf.len > 0)
				appendStringInfoString(&buf, ", ");
			appendStringInfoString(&buf,
								   quote_identifier(get_attname(relid,
																(AttrNumber) lfirst_int(lc),
																false)));
		}
		appendStringInfo(&buf, " (%s)", get_rel_name(list_nth_oid(srcs, i)));
		if (es->format == EXPLAIN_FORMAT_TEXT)
			ExplainPropertyText("Store", buf.data, es);
		else
			stores = lappend(stores, pstrdup(buf.data));
	}
	if (stores != NIL)
		ExplainPropertyList("Store", stores, es);

	if (es->analyze)
	{
		ExplainPropertyInteger("Store Rows", NULL, (int64) st->gstorerows, es);
		ExplainPropertyInteger("Store Rows From Heap", NULL,
							   (int64) st->gheaprows, es);
		ExplainPropertyInteger("Store Pages Absent", NULL,
							   (int64) st->gabsent, es);
		ExplainPropertyInteger("Heap Fetches", NULL, (int64) st->fetched, es);
		if (st->removed > 0 || (st->builds > 0 && !st->exact))
			ExplainPropertyInteger("Rows Removed by Lion Recheck", NULL,
								   (int64) st->removed, es);
		if (st->builds > 0)
		{
			resetStringInfo(&buf);
			appendStringInfo(&buf, "%d containers, %s", st->ncont,
							 st->degraded ? "degraded" :
							 st->exact ? "exact" : "rechecked");
			if (st->builds > 1)
				appendStringInfo(&buf, ", %llu scans",
								 (unsigned long long) st->builds);
			ExplainPropertyText("Lion Set", buf.data, es);
		}
	}
	pfree(buf.data);
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
	 * DESIGN.md §40: ORDER BY a window store's columns LIMIT n.  Off,
	 * LionOrdered offers its walks only.
	 */
	DefineCustomBoolVariable("pg_lion.enable_ordered_store",
							 "Lets LionOrdered rank an ORDER BY ... LIMIT from the columns a lion index stores.",
							 "Off, only its walks of a btree or of a lion column are offered.",
							 &lion_enable_ordered_store,
							 true,
							 PGC_USERSET,
							 0,
							 NULL, NULL, NULL);

	/*
	 * DESIGN.md §40, "As built: the row gather": a plain scan of the rows a
	 * lion set keeps with the columns the window stores hold.  Off, it is
	 * never offered.
	 */
	DefineCustomBoolVariable("pg_lion.enable_store_scan",
							 "Lets LionStoreScan return the rows lion indexes select with the columns their window stores hold.",
							 "On, the planner chooses it by price over core's index, index-only and bitmap scans; off, such a query reads its columns as core's scans do.",
							 &lion_enable_store_scan,
							 true,
							 PGC_USERSET,
							 0,
							 NULL, NULL, NULL);

	RegisterCustomScanMethods(&lo_scan_methods);
	RegisterCustomScanMethods(&ls_scan_methods);

	lion_prev_set_rel_pathlist_hook = set_rel_pathlist_hook;
	set_rel_pathlist_hook = lion_ordered_set_rel_pathlist;
}
