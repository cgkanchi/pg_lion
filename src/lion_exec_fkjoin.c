/*-------------------------------------------------------------------------
 *
 * lion_exec_fkjoin.c
 *		Executing the FK-side join (DESIGN.md §27), and the LionJoinAgg node
 *		that gives the query its aggregates back above it.
 *
 * Part of the LionCount custom scan: lion_customscan.h describes the node
 * and declares what its files share.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "lion_customscan.h"

static const CustomExecMethods lion_join_agg_exec_methods = {
	.CustomName = "LionJoinAgg",
	.BeginCustomScan = lion_begin_join_agg,
	.ExecCustomScan = lion_exec_join_agg,
	.EndCustomScan = lion_end_join_agg,
	.ReScanCustomScan = lion_rescan_join_agg,
};

/*
 * The LionJoinAgg (DESIGN.md §27, "Every aggregate over the node's rows"):
 * the Agg below it, whose rows it hands up as they are.  Its target list is
 * custom_scan_tlist column for column, so it has no projection to make - but
 * for a target that names one aggregate twice, which setrefs.c points at the
 * first of them - and it returns the Agg's own slot, whose kind is then its
 * result's.
 */
Node *
lion_create_join_agg_state(CustomScan *cscan)
{
	CustomScanState *css = (CustomScanState *)
		newNode(sizeof(CustomScanState), T_CustomScanState);

	css->methods = &lion_join_agg_exec_methods;
	return (Node *) css;
}

void
lion_begin_join_agg(CustomScanState *node, EState *estate, int eflags)
{
	CustomScan *cscan = (CustomScan *) node->ss.ps.plan;
	PlanState  *child;

	if (list_length(cscan->custom_plans) != 1)
		elog(ERROR, "LionJoinAgg: needs exactly one child plan");
	child = ExecInitNode((Plan *) linitial(cscan->custom_plans), estate,
						 eflags);
	node->custom_ps = list_make1(child);

	if (node->ss.ps.ps_ProjInfo == NULL)
	{
		node->ss.ps.resultopsset = true;
		node->ss.ps.resultops =
			ExecGetResultSlotOps(child, &node->ss.ps.resultopsfixed);
	}
}

TupleTableSlot *
lion_exec_join_agg(CustomScanState *node)
{
	PlanState  *child = (PlanState *) linitial(node->custom_ps);
	ExprContext *econtext = node->ss.ps.ps_ExprContext;
	TupleTableSlot *slot;

	slot = ExecProcNode(child);
	if (TupIsNull(slot) || node->ss.ps.ps_ProjInfo == NULL)
		return slot;

	ResetExprContext(econtext);
	econtext->ecxt_scantuple =
		ExecCopySlot(node->ss.ss_ScanTupleSlot, slot);
	return ExecProject(node->ss.ps.ps_ProjInfo);
}

void
lion_end_join_agg(CustomScanState *node)
{
	ExecEndNode((PlanState *) linitial(node->custom_ps));
}

/*
 * Core hands a changed parameter on to outer and inner plans but not to
 * custom_ps, so it is handed on here, as the LionCount node does for its
 * child.
 */
void
lion_rescan_join_agg(CustomScanState *node)
{
	PlanState  *child = (PlanState *) linitial(node->custom_ps);

	if (node->ss.ps.chgParam != NULL)
		UpdateChangedParamSet(child, node->ss.ps.chgParam);
	if (child->chgParam == NULL)
		ExecReScan(child);
}

/*
 * EXPLAIN ANALYZE's clock of the FK-side join (DESIGN.md §27, "Where a key's
 * time goes"): charge the time since *since to `phase` (to nothing when phase
 * is negative) and start the next phase now.  It does nothing unless the node
 * is timed - under EXPLAIN ANALYZE with its TIMING option, as core times a
 * node - so an untimed run pays a test and no clock read.
 */
static inline void
lion_join_clock(LionCountScanState *st, int phase, instr_time *since)
{
	instr_time	now;

	if (!st->jointiming)
		return;
	INSTR_TIME_SET_CURRENT(now);
	if (phase >= 0)
		INSTR_TIME_ACCUM_DIFF(st->jointime[phase], now, *since);
	*since = now;
}

/*
 * The FK-side join's fact filters located - each clause's sets found, and a
 * range among them collected into memory (DESIGN.md §32, "A range as a
 * source"), which can be most of a run - and timed when the node is: once a
 * run over a plain table, once a leaf's turn over a partitioned one
 * (lion_join_part_open()).  It used to be in no phase, so EXPLAIN ANALYZE
 * left a range's collection out of every timer the node prints.
 */
void
lion_join_locate_where(LionCountScanState *st)
{
	instr_time	t;

	INSTR_TIME_SET_ZERO(t);
	lion_join_clock(st, -1, &t);
	lion_locate_where(st);
	lion_join_clock(st, LION_JT_LOCATE, &t);
}

/* The child's next row, counted, and timed when the node is. */
static TupleTableSlot *
lion_join_child_next(LionCountScanState *st)
{
	TupleTableSlot *slot;
	instr_time	t;

	INSTR_TIME_SET_ZERO(t);
	lion_join_clock(st, -1, &t);
	slot = ExecProcNode(st->child);
	lion_join_clock(st, LION_JT_CHILD, &t);
	if (!TupIsNull(slot))
		st->joinchildrows++;
	return slot;
}

/*
 * One key's count - or existence test, for a semi or anti join and for the
 * rows of a count(DISTINCT) that do not carry their counts - of the fk set
 * located into groupset, ANDed with the fact filters or their collected copy;
 * the posting pages it reads are EXPLAIN ANALYZE's (DESIGN.md §27, "Where a
 * key's time goes").
 *
 * A count that probes the filters adds what it cost beyond what the same
 * count against a copy of them would have to the run's account - or the
 * leaf's, over a partitioned fact table - which lion_join_maybe_switch()
 * holds against the price of collecting them (DESIGN.md §27, "Probed, then
 * collected").  In the planner's units and at the planner's prices
 * (lion_cost_fkjoin_rel(), "The per-key terms, refitted"), from what the
 * count read: the count set up (LION_FKJOIN_COUNT_COST) and each set of a
 * union source (LION_FKJOIN_SET_COST), the filters' containers, each a seek
 * and the container found there (LION_FKJOIN_PROBE_COST; one of a set in
 * memory, LION_MEMORY_PROBE_COST), the posting pages those seeks read
 * (LION_FKJOIN_PROBE_PAGE_COST) and the unions built at a key
 * (LION_UNION_KEY_COST) - less the count against a copy the key would have
 * made instead, set up (LION_FKJOIN_COPY_COUNT_COST) and looked up at the
 * key's own containers (LION_FKJOIN_COPY_PROBE_COST).  What both ways share
 * - the key's own set read and counted - is in neither.
 *
 * A count can come out cheaper probed than against a copy - a key of one
 * container over filters small enough to be in memory - and the account
 * does not go below nothing: it is what probing has cost since it last was
 * the cheaper way, so that keys probed cheaply early in a run do not put off
 * the switch once keys that probing is dear for come.
 */
static int64
lion_join_count_key(LionCountScanState *st)
{
	int64		pages = lion_posting_pages_read;
	int64		visited = st->stats.containers_visited;
	int64		keyc = st->stats.key_containers;
	int64		copyc = st->stats.copy_containers;
	int64		unions = st->stats.unions_built;
	int64		count;
	bool		exists = (st->jointype != LION_JOIN_INNER ||
						  (st->joinrows && !st->joincounts));

	count = st->joinfiltered ?
		lion_node_count(st, 2, st->joinsources, exists) :
		lion_node_count(st, st->nsource, st->sources, exists);
	pages = lion_posting_pages_read - pages;
	st->joinposting += pages;

	if (!st->joinfiltered && st->joinswitch)
	{
		double		key = (double) (st->stats.key_containers - keyc);
		double		mem = (double) (st->stats.copy_containers - copyc);
		double		filt = (double) (st->stats.containers_visited - visited) -
			key - mem;
		double		sets = 0;
		double		rent;
		int			k;

		for (k = 1; k <= st->nitem; k++)
		{
			if (!st->sources[k].negated && st->sources[k].nsets > 1)
				sets += (double) st->sources[k].nsets;
		}
		rent = LION_FKJOIN_COUNT_COST - LION_FKJOIN_COPY_COUNT_COST +
			sets * LION_FKJOIN_SET_COST +
			Max(filt, 0.0) * LION_FKJOIN_PROBE_COST +
			mem * LION_MEMORY_PROBE_COST +
			(double) pages * LION_FKJOIN_PROBE_PAGE_COST +
			(double) (st->stats.unions_built - unions) * LION_UNION_KEY_COST -
			key * LION_FKJOIN_COPY_PROBE_COST;
		*st->joinrentp = Max(*st->joinrentp + rent, 0.0);
		(*st->joinprobedp)++;
	}
	return count;
}

/*
 * PROBED, THEN COLLECTED (DESIGN.md §27).  Whether an FK-side join probes its
 * fact filters at every count or collects them once is the planner's choice,
 * made from its estimate of the dimension rows - which core makes for a
 * filtered dimension from the product of its quals' selectivities, and which
 * a semi join over skewed keys can miss by two orders of magnitude, while
 * the keys the dimension keeps are the heavy ones, far above the fact's
 * average rows a key that the model prices each at.  A plan made to probe
 * for a few keys that meets many times as many heavy ones rebuilds the
 * filters' unions at every container of every key, where one collection
 * would have served them all.
 *
 * So a plan that probes keeps an account, and switches: every count adds
 * what probing cost it over what the same count against a copy would have
 * (lion_join_count_key()), and once the account reaches what collecting the
 * filters costs (lion_join_copy_price(), from the sets as they were located)
 * they are collected - between two keys, as lion_join_collect() would have
 * collected them before the first - and every count after that reads the
 * copy.  That is the ski-rental rule: a run that switches pays what probing
 * cost until it did, which is at most the collection's price, and the
 * collection, so it never costs more than about twice what the better of
 * the two ways chosen in hindsight would have; a run that never reaches the
 * price was right to probe.
 *
 * Only where a copy can be made and is expected to fit: a hash join's
 * memory (get_hash_memory_limit()), what a plan that collects is given - or
 * over a partitioned fact what the leaves' copies before it left of it -
 * and never on a hot standby, which makes no copy.  A copy that turns out
 * larger than expected spills, as a planned one does.  The copy is taken
 * after the query's snapshot, and is only ever counted beside a located fk
 * set, which is all "Why a stale copy is safe" asks of it; the filters were
 * located in this run, and the collection reads them as a planned one does,
 * pinning nothing.  In a parallel plan each participant keeps its own
 * account and makes its own copy, as the leader of a plan run without its
 * workers does: a probing plan has sized no shared copy, and the
 * participants reach the price at keys of their own.  Each run - each
 * rescan - starts its account again.
 */
void
lion_join_switch_reset(LionCountScanState *st)
{
	int			p;

	st->joinswitch = (st->joinclause >= 0 && !st->joincollect &&
					  lion_enable_filter_switch && !RecoveryInProgress());
	st->joinrent = 0;
	st->joinbuy = -1;
	st->joinprobed = 0;
	st->joinrentp = &st->joinrent;
	st->joinprobedp = &st->joinprobed;
	if (st->joinpart != NULL)
	{
		for (p = 0; p < st->npart; p++)
		{
			st->joinpart[p].rent = 0;
			st->joinpart[p].buy = -1;
			st->joinpart[p].probed = 0;
		}
	}
}

/*
 * The rows a node of a source's tree holds, and the container keys it has,
 * from what the located sets carry (`ntids`, `ncontainers`): a set's own, a
 * union's sets added up - at most the heap's keys - and the least of an
 * intersection's children.  Upper bounds, as lion_node_members() takes them.
 */
static void
lion_join_node_size(const LionKeyNode *node, const LionPostingSet *sets,
					int nsets, double ckeys, double *members, double *keys)
{
	int			i;

	check_stack_depth();
	*members = 0;
	*keys = 0;
	if (node == NULL)
	{
		/* the implicit union of every set */
		for (i = 0; i < nsets; i++)
		{
			if (!sets[i].found)
				continue;
			*members += (double) sets[i].ntids;
			*keys += Min((double) sets[i].ncontainers, ckeys);
		}
		*keys = Min(*keys, ckeys);
		return;
	}
	if (node->kind == LION_KN_KEY)
	{
		if (node->keyno >= 0 && node->keyno < nsets && sets[node->keyno].found)
		{
			*members = (double) sets[node->keyno].ntids;
			*keys = Min((double) sets[node->keyno].ncontainers, ckeys);
		}
		return;
	}
	for (i = 0; i < node->nargs; i++)
	{
		double		m;
		double		k;

		lion_join_node_size(node->args[i], sets, nsets, ckeys, &m, &k);
		if (node->kind == LION_KN_OR)
		{
			*members += m;
			*keys = Min(*keys + k, ckeys);
		}
		else if (i == 0 || m < *members)
		{
			*members = m;
			*keys = (i == 0) ? k : Min(*keys, k);
		}
		else
			*keys = Min(*keys, k);
	}
}

/*
 * What collecting the located fact filters would cost now, in the planner's
 * units - 0 when no copy of them can be made, or the copy is not expected to
 * fit in `budget` bytes (DESIGN.md §27, "Probed, then collected").  From the
 * sets as they were located, which carry their rows and containers: every
 * container of every set read once (LION_CONTAINER_COST), the posting pages
 * a set on pages of its own takes, a page visited each (LION_DESCENT_COST),
 * each union's image at each of its keys and each of its members set in it
 * (LION_UNION_KEY_COST, LION_UNION_MEMBER_COST), and each container of the
 * copy made (LION_FKJOIN_COPY_CONTAINER_COST).  The copy's size is the
 * planner's formula (lion_cost_fkjoin_rel()) over the rows the filters'
 * product leaves, taken as independent - its containers at most the fewest
 * any source has.  What cannot be collected is what lion_join_collect_into()
 * refuses: a range too large to collect (§32), filters that only subtract,
 * a list located a batch at a time.
 */
static double
lion_join_copy_price(LionCountScanState *st, Size budget)
{
	BlockNumber blocks;
	double		ckeys;
	double		tuples;
	double		frac = 1.0;
	double		copykeys;
	double		copyrows;
	double		bytes;
	double		cost = 0;
	double	   *srcmembers;
	bool		positive = false;
	int			k;
	int			j;

	if (st->wheremissing || st->nitem == 0 || RecoveryInProgress())
		return 0;

	blocks = RelationGetNumberOfBlocks(st->heap);
	ckeys = Max((double) blocks / LION_BLOCKS_PER_CONTAINER, 1.0);
	tuples = Max((double) st->heap->rd_rel->reltuples, 1.0);
	copykeys = ckeys;
	srcmembers = (double *) palloc0(sizeof(double) * (st->nitem + 1));

	for (k = 1; k <= st->nitem; k++)
	{
		LionCountSource *src = &st->sources[k];
		double		members;
		double		keys;
		double		setmembers = 0;

		if (src->negated)
			continue;
		if (src->rangewalk != NULL || src->nsets == 0)
		{
			pfree(srcmembers);
			return 0;
		}
		positive = true;

		for (j = 0; j < src->nsets; j++)
		{
			LionPostingSet *ps = &src->sets[j];
			double		nc;
			double		per;

			if (!ps->found)
				continue;
			nc = Max((double) ps->ncontainers, 1.0);
			setmembers += (double) ps->ntids;
			cost += nc * LION_CONTAINER_COST;
			if (!ps->is_inline && ps->mat == NULL)
			{
				per = LION_CONTAINER_HDRSZ +
					Min(2.0 * (double) ps->ntids / nc, (double) LION_BITSET_BYTES);
				cost += ceil(nc * per / (double) LION_PAGE_CAPACITY) *
					LION_DESCENT_COST;
			}
		}

		lion_join_node_size(src->tree, src->sets, src->nsets, ckeys,
							&members, &keys);
		srcmembers[k] = members;
		tuples = Max(tuples, members);
		if (src->nsets > 1)
			cost += keys * LION_UNION_KEY_COST +
				setmembers * LION_UNION_MEMBER_COST;
		copykeys = Min(copykeys, keys);
	}
	for (k = 1; k <= st->nitem; k++)
	{
		if (!st->sources[k].negated)
			frac *= srcmembers[k] / tuples;
	}
	pfree(srcmembers);
	if (!positive)
		return 0;

	copyrows = tuples * Min(frac, 1.0);
	copykeys = Max(Min(copykeys, copyrows), 1.0);
	bytes = copykeys * (LION_CONTAINER_HDRSZ + sizeof(LionContainer *) +
						Min(2.0 * copyrows / copykeys,
							(double) LION_BITSET_BYTES));
	if (bytes > (double) budget)
		return 0;
	cost += copykeys * LION_FKJOIN_COPY_CONTAINER_COST;
	return Max(cost, 1e-6);
}

/*
 * The fact filters of the FK-side join, collected once per run into a private
 * posting set when the plan asks for it (DESIGN.md §27): what each dimension
 * row's count reads instead of the filters themselves.
 *
 * Without it every count merges the fk set with the filter SOURCES, and a
 * source is sought at each of the fk set's container keys - a descent of its
 * posting tree, a leaf copied, and for a multi-key clause or a union every one
 * of its sets merged at that key - which for an fk of a few rows per key over
 * a large heap is a descent per ROW of the fk set, per dimension row.  The
 * copy is the filters' intersection, made in one pass over their containers,
 * and a count looks it up in memory at its fk set's container keys.
 *
 * Its safety is lion_sources_collect()'s: the copy is only ever counted beside
 * the dimension row's fk set, which is located under its own pin and carries
 * the §9 interlock, exactly as a materialized WHERE set is only counted beside
 * a source that does.  The WHERE sets themselves stay located, as they always
 * are for the length of a run.  Not on a standby, where the interlock depends
 * on the WAL mode of every index read (lion_count_sources_cached()) and the
 * ordinary counts are left to decide it - the planner prices a standby's
 * counts as the probing they are (lion_cost_fkjoin_rel()).
 *
 * Past a hash join's memory the copy SPILLS to a temporary file, as the hash
 * join would (2026-09-28 review).  It used to give up there, and every
 * dimension row's count then read the filters themselves: for an IN list of
 * a parameter the planner had estimated at ten values, a union of every one
 * of its sets built again for each row - hour-scale for a long list over many
 * dimension rows.  A spilled copy is read a container at a time, and a count
 * seeks it by the keys memory keeps of it.
 *
 * In a parallel plan whose workers started, the copy is made ONCE, by all the
 * participants together, in the Gather's dynamic shared memory
 * (lion_shared_copy_collect(), DESIGN.md §27 "One copy per query"), and each
 * reads it through a view of its own.  Its memory is a Parallel Hash's: a hash
 * table's times the participants the plan was made for, and past that its
 * chunks spill to files of the plan's file set.  A plan run without its
 * workers makes a copy of its own, as a serial plan does.
 */
static bool
lion_join_shares_copy(LionCountScanState *st, LionSharedCopy *shared)
{
	if (shared == NULL || st->css.ss.ps.state->es_query_dsa == NULL)
		return false;
	if (IsParallelWorker())
		return true;
	return st->joinpcxt != NULL && st->joinpcxt->nworkers_launched > 0;
}

/*
 * The copy is made into *out, in memory context cxt, from the relation being
 * counted, and shared as `shared` says: st->joinfilter in outercxt for a
 * plain table, and each leaf partition's own copy for a partitioned one
 * (lion_join_part_open()), held for the whole run, all of them within the
 * one hash table's memory `budget` leaves them.  `force` makes it for a plan
 * that probes the filters, which switches to the copy part way through a run
 * (lion_join_maybe_switch()); it is then never shared.
 */
static void
lion_join_collect_into(LionCountScanState *st, LionPostingSet *out,
					   MemoryContext cxt, LionSharedCopy *shared,
					   bool *viewshared, Size budget, bool force)
{
	EState	   *estate = st->css.ss.ps.state;
	MemoryContext oldcxt;
	LionCountStats cstats;
	instr_time	t;
	bool		ok;
	bool		spilled;
	int			k;

	st->joincollected = true;
	if ((!st->joincollect && !force) || st->wheremissing || st->nitem == 0)
		return;
	if (RecoveryInProgress())
		return;

	/*
	 * The copy is an intersection, so it needs a positive source to start
	 * from: filters that are all `IS NOT NULL` are subtracted from each fk
	 * set as they always were.  The planner does not ask for a copy then.
	 */
	for (k = 0; k < st->nitem; k++)
	{
		if (!st->sources[k + 1].negated)
			break;
	}
	if (k == st->nitem)
		return;

	/*
	 * A range taken as a source that was too large to collect on its own
	 * (DESIGN.md §32) is not a set to collect either; the counts walk it.
	 */
	for (k = 0; k < st->nitem; k++)
	{
		if (st->sources[k + 1].rangewalk != NULL)
			return;
	}

	/*
	 * The budget is a hash join's (get_hash_memory_limit(): work_mem times
	 * hash_mem_multiplier): the copy stands where the ordinary plan's hash
	 * table would - or, shared by a parallel plan's participants, where its
	 * Parallel Hash's would, which is that times the participants.
	 */
	INSTR_TIME_SET_ZERO(t);
	lion_join_clock(st, -1, &t);
	memset(&cstats, 0, sizeof(cstats));
	oldcxt = MemoryContextSwitchTo(cxt);
	if (lion_join_shares_copy(st, shared))
	{
		int			chunks;
		bool		built;

		/*
		 * The participant that indexed the copy counts it, and its spill,
		 * once for all of them; each counts the chunks it collected.
		 */
		lion_shared_copy_collect(shared, estate->es_query_dsa, st->heap,
								 estate->es_snapshot, st->nitem,
								 &st->sources[1], out, &chunks,
								 &built, &spilled, &cstats);
		*viewshared = true;
		st->joincopychunks += chunks;
		if (built)
			st->joincopies++;
		else
			spilled = false;
		ok = true;
	}
	else
		ok = lion_sources_collect(st->heap, estate->es_snapshot, st->nitem,
								  &st->sources[1], budget,
								  true, out, &spilled, &cstats);
	MemoryContextSwitchTo(oldcxt);
	lion_join_clock(st, LION_JT_COLLECT, &t);

	/*
	 * What EXPLAIN ANALYZE reports of the copy is what the counts read of it
	 * ("Where a key's time goes"); a filter that is itself a copy (a range
	 * collected into memory, §32) was read once to make this one, and that
	 * is the collection's, whose containers are counted as any are.
	 */
	cstats.copy_containers = 0;
	cstats.copy_seeks = 0;
	cstats.copy_file_reads = 0;
	lion_count_stats_add(&st->stats, &cstats);
	if (!ok)
		return;

	st->joinfiltered = true;
	if (st->npart > 0)
	{
		/* a partitioned fact table's copies are one per partition: summed */
		st->joinrunfilterrows += (int64) out->ntids;
		st->joinfilterrows = st->joinrunfilterrows;
	}
	else
		st->joinfilterrows = (int64) out->ntids;
	if (spilled)
		st->joinspilled++;

	/*
	 * No count reads the WHERE sets again this run - each reads its fk set
	 * and the copy - so their pins go now, before the child runs, rather than
	 * at the first row that goes up: pins of index pages held while the
	 * child runs user code are what keep a VACUUM of the fact table waiting
	 * ("Visibility and the §9 interlock").
	 */
	lion_unpin_where(st);

	/*
	 * Every count from here on reads its fk set and the copy - joinfiltered
	 * says so, whatever the copy holds.
	 */
	memset(st->joinsources, 0, sizeof(st->joinsources));
	st->joinsources[0].nsets = 1;
	st->joinsources[0].sets = &st->groupset;
	st->joinsources[1].nsets = 1;
	st->joinsources[1].sets = out;

	/*
	 * The filters select no row at all: as a clause with no entry does, and
	 * no key is looked up after this - which, for a copy made part way
	 * through the run, every way of reading the child sees to itself
	 * (lion_next_join_row()).
	 */
	if (!out->found)
		st->wheremissing = true;
}

static void
lion_join_collect(LionCountScanState *st)
{
	lion_join_collect_into(st, &st->joinfilter, st->outercxt,
						   st->joinsharedcopy, &st->joinviewshared,
						   get_hash_memory_limit(), false);
}

/*
 * Between two keys of a plan that probes the fact filters of a plain table:
 * collect them now, if probing has cost what collecting them would (DESIGN.md
 * §27, "Probed, then collected"; lion_join_switch_reset()).  The price is
 * taken once, from the sets as they were located, at the first count that
 * probes them; a run whose filters cannot be collected, or whose copy would
 * not fit, stops asking.  The walk of the fk index lets go of its leaf while
 * the copy is made, as it does before a row goes up, and reads it again by
 * its block number for the next key.
 */
static void
lion_join_maybe_switch(LionCountScanState *st)
{
	if (!st->joinswitch || st->joinfiltered)
		return;
	if (st->joinbuy < 0)
		st->joinbuy = lion_join_copy_price(st, get_hash_memory_limit());
	if (st->joinbuy <= 0)
	{
		st->joinswitch = false;
		return;
	}
	if (st->joinrent < st->joinbuy)
		return;

	st->joinswitch = false;
	if (st->joinwalkbegun)
		lion_lookup_walk_pause(&st->joinwalker);
	lion_join_collect_into(st, &st->joinfilter, st->outercxt, NULL,
						   &st->joinviewshared, get_hash_memory_limit(), true);
	if (st->joinfiltered)
	{
		st->joinswitches++;
		st->joinswitchkeys += st->joinprobed;
	}
}

/*
 * ... and between two keys of leaf partition p's turn (DESIGN.md §27, "A
 * partitioned fact table"): each leaf keeps an account of its own, and its
 * copy, once made, is its copy for the rest of the run, as a planned one is -
 * within what the copies of the leaves before it left of a hash table's
 * memory.
 */
static void
lion_join_part_maybe_switch(LionCountScanState *st, int p)
{
	LionJoinPart *jp = &st->joinpart[p];
	Size		limit;
	Size		held;
	Size		budget;

	if (!st->joinswitch || st->joinfiltered || jp->buy == 0)
		return;
	limit = get_hash_memory_limit();
	held = MemoryContextMemAllocated(st->joinpartcxt, true);
	budget = (held < limit) ? limit - held : 0;
	if (jp->buy < 0)
		jp->buy = lion_join_copy_price(st, budget);
	if (jp->buy <= 0 || jp->rent < jp->buy)
		return;

	/* the leaves' copies so far may have taken what it had */
	jp->buy = 0;
	if (lion_join_copy_price(st, budget) <= 0)
		return;
	lion_lookup_walk_pause(&st->joinwalker);
	lion_join_collect_into(st, &jp->filter, jp->cxt, NULL, &jp->viewshared,
						   budget, true);
	jp->filtered = st->joinfiltered;
	if (jp->filtered)
	{
		if (jp->filter.index != NULL)
			jp->filterindex = RelationGetRelid(jp->filter.index);
		st->joinswitches++;
		st->joinswitchkeys += jp->probed;
	}
	jp->missing = st->wheremissing;
}

/*
 * The forward semi join over a non-unique key (DESIGN.md §27, "Forward semi
 * joins over a non-unique key"): run the whole child and sort the keys of its
 * rows, so that they come out in order and equal ones next to each other.  A
 * NULL key joins nothing and is left out.  The sort is a datum sort of the key
 * alone - what a Sort node over one column does - within work_mem, spilling
 * past it as a Sort node's does, in the per-query memory, where it lives until
 * the run is reset (lion_reset_run()).
 */
static void
lion_join_sort_keys(LionCountScanState *st)
{
	EState	   *estate = st->css.ss.ps.state;
	MemoryContext oldcxt;

	oldcxt = MemoryContextSwitchTo(estate->es_query_cxt);
	st->joinsort = tuplesort_begin_datum(st->joinkeytype, st->joinsortop,
										 st->joinsortcoll, false, work_mem,
										 NULL, TUPLESORT_NONE);
	MemoryContextSwitchTo(oldcxt);

	for (;;)
	{
		TupleTableSlot *slot;
		Datum		key;
		bool		isnull;

		CHECK_FOR_INTERRUPTS();
		slot = lion_join_child_next(st);
		if (TupIsNull(slot))
			break;
		key = slot_getattr(slot, st->joinkeyresno, &isnull);
		if (isnull)
			continue;
		tuplesort_putdatum(st->joinsort, key, false);
		st->joinsorted++;
	}
	tuplesort_performsort(st->joinsort);
	tuplesort_get_stats(st->joinsort, &st->joinsortstats);
	st->joinhavesortstats = true;
	st->joinsortdone = true;
}

/*
 * The next distinct key of the sorted child rows, as the row that carries it,
 * or NULL when there are no more.  A row whose key the sort operator's
 * equality - the join operator's, for the key's type - finds equal to the
 * previous distinct key's is skipped: its fact rows are that key's, and have
 * been counted.
 *
 * In a parallel plan every participant has sorted every row, so the distinct
 * keys come out in the same sequence in each, and they are divided by their
 * position in it: a participant counts the keys of the runs of
 * LION_FKJOIN_UNIQUE_CHUNK it claims from the shared counter, and passes over
 * the others, which another participant claimed.  A participant claims the
 * run after its last one only once it has finished that one, so the counter
 * never gives out a run behind the participant's position, and every run is
 * counted by exactly one of them - however many start, and whether the leader
 * takes part or not.  A plan run without its workers, or a node that is not
 * parallel-aware, has no shared counter and counts every key.
 */
static TupleTableSlot *
lion_join_next_key(LionCountScanState *st)
{
	TupleTableSlot *slot = st->joinsortslot;

	for (;;)
	{
		Datum		key;
		bool		isnull;
		int64		chunk;
		MemoryContext oldcxt;

		CHECK_FOR_INTERRUPTS();
		if (!tuplesort_getdatum(st->joinsort, true, false, &key, &isnull,
								NULL))
			return NULL;
		Assert(!isnull);
		if (st->joinhaveprev &&
			DatumGetBool(FunctionCall2Coll(&st->joineqfn, st->joinsortcoll,
										   st->joinprevkey, key)))
			continue;

		/* the sort's copy lasts until the next key: keep one of our own */
		ExecClearTuple(slot);
		MemoryContextReset(st->joinkeycxt);
		oldcxt = MemoryContextSwitchTo(st->joinkeycxt);
		st->joinprevkey = datumCopy(key, st->joinkeybyval, st->joinkeylen);
		MemoryContextSwitchTo(oldcxt);
		st->joinhaveprev = true;

		chunk = st->joinkeypos++ / LION_FKJOIN_UNIQUE_CHUNK;
		if (st->shared != NULL)
		{
			if (st->joinchunk < chunk)
				st->joinchunk = (int64)
					pg_atomic_fetch_add_u32(&st->shared->nextchunk, 1);
			if (st->joinchunk != chunk)
				continue;
		}

		memset(slot->tts_isnull, true,
			   sizeof(bool) * slot->tts_tupleDescriptor->natts);
		slot->tts_values[st->joinkeyresno - 1] = st->joinprevkey;
		slot->tts_isnull[st->joinkeyresno - 1] = false;
		return ExecStoreVirtualTuple(slot);
	}
}

/*
 * LOOKUPS IN KEY ORDER (DESIGN.md §27).  Each child row's lookup used to be a
 * descent of the fk index's directory - the root, the internal levels and a
 * leaf - in the order the dimension's rows came, which is not the fk index's.
 * Over a high-cardinality fk the directory is large, the leaves those lookups
 * read are spread all over it, and consecutive rows share none of them.  So a
 * plan that asks for it (LION_JOINFLAG_WALK, which the cost model sets where
 * the walk saves a page a key: lion_cost_fkjoin_walk()) reads the child's
 * rows a BATCH at a time, sorts the batch into the directory's order and
 * locates the keys with one walk of the leaves (LionLookupWalk, lion_set.c):
 * a key on the leaf the last one was found on costs that leaf, one on the next
 * leaf a step right, and only a key further away a descent.
 *
 * Why the answers are the same.  Each row's count is the one the row-at-a-time
 * loop computes, from the same set: the walk locates exactly the entry a
 * descent would, and the count is made the same way, under the same
 * snapshot, the set located, counted and released before the next row's is
 * located.  Only the ORDER of the rows changes, and nothing above the node
 * depends on it: the node claims no order (its path has no pathkeys), a
 * partial count is grouped and added up by core's Finalize Agg whatever order
 * the rows come in, and the rows of a count(DISTINCT) go through a Sort.  A
 * key that appears again in a batch - two dimension rows with one key, which
 * a semi or anti join counts once each - comes out next to the first by the
 * sort, and takes its answer instead of a second lookup: the count of one key
 * under one snapshot is one number.
 *
 * The batch holds rows as the child made them, copied, and is bounded by
 * work_mem - another input of the node, as each input of a hash join has an
 * allowance of its own - with at least one row in it.  Nothing is pinned
 * while it is filled, since the child may run for as long as its quals take:
 * the walk lets go of its leaf, and the WHERE sets let go of theirs as they
 * do before a row goes up (lion_pause_run()), which a row-at-a-time run first
 * does after the child's first rows and a batched one would otherwise do only
 * after its whole first batch.  Every count after that is carried by its fk
 * set's own pin, as every count after the first row always was (§27,
 * "Visibility and the §9 interlock").
 */


/* The directory's order of two batched rows; NULL keys (an anti join's) first. */
static int
lion_join_ent_cmp(const void *a, const void *b, void *arg)
{
	const LionJoinEnt *x = (const LionJoinEnt *) a;
	const LionJoinEnt *y = (const LionJoinEnt *) b;

	if (x->isnull != y->isnull)
		return x->isnull ? -1 : 1;
	if (!x->isnull)
	{
		int			c = lion_lookup_walk_cmp((LionLookupWalk *) arg,
											 x->key, x->hash, y->key, y->hash);

		if (c != 0)
			return c;
	}
	return (x->seq > y->seq) - (x->seq < y->seq);
}

/*
 * Empty the batch: what it held, the slots that point into it, and the walk's
 * place, which the next batch's first key may be left of.
 */
void
lion_join_batch_reset(LionCountScanState *st)
{
	if (st->joinwalkbegun)
		lion_lookup_walk_restart(&st->joinwalker);
	if (st->joinbatchslot != NULL)
		ExecClearTuple(st->joinbatchslot);
	if (st->joinsortslot != NULL)
		ExecClearTuple(st->joinsortslot);
	if (st->joinbatchcxt != NULL)
		MemoryContextReset(st->joinbatchcxt);
	st->joinbatch = NULL;
	st->joinbatchn = 0;
	st->joinbatchcap = 0;
	st->joinbatchpos = 0;
	st->joinlasthave = false;
}

/*
 * Read the next batch: child rows (or distinct keys) until work_mem is full or
 * the child has no more, the rows whose NULL key joins nothing left out but
 * for an anti join, whose rows they are.  Then sort it into the directory's
 * order.  False when the child had no row left at all.
 */
static bool
lion_join_fill_batch(LionCountScanState *st)
{
	bool		anti = (st->jointype == LION_JOIN_ANTI);
	Size		limit = (Size) work_mem * 1024;
	MemoryContext oldcxt;
	int			n = 0;

	ExecClearTuple(st->css.ss.ss_ScanTupleSlot);
	st->childslot = NULL;
	lion_join_batch_reset(st);
	lion_pause_run(st);

	while (!st->joinchilddone)
	{
		TupleTableSlot *slot;
		LionJoinEnt *ent;
		Datum		key;
		bool		isnull;

		if (n > 0 &&
			(MemoryContextMemAllocated(st->joinbatchcxt, false) >= limit ||
			 n >= LION_JOIN_BATCH_MAX))
			break;

		CHECK_FOR_INTERRUPTS();
		slot = st->joinunique ? lion_join_next_key(st) :
			lion_join_child_next(st);
		if (TupIsNull(slot))
		{
			st->joinchilddone = true;
			break;
		}
		key = slot_getattr(slot, st->joinkeyresno, &isnull);
		if (isnull && !anti)
			continue;

		oldcxt = MemoryContextSwitchTo(st->joinbatchcxt);
		if (n >= st->joinbatchcap)
		{
			int			cap = (st->joinbatchcap == 0) ? 1024 :
				(int) Min((Size) st->joinbatchcap * 2,
						  (Size) LION_JOIN_BATCH_MAX);

			if (st->joinbatch == NULL)
				st->joinbatch = (LionJoinEnt *)
					palloc(sizeof(LionJoinEnt) * cap);
			else
				st->joinbatch = (LionJoinEnt *)
					repalloc(st->joinbatch, sizeof(LionJoinEnt) * cap);
			st->joinbatchcap = cap;
		}
		ent = &st->joinbatch[n];
		ent->seq = n;
		ent->isnull = isnull;
		ent->key = (Datum) 0;
		ent->hash = 0;
		ent->acc = 0;
		if (!isnull)
		{
			ent->key = datumCopy(key, st->joinkeybyval, st->joinkeylen);

			/* a partitioned fact table sorts it for each partition's walk */
			if (st->joinpart == NULL)
				ent->hash = lion_lookup_walk_hash(&st->joinwalker, ent->key);
		}
		ent->tuple = st->joinunique ? NULL : ExecCopySlotMinimalTuple(slot);
		MemoryContextSwitchTo(oldcxt);
		n++;
	}

	st->joinbatchn = n;
	if (n == 0)
		return false;
	st->joinbatches++;
	if (n > 1 && st->joinpart == NULL)
		qsort_arg(st->joinbatch, n, sizeof(LionJoinEnt), lion_join_ent_cmp,
				  &st->joinwalker);
	return true;
}

/* A batched row as the child's current row, for the target list. */
static void
lion_join_batch_row(LionCountScanState *st, const LionJoinEnt *ent)
{
	if (st->joinunique)
	{
		TupleTableSlot *slot = st->joinsortslot;

		ExecClearTuple(slot);
		memset(slot->tts_isnull, true,
			   sizeof(bool) * slot->tts_tupleDescriptor->natts);
		slot->tts_values[st->joinkeyresno - 1] = ent->key;
		slot->tts_isnull[st->joinkeyresno - 1] = false;
		st->childslot = ExecStoreVirtualTuple(slot);
	}
	else
		st->childslot = ExecStoreMinimalTuple(ent->tuple, st->joinbatchslot,
											  false);
}

/*
 * One batched key looked up in the relation being counted - walked to in key
 * order, or descended to - and its set counted, or tested, and let go of; or,
 * when it is the key the batch looked up last, that key's answer again (a
 * duplicated key comes out of the batch's sort next to the first).  Whether
 * it has an entry at all in *foundp.  After a lookup, the fact filters a plan
 * probes are collected from here on if probing has cost what that would
 * (lion_join_maybe_switch(), or lion_join_part_maybe_switch() for leaf
 * partition `part` of a partitioned fact; -1 for a plain one).
 */
static int64
lion_join_lookup_ent(LionCountScanState *st, const LionJoinEnt *ent,
					 bool walked, int part, bool *foundp)
{
	MemoryContext oldcxt;
	instr_time	t;
	bool		found;
	int64		count;

	if (st->joinlasthave &&
		datumIsEqual(ent->key, st->joinlastkey, st->joinkeybyval,
					 st->joinkeylen))
	{
		*foundp = st->joinlastfound;
		return st->joinlastcount;
	}

	INSTR_TIME_SET_ZERO(t);
	lion_join_clock(st, -1, &t);
	MemoryContextReset(st->pergroup);
	oldcxt = MemoryContextSwitchTo(st->pergroup);
	found = walked ?
		lion_lookup_walk_find(&st->joinwalker, ent->key, ent->hash,
							  &st->groupset) :
		lion_lookup_walk_descend(&st->joinwalker, ent->key, &st->groupset);
	lion_join_clock(st, LION_JT_LOOKUP, &t);
	count = found ? lion_join_count_key(st) : 0;
	lion_posting_set_release(&st->groupset);
	MemoryContextSwitchTo(oldcxt);
	lion_join_clock(st, LION_JT_COUNT, &t);

	st->joinlasthave = true;
	st->joinlastkey = ent->key;
	st->joinlastfound = found;
	st->joinlastcount = count;

	/* probed so far, and collected from here on if that pays */
	if (part < 0)
		lion_join_maybe_switch(st);
	else
		lion_join_part_maybe_switch(st, part);

	*foundp = found;
	return count;
}

/*
 * lion_join_next_row() for a plan that looks the keys up in key order: the
 * same rows, counted the same way, from the batches above.
 */
static bool
lion_join_next_walked(LionCountScanState *st, int64 *countp)
{
	bool		anti = (st->jointype == LION_JOIN_ANTI);

	for (;;)
	{
		LionJoinEnt *ent;
		bool		found;
		int64		count;

		CHECK_FOR_INTERRUPTS();

		/*
		 * Once a copy of the fact filters made part way through the run has
		 * found them to select nothing (lion_join_maybe_switch()), no row
		 * after joins a fact row: only an anti join has rows left.
		 */
		if (st->wheremissing && !anti)
		{
			lion_join_batch_reset(st);
			return false;
		}

		if (st->joinbatchpos >= st->joinbatchn &&
			!lion_join_fill_batch(st))
		{
			lion_join_batch_reset(st);
			return false;
		}
		ent = &st->joinbatch[st->joinbatchpos++];

		if (ent->isnull || st->wheremissing)
		{
			/*
			 * Joins nothing: an anti join's row, which only it batched - or,
			 * once that copy has been made, any row of it, the rest of the
			 * batch and the batches after, looked up no more.
			 */
			if (!anti)
				continue;
			if (!st->joinsum)
				lion_join_batch_row(st, ent);
			*countp = 1;
			return true;
		}
		st->joinlookups++;
		count = lion_join_lookup_ent(st, ent, true, -1, &found);

		if (!found)
			st->joinmissing++;
		if (anti)
			count = 1 - count;	/* no entry, or a test that found nothing */
		if (count == 0)
			continue;

		if (!st->joinsum)
			lion_join_batch_row(st, ent);
		*countp = count;
		return true;
	}
}

/*
 * The FK-side join (DESIGN.md §27): the next dimension row with fact rows -
 * its partial count in *countp, and the row itself in childslot, for the
 * target list - or false when there is none left.
 *
 * Each row of the child plan - the dimension side, under the query's snapshot,
 * with its quals, RLS and privileges applied by core - carries a key.  A NULL
 * key joins nothing, since the join operator is strict.  Any other is looked
 * up in the fk index, and its posting set, ANDed with the fact's WHERE
 * sources, is counted exactly as §15's GROUP BY driver counts one group: the
 * set is source slot 0, located, counted and released inside pergroup before
 * the next child row is fetched, so one fk set and the WHERE sets are all the
 * pins there are (DESIGN.md §9), and a lookup over the pin budget comes out
 * NOPIN and is taken care of by the count (§15).  A key with no entry, or
 * whose rows the fact filters and the snapshot leave none of, produces no row:
 * an inner join has no pair for it.  When the WHERE sources were collected
 * into one private set (lion_join_collect()) the count is the fk set ANDed
 * with that copy instead, which is the same intersection.
 *
 * A SEMI join's row is the dimension row itself, once, when it joins at least
 * one fact row, so its count is an EXISTENCE test (§26's, the same merge
 * stopped at the first visible row) and the partial count is 1.  An ANTI
 * join's row is the dimension row that joins none: a NULL key, a key with no
 * entry, a key whose test finds nothing, and every row when a fact filter
 * selects nothing at all.  The rows of a count(DISTINCT) (joinrows) are the
 * same existence tests over an inner join: the dimension rows that join,
 * once each, with no count at all - unless they carry their counts
 * (joincounts), when each is the inner join's count, for the Agg above to
 * add up in place of count() (DESIGN.md §27, "Every aggregate over the
 * node's rows").
 *
 * A forward semi join over a non-unique key (joinunique) reads its rows from
 * the sorted child instead, one per DISTINCT key (lion_join_next_key()), and
 * counts each as an inner join counts a dimension row.  The posting sets of
 * distinct keys are disjoint - a fact row has one fk value, and two distinct
 * keys cannot both equal it - so the counts add up to the fact rows with at
 * least one matching dimension row, each once: the semi join's count.
 *
 * A summed join (joinsum, lion_next_join_row()) wants no row, only the
 * count: the child's next row is fetched with no pin of the node's held
 * (lion_pause_run(), which has something to let go of only the first time -
 * the WHERE sets' pins - as a row-at-a-time join that hands its rows up lets
 * go of them at its first row).
 */
static bool
lion_join_next_row(LionCountScanState *st, int64 *countp)
{
	bool		anti = (st->jointype == LION_JOIN_ANTI);
	MemoryContext oldcxt;

	for (;;)
	{
		TupleTableSlot *childslot;
		Datum		key;
		bool		isnull;
		bool		found;
		int64		count;
		instr_time	t;

		CHECK_FOR_INTERRUPTS();

		/*
		 * A copy made part way through that selects nothing ends a semi or
		 * inner join's run, as walked (lion_join_next_walked()).
		 */
		if (st->wheremissing && !anti)
		{
			st->childslot = NULL;
			return false;
		}

		if (st->joinsum)
			lion_pause_run(st);
		ExecClearTuple(st->css.ss.ss_ScanTupleSlot);
		childslot = st->joinunique ? lion_join_next_key(st) :
			lion_join_child_next(st);
		if (TupIsNull(childslot))
		{
			st->childslot = NULL;
			return false;
		}

		key = slot_getattr(childslot, st->joinkeyresno, &isnull);
		if (isnull || st->wheremissing)
		{
			/* joins nothing: a row only of an anti join */
			if (!anti)
				continue;
			st->childslot = childslot;
			*countp = 1;
			return true;
		}
		st->joinlookups++;

		INSTR_TIME_SET_ZERO(t);
		lion_join_clock(st, -1, &t);
		MemoryContextReset(st->pergroup);
		oldcxt = MemoryContextSwitchTo(st->pergroup);

		/* a descent, with the key's probe resolved once for the node */
		found = lion_lookup_walk_descend(&st->joinwalker, key, &st->groupset);
		lion_join_clock(st, LION_JT_LOOKUP, &t);
		if (!found)
		{
			lion_posting_set_release(&st->groupset);
			MemoryContextSwitchTo(oldcxt);
			st->joinmissing++;
			if (!anti)
				continue;
			st->childslot = childslot;
			*countp = 1;
			return true;
		}

		count = lion_join_count_key(st);
		lion_posting_set_release(&st->groupset);
		MemoryContextSwitchTo(oldcxt);
		lion_join_clock(st, LION_JT_COUNT, &t);

		/* probed so far, and collected from here on if that pays */
		lion_join_maybe_switch(st);

		if (anti)
			count = 1 - count;
		if (count == 0)
			continue;

		st->childslot = childslot;
		*countp = count;
		return true;
	}
}

/*
 * A PARTITIONED FACT TABLE (DESIGN.md §27, "A partitioned fact table").  The
 * join's count for one dimension row is its count over the fact's rows, which
 * are the rows of the fact's leaf partitions - the ones the planner kept - so
 * it is the SUM of the row's counts in each of them: every key is looked up in
 * every partition's fk index and counted against that partition's own fact
 * filters.  A semi join's row joins when any partition has a match, and an
 * anti join's when none has; both are that sum compared with zero.
 *
 * The keys are read a batch at a time - the batches of "Lookups in key order",
 * whatever the plan says of the walk - and each batch is taken to the leaf
 * partitions in turn: a partition's turn begins, its fk index is walked (or
 * descended into) for every key of the batch, each key's count added into the
 * key's own entry, and the turn ends - everything located in the partition
 * let go of - before the next one's begins, so that no partition's pin
 * outlives its turn (DESIGN.md §16).  The partitions' relations themselves
 * stay open for the life of the node (lion_open_parts()).  A key that has
 * matched in one partition is not looked up again by an existence test.  Only
 * when every partition has had the batch are its rows handed up, each with
 * its sum; the node hands up no row in the middle of a partition's turn.
 *
 * Each partition is its own table in everything §9 asks: its own heap and
 * visibility map, its own pins, and a count only ever beside its own fk set,
 * located under its own pin.  Its fact filters - without the ones its bounds
 * imply (§16) - are located at its first turn and, where the plan collects
 * them, copied then into a copy of its own (lion_join_part_open()), which the
 * later batches read: the copy is made after the snapshot was taken, and is
 * counted only beside this partition's fk sets, which is all "Why a stale copy
 * is safe" asks of it.  The copies stay for the run, within one hash table's
 * memory between them, and spill past it.
 */

/*
 * Leaf partition p's turn begins: the partition made the relation counted,
 * its fk index's walk begun, and its fact filters located - or, once they
 * were collected, its copy of them made the filters of every count again,
 * with the row filter its multi-key clauses need rebuilt from what they made
 * of this run's values.
 */
static void
lion_join_part_open(LionCountScanState *st, int p)
{
	LionJoinPart *jp = &st->joinpart[p];
	LionClauseState *jcl = &st->clause[st->joinclause];
	MemoryContext oldcxt;
	int			i;

	lion_open_relation(st, p);

	oldcxt = MemoryContextSwitchTo(st->joinvisitcxt);
	lion_lookup_walk_begin(&st->joinwalker, jcl->idx, jcl->idxcol,
						   jcl->valtype);
	MemoryContextSwitchTo(oldcxt);
	st->joinwalkbegun = true;
	st->joinlasthave = false;
	st->joinfiltered = false;

	if (jp->collected && jp->filtered)
	{
		/* the copy is named after one of the partition's indexes */
		jp->filter.index = NULL;
		for (i = 0; i < st->nclause; i++)
		{
			if (st->clause[i].idx != NULL &&
				RelationGetRelid(st->clause[i].idx) == jp->filterindex)
			{
				jp->filter.index = st->clause[i].idx;
				break;
			}
		}
		if (jp->filter.index == NULL)
			elog(ERROR, "LionCount: a partition's copy of its fact filters has no index");

		for (i = 0; i < st->nclause; i++)
			st->clause[i].qmode = jp->qmode[i];
		lion_build_filter(st);
		st->located = true;
		st->wheremissing = jp->missing;
		st->joinfiltered = true;
		memset(st->joinsources, 0, sizeof(st->joinsources));
		st->joinsources[0].nsets = 1;
		st->joinsources[0].sets = &st->groupset;
		st->joinsources[1].nsets = 1;
		st->joinsources[1].sets = &jp->filter;
		return;
	}

	/*
	 * A plan that probes the filters locates them again each turn, and that
	 * is part of what probing them costs (DESIGN.md §27, "Probed, then
	 * collected"): its directory pages go on the leaf's account, as the
	 * counts' posting pages do.
	 */
	st->joinrentp = &jp->rent;
	st->joinprobedp = &jp->probed;
	if (jp->collected)
	{
		int64		dirpages = lion_dir_pages_read;

		lion_join_locate_where(st);
		if (st->joinswitch)
			jp->rent += (double) (lion_dir_pages_read - dirpages) *
				LION_DESCENT_COST;
		return;					/* probed, and located again each turn */
	}
	lion_join_locate_where(st);

	jp->collected = true;
	for (i = 0; i < st->nclause; i++)
		jp->qmode[i] = st->clause[i].qmode;
	if (st->joincollect)
	{
		Size		limit = get_hash_memory_limit();
		Size		held = MemoryContextMemAllocated(st->joinpartcxt, true);

		lion_join_collect_into(st, &jp->filter, jp->cxt, jp->shared,
							   &jp->viewshared,
							   (held < limit) ? limit - held : 0, false);
		jp->filtered = st->joinfiltered;
		if (jp->filtered && jp->filter.index != NULL)
			jp->filterindex = RelationGetRelid(jp->filter.index);
	}
	jp->missing = st->wheremissing;
}

/* ... and ends: everything it located let go of, and its turn over. */
static void
lion_join_part_close(LionCountScanState *st)
{
	if (st->joinwalkbegun)
	{
		lion_lookup_walk_pause(&st->joinwalker);
		st->joinwalkbegun = false;
	}
	lion_posting_set_release(&st->groupset);
	lion_release_where(st);
	st->joinfiltered = false;
	memset(st->joinsources, 0, sizeof(st->joinsources));
	st->joinrentp = &st->joinrent;
	st->joinprobedp = &st->joinprobed;
	lion_close_relation(st);
	MemoryContextReset(st->joinvisitcxt);
}

/*
 * The batch into the order the partition's walk takes its keys in - unless
 * it is in that order already, as it is for every partition after the first
 * whose fk index orders its keys the same way (lion_walk_order_equal()).
 */
static void
lion_join_part_sort(LionCountScanState *st)
{
	LionWalkOrder order;
	int			k;

	lion_lookup_walk_order(&st->joinwalker, &order);
	if (lion_walk_order_equal(&order, &st->joinorder))
		return;
	for (k = 0; k < st->joinbatchn; k++)
	{
		LionJoinEnt *ent = &st->joinbatch[k];

		if (!ent->isnull)
			ent->hash = lion_lookup_walk_hash(&st->joinwalker, ent->key);
	}
	if (st->joinbatchn > 1)
		qsort_arg(st->joinbatch, st->joinbatchn, sizeof(LionJoinEnt),
				  lion_join_ent_cmp, &st->joinwalker);
	st->joinorder = order;
}

/*
 * Every partition's turn with the batch just read: each key looked up and
 * counted in each partition, and the counts added up in the key's entry.  An
 * existence test - a semi or anti join, the rows of a count(DISTINCT) - needs
 * one match, and a key that has one is not looked up in the partitions after.
 * A partition whose fact filters select nothing has no turn.
 */
static void
lion_join_count_parts(LionCountScanState *st)
{
	bool		exists = (st->jointype != LION_JOIN_INNER ||
						  (st->joinrows && !st->joincounts));
	int			p;
	int			k;

	for (p = 0; p < st->npart; p++)
	{
		bool		walked;

		CHECK_FOR_INTERRUPTS();
		lion_join_part_open(st, p);
		if (st->wheremissing)
		{
			lion_join_part_close(st);
			continue;
		}
		walked = (st->joinwalk && lion_lookup_walk_ordered(&st->joinwalker));
		if (walked)
			lion_join_part_sort(st);

		for (k = 0; k < st->joinbatchn; k++)
		{
			LionJoinEnt *ent = &st->joinbatch[k];
			bool		found;
			int64		count;

			if (ent->isnull || (exists && ent->acc > 0))
				continue;
			CHECK_FOR_INTERRUPTS();
			st->joinlookups++;
			count = lion_join_lookup_ent(st, ent, walked, p, &found);
			if (!found)
				st->joinmissing++;
			ent->acc += count;

			/* a copy made part way through that selects nothing */
			if (st->wheremissing)
				break;
		}
		lion_join_part_close(st);
	}
}

/*
 * A semi or anti join path's batch over a plain fact table (DESIGN.md §27,
 * "The semi and anti join as a join path"), walked in key order: every key of
 * it tested before any of its rows goes up - into the key's entry, as a
 * partitioned fact's are (lion_join_count_parts()) - so that the rows can go
 * up in the child's order, and the walk keeps its place from one key to the
 * next without a row going up in between.  Once a copy of the fact filters
 * made part way through the run has found them to select nothing
 * (lion_join_maybe_switch()), the keys left - of this batch and the ones
 * after - are looked up no more, and join nothing.
 */
static void
lion_join_count_batch(LionCountScanState *st)
{
	int			k;

	for (k = 0; k < st->joinbatchn; k++)
	{
		LionJoinEnt *ent = &st->joinbatch[k];
		bool		found;

		if (st->wheremissing)
			break;
		if (ent->isnull)
			continue;
		CHECK_FOR_INTERRUPTS();
		st->joinlookups++;
		ent->acc = lion_join_lookup_ent(st, ent, true, -1, &found);
		if (!found)
			st->joinmissing++;
	}
}

/*
 * The batch back into the child's order, from the key order its lookups took
 * (LION_JOINFLAG_ORDERED): each row's place in the child's order is its
 * `seq`, 0 to n - 1, so a row is swapped into its place until every place
 * holds its own - n swaps at most, and no comparison.
 */
static void
lion_join_batch_unsort(LionCountScanState *st)
{
	int			i;

	for (i = 0; i < st->joinbatchn; i++)
	{
		while (st->joinbatch[i].seq != i)
		{
			int			j = st->joinbatch[i].seq;
			LionJoinEnt tmp = st->joinbatch[j];

			Assert(j >= 0 && j < st->joinbatchn && j != i);
			st->joinbatch[j] = st->joinbatch[i];
			st->joinbatch[i] = tmp;
		}
	}
}

/*
 * lion_join_next_row() for a partitioned fact table: the same rows, each with
 * its counts summed over the partitions (lion_join_count_parts()).  A NULL
 * key joins nothing, in any partition.
 *
 * And for a semi or anti join path over a plain fact table walked in key
 * order, whose batch is tested whole first (lion_join_count_batch()).  Either
 * way the rows of a join path whose order is claimed go up in the child's
 * order (lion_join_batch_unsort()); the others in the order the last walk
 * left them, or the child's where nothing sorted them.
 */
static bool
lion_join_next_parts(LionCountScanState *st, int64 *countp)
{
	bool		anti = (st->jointype == LION_JOIN_ANTI);
	bool		exists = (st->jointype != LION_JOIN_INNER ||
						  (st->joinrows && !st->joincounts));

	for (;;)
	{
		LionJoinEnt *ent;
		int64		count;

		CHECK_FOR_INTERRUPTS();
		if (st->joinbatchpos >= st->joinbatchn)
		{
			/*
			 * The batch tested whole has gone up; after a copy of a plain
			 * fact's filters that selects nothing, only an anti join has
			 * rows left.  (A leaf partition's empty copy leaves only that
			 * leaf out, and not past its turn: lion_join_count_parts().)
			 */
			if ((st->wheremissing && !anti) || !lion_join_fill_batch(st))
			{
				lion_join_batch_reset(st);
				return false;
			}
			if (st->joinpart != NULL)
			{
				memset(&st->joinorder, 0, sizeof(st->joinorder));
				lion_join_count_parts(st);
			}
			else
				lion_join_count_batch(st);
			if (st->joinordered)
				lion_join_batch_unsort(st);
		}
		ent = &st->joinbatch[st->joinbatchpos++];

		if (ent->isnull)
			count = anti ? 1 : 0;
		else if (anti)
			count = (ent->acc == 0) ? 1 : 0;
		else if (exists)
			count = (ent->acc > 0) ? 1 : 0;
		else
			count = ent->acc;
		if (count == 0)
			continue;

		if (!st->joinsum)
			lion_join_batch_row(st, ent);
		*countp = count;
		return true;
	}
}

/*
 * GROUPED BY A FACT COLUMN (DESIGN.md §27, "Grouped by a fact column").  A
 * dimension row's count is one posting set ANDed with the fact filters, and
 * says nothing of how the rows it counts divide among a fact column's values.
 * So each key is counted once per GROUP of the column instead - the column's
 * posting set for the group ANDed in as one more source - and each count that
 * is not 0 goes up as a partial row of its own: the dimension row's columns,
 * the group's value and the count, which the Finalize Agg adds up per group
 * as it adds up a dimension column's.
 *
 * The groups are a relation's own.  In the plain table, and in a partition
 * whose bounds leave the column several values, they are the entries of the
 * column's lion index there, walked a chunk of LION_FKJOIN_GROUP_CHUNK at a
 * time: the chunk's sets located - pinned, as the sets of an IN list are -
 * each key of the batch looked up once and counted in every one of them, and
 * the chunk let go of before the next; one chunk, for the few groups the cost
 * model takes this for.  In a partition whose bounds give the column one
 * value, every row is of that group, and the key's count there is the
 * group's: counted as any partition's is, with no index on the column at all.
 *
 * The keys go a batch at a time (the batches of "Lookups in key order"),
 * each batch to each relation in turn; the rows a turn counted go up once
 * the turn is over and the partition closed, as a partitioned fact's always
 * do, or - over the plain table - once its sets have let go of their pins
 * (lion_pause_run()).  A turn holds its rows until then: at most one for
 * each key and group with rows in the relation, which is no more than the
 * batch's join rows there.
 */
#define LION_FKJOIN_GROUP_CHUNK		64

/* A row counted in a group, put by until the turn is over. */
static void
lion_join_group_row(LionCountScanState *st, int ent, Datum key, bool isnull,
					int64 count)
{
	LionFactGroupState *fg = lion_st_fg(st);
	LionJoinGroupRow *r;

	if (fg->nrows >= fg->rowcap)
	{
		Size		cap = (fg->rowcap == 0) ? 256 : (Size) fg->rowcap * 2;

		if (fg->rows == NULL)
			fg->rows = (LionJoinGroupRow *)
				MemoryContextAllocExtended(fg->rowcxt,
										   sizeof(LionJoinGroupRow) * cap,
										   MCXT_ALLOC_HUGE);
		else
			fg->rows = (LionJoinGroupRow *)
				repalloc_huge(fg->rows, sizeof(LionJoinGroupRow) * cap);
		fg->rowcap = (int) Min(cap, (Size) INT_MAX);
	}
	r = &fg->rows[fg->nrows++];
	r->ent = ent;
	r->key = key;
	r->isnull = isnull;
	r->count = count;
}

/*
 * Batch entry k's key looked up in the relation being counted and counted in
 * each group of the chunk located (fg->sets), the counts that are not 0 put
 * by.
 */
static void
lion_join_count_groups(LionCountScanState *st, int k, bool walked)
{
	LionFactGroupState *fg = lion_st_fg(st);
	LionJoinEnt *ent = &st->joinbatch[k];
	MemoryContext oldcxt;
	instr_time	t;
	bool		found;
	int			j;

	INSTR_TIME_SET_ZERO(t);
	lion_join_clock(st, -1, &t);
	MemoryContextReset(st->pergroup);
	oldcxt = MemoryContextSwitchTo(st->pergroup);
	found = walked ?
		lion_lookup_walk_find(&st->joinwalker, ent->key, ent->hash,
							  &st->groupset) :
		lion_lookup_walk_descend(&st->joinwalker, ent->key, &st->groupset);
	lion_join_clock(st, LION_JT_LOOKUP, &t);
	if (!found)
		st->joinmissing++;
	else
	{
		LionCountSource *base = st->joinfiltered ? st->joinsources : st->sources;
		int			nbase = st->joinfiltered ? 2 : st->nsource;
		int64		pages = lion_posting_pages_read;

		/* the key's set and the filters - or their copy - and one group */
		Assert(nbase + 1 <= fg->nsrc);
		for (j = 0; j < fg->n; j++)
		{
			int64		count;

			memcpy(fg->src, base, sizeof(LionCountSource) * nbase);
			memset(&fg->src[nbase], 0, sizeof(LionCountSource));
			fg->src[nbase].nsets = 1;
			fg->src[nbase].sets = &fg->sets[j];
			count = lion_node_count(st, nbase + 1, fg->src, false);
			fg->groupcounts++;
			if (count > 0)
				lion_join_group_row(st, k, fg->key[j], fg->isnull[j], count);
		}
		st->joinposting += lion_posting_pages_read - pages;
	}
	lion_posting_set_release(&st->groupset);
	MemoryContextSwitchTo(oldcxt);
	lion_join_clock(st, LION_JT_COUNT, &t);
}

/*
 * Relation p's turn with the batch - leaf partition p, or the plain table for
 * p < 0: every key counted in every group of the fact column there, and the
 * counts that are not 0 put by to go up.
 */
static void
lion_join_group_turn(LionCountScanState *st, int p)
{
	LionFactGroupState *fg = lion_st_fg(st);
	Relation	fgidx = (p < 0) ? fg->idx : st->part[p].fgidx;
	Const	   *fgconst = (p < 0) ? NULL : st->part[p].fgconst;
	AttrNumber	fgidxcol;
	LionState  *istate;
	LionEntryScan es;
	MemoryContext oldcxt;
	bool		walked;
	bool		more = true;
	bool		first = true;
	int			k;
	int			j;

	if (p >= 0)
	{
		lion_join_part_open(st, p);
		if (st->wheremissing)
		{
			lion_join_part_close(st);
			return;
		}
	}
	walked = (st->joinwalk && lion_lookup_walk_ordered(&st->joinwalker));
	if (p >= 0 && walked)
		lion_join_part_sort(st);

	if (fgconst != NULL)
	{
		/*
		 * The partition's bounds give the column one value: a key's count
		 * there is its count in that group, a partition's as any other.
		 */
		for (k = 0; k < st->joinbatchn; k++)
		{
			LionJoinEnt *ent = &st->joinbatch[k];
			bool		found;
			int64		count;

			if (ent->isnull)
				continue;
			CHECK_FOR_INTERRUPTS();
			st->joinlookups++;
			count = lion_join_lookup_ent(st, ent, walked, p, &found);
			if (!found)
				st->joinmissing++;
			if (count > 0)
				lion_join_group_row(st, k, fgconst->constvalue,
									fgconst->constisnull, count);

			/* a copy made part way through that selects nothing */
			if (st->wheremissing)
				break;
		}
		lion_join_part_close(st);
		return;
	}

	/*
	 * The groups are the entries of the column's index here, its key column
	 * derived from the index as it was opened (DESIGN.md §24), and the index
	 * predicate-locked as every index the node reads is (lion_open_relation()).
	 * It is open with the relation's others for the life of the node
	 * (lion_open_relation() for a plain table, lion_open_parts() for a
	 * partition), and stays open after the turn.
	 */
	Assert(fgidx != NULL);
	lion_reader_lock(fgidx, st->css.ss.ps.state->es_snapshot);
	fgidxcol = lion_index_col_for(fgidx,
								  lion_heap_attno_in(st->heap, st->heapoid,
													 fg->attno),
								  false);
	istate = lion_index_column_state(fgidx, fgidxcol);

	/* the walk lives for the turn; the rows' keys with it, until they go up */
	oldcxt = MemoryContextSwitchTo(fg->rowcxt);
	lion_entry_scan_begin_col(&es, fgidx, fgidxcol);
	MemoryContextSwitchTo(oldcxt);

	while (more)
	{
		int			n = 0;

		MemoryContextReset(fg->cxt);
		oldcxt = MemoryContextSwitchTo(fg->cxt);
		fg->sets = (LionPostingSet *)
			palloc0(sizeof(LionPostingSet) * LION_FKJOIN_GROUP_CHUNK);
		fg->key = (Datum *) palloc(sizeof(Datum) * LION_FKJOIN_GROUP_CHUNK);
		fg->isnull = (bool *) palloc(sizeof(bool) * LION_FKJOIN_GROUP_CHUNK);
		while (n < LION_FKJOIN_GROUP_CHUNK)
		{
			Datum		key;

			CHECK_FOR_INTERRUPTS();
			if (!lion_entry_scan_next(&es, &key, &fg->sets[n]))
			{
				more = false;
				break;
			}
			fg->isnull[n] = fg->sets[n].keyisnull;
			fg->key[n] = (Datum) 0;
			if (!fg->isnull[n])
			{
				/* the key goes up with the rows, after the chunk is gone */
				MemoryContextSwitchTo(fg->rowcxt);
				fg->key[n] = datumCopy(key, istate->typbyval,
									   istate->typlen);
				MemoryContextSwitchTo(fg->cxt);
			}
			n++;
		}
		MemoryContextSwitchTo(oldcxt);
		fg->n = n;
		if (n == 0)
			break;

		/* a chunk after the first walks the fk index from its start again */
		if (!first && st->joinwalkbegun)
			lion_lookup_walk_restart(&st->joinwalker);
		first = false;

		for (k = 0; k < st->joinbatchn; k++)
		{
			if (st->joinbatch[k].isnull)
				continue;
			CHECK_FOR_INTERRUPTS();
			st->joinlookups++;
			lion_join_count_groups(st, k, walked);
		}

		for (j = 0; j < n; j++)
			lion_posting_set_release(&fg->sets[j]);
		fg->n = 0;
	}
	lion_entry_scan_end(&es);
	MemoryContextReset(fg->cxt);
	fg->sets = NULL;
	fg->key = NULL;
	fg->isnull = NULL;

	if (p >= 0)
		lion_join_part_close(st);
	else
		lion_pause_run(st);
}

/*
 * lion_next_join_row() grouped by a fact column: the next row put by, or the
 * next relation's turn with the batch, or the next batch.
 */
static TupleTableSlot *
lion_next_join_group(LionCountScanState *st)
{
	LionFactGroupState *fg = lion_st_fg(st);
	int			nturn = Max(st->npart, 1);

	/*
	 * A plain table's fact filters are located - and collected, where the
	 * plan says - once for the run, and its fk index's walk begun once, in
	 * the query's memory, before the first batch is sorted into its order.
	 * A partition's are its turn's (lion_join_part_open()).
	 */
	if (st->joinpart == NULL)
	{
		LionClauseState *jcl = &st->clause[st->joinclause];

		if (!st->joincollected)
			lion_join_collect(st);
		if (st->wheremissing)
		{
			st->done = true;
			return NULL;
		}
		if (!st->joinwalkbegun)
		{
			MemoryContext oldcxt =
				MemoryContextSwitchTo(st->css.ss.ps.state->es_query_cxt);

			lion_lookup_walk_begin(&st->joinwalker, jcl->idx, jcl->idxcol,
								   jcl->valtype);
			MemoryContextSwitchTo(oldcxt);
			st->joinwalkbegun = true;
		}
	}
	if (st->joinunique && !st->joinsortdone)
		lion_join_sort_keys(st);

	for (;;)
	{
		CHECK_FOR_INTERRUPTS();
		if (fg->rowpos < fg->nrows)
		{
			LionJoinGroupRow *r = &fg->rows[fg->rowpos++];

			lion_join_batch_row(st, &st->joinbatch[r->ent]);
			return lion_emit_tuple(st, r->key, r->isnull, (Datum) 0, true,
								   r->count);
		}

		/* every row of the last turn has gone up */
		fg->nrows = 0;
		fg->rowpos = 0;
		fg->rowcap = 0;
		fg->rows = NULL;
		MemoryContextReset(fg->rowcxt);

		if (fg->turn >= nturn || st->joinbatchn == 0)
		{
			if (!lion_join_fill_batch(st))
			{
				lion_join_batch_reset(st);
				st->childslot = NULL;
				st->done = true;
				return NULL;
			}
			fg->turn = 0;
			memset(&st->joinorder, 0, sizeof(st->joinorder));
		}
		lion_join_group_turn(st, (st->joinpart != NULL) ? fg->turn : -1);
		fg->turn++;
	}
}

/*
 * The FK-side join's next row (DESIGN.md §27): one PARTIAL row per dimension
 * row with fact rows - its dimension columns and its count - which core's
 * Finalize Agg above groups by the dimension columns and adds up, the join's
 * count for each group being a sum over the group's dimension rows; or, for
 * joinrows, a row core's plain Agg aggregates as it would the join's.
 *
 * SUMMED (joinsum, LION_JOINFLAG_SUM): when the target list is counts alone -
 * `SELECT count(*) FROM fact JOIN dim ...` and its semi, anti and distinct-key
 * forms, with no dimension column to group by or to print - the rows'
 * partial counts differ in nothing the Finalize Agg looks at but the count,
 * and it adds them up.  So the node adds them up itself and hands up ONE
 * partial row, their sum, per run of each participant: exactly what a
 * Partial Aggregate below a Gather hands up.  Every dimension row that used
 * to go up alone paid for it - its projection, the return through the
 * executor, a pause that let the walk's leaf go (read again for the next
 * key), the Finalize Agg's transition, and in a parallel plan a trip through
 * the Gather's tuple queue into the leader, which reads every worker's rows
 * alone - and none of that was the key's own work.  An empty join hands up
 * no row, as a join whose every row has count 0 did: a plain Finalize Agg
 * answers 0 for it, and a GROUP BY folded to constants has no group, as core
 * forms none over an empty input.
 */
TupleTableSlot *
lion_next_join_row(LionCountScanState *st)
{
	LionClauseState *jcl = &st->clause[st->joinclause];
	bool		anti = (st->jointype == LION_JOIN_ANTI);
	bool		walked;
	int64		count;
	MemoryContext oldcxt;

	switch (st->mode)
	{
		case LION_MODE_JOIN_FACTGROUP:
			/* grouped by a fact column: each key counted once per group */
			Assert(st->fg != NULL);
			return lion_next_join_group(st);
		case LION_MODE_JOIN:
			Assert(st->fg == NULL);
			break;
		default:
			elog(ERROR, "LionCount: mode %d is no join", (int) st->mode);
	}

	/* a partitioned fact table: every batch to every partition in turn */
	if (st->joinpart != NULL)
	{
		if (st->joinunique && !st->joinsortdone)
			lion_join_sort_keys(st);
		if (st->joinsum)
		{
			int64		total = 0;

			while (lion_join_next_parts(st, &count))
				total += count;
			st->done = true;
			st->childslot = NULL;
			if (total == 0)
				return NULL;
			return lion_emit_tuple(st, (Datum) 0, true, (Datum) 0, true,
								   total);
		}
		if (!lion_join_next_parts(st, &count))
		{
			st->done = true;
			return NULL;
		}
		return lion_emit_tuple(st, (Datum) 0, true, (Datum) 0, true, count);
	}

	/*
	 * The run's first row: the fact filters collected where the plan says,
	 * and the way the child is read decided, for the whole run (joinbegun,
	 * joinwalked).  A copy of the filters made part way through that selects
	 * nothing (lion_join_maybe_switch()) does not change it: each way sees to
	 * that itself, the rows of a batch tested whole still going up.
	 */
	if (!st->joinbegun)
	{
		if (!st->joincollected)
			lion_join_collect(st);

		/*
		 * A fact clause that selects nothing leaves no dimension row a count
		 * - and makes every one of them a row of an anti join, which the
		 * loop a row at a time emits without a lookup.
		 */
		if (st->wheremissing && !anti)
		{
			st->done = true;
			return NULL;
		}

		if (st->joinunique && !st->joinsortdone)
			lion_join_sort_keys(st);

		/*
		 * The walk of the fk index's directory, begun once for the node in
		 * its query memory, which resolves the key's probe once for all the
		 * lookups (lion_lookup_walk_begin()) - in key order when the plan
		 * asks for it and the keys can be sorted into it, and otherwise a
		 * descent per key.  Keys the directory cannot be sorted by - a
		 * cross-type key whose family has no ordering of its own for that
		 * type (lion_probe_init()) - are looked up a row at a time, as they
		 * would be without the walk.
		 */
		if (!st->wheremissing)
		{
			if (!st->joinwalkbegun)
			{
				oldcxt = MemoryContextSwitchTo(st->css.ss.ps.state->es_query_cxt);
				lion_lookup_walk_begin(&st->joinwalker, jcl->idx, jcl->idxcol,
									   jcl->valtype);
				MemoryContextSwitchTo(oldcxt);
				st->joinwalkbegun = true;
			}
			st->joinwalked = (st->joinwalk &&
							  lion_lookup_walk_ordered(&st->joinwalker));
		}
		st->joinbegun = true;
	}
	walked = st->joinwalked;

	if (st->joinsum)
	{
		int64		total = 0;

		while (walked ? lion_join_next_walked(st, &count) :
			   lion_join_next_row(st, &count))
			total += count;
		st->done = true;
		st->childslot = NULL;
		if (total == 0)
			return NULL;
		return lion_emit_tuple(st, (Datum) 0, true, (Datum) 0, true, total);
	}

	/*
	 * A semi or anti join path tests a walked batch whole before its rows go
	 * up, in the child's order where the path claims it (DESIGN.md §27, "The
	 * semi and anti join as a join path"); a row at a time, its rows are in
	 * the child's order anyway.
	 */
	if (!(walked ? (st->joinouter ? lion_join_next_parts(st, &count) :
					lion_join_next_walked(st, &count)) :
		  lion_join_next_row(st, &count)))
	{
		st->done = true;
		return NULL;
	}
	return lion_emit_tuple(st, (Datum) 0, true, (Datum) 0, true, count);
}
