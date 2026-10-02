/*-------------------------------------------------------------------------
 *
 * lion_plan_fkjoin_cost.c
 *		The cost model of the FK-side join (DESIGN.md §27).
 *
 * Part of the LionCount custom scan: lion_customscan.h describes the node
 * and declares what its files share.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "lion_customscan.h"

/*
 * The fk column's distinct keys (DESIGN.md §27).  A key's posting set holds
 * every row of the table with that key, not the ones the fact filters leave,
 * so this is the column's n_distinct over the whole table, taken as
 * lion_range_entries() takes it.  estimate_num_groups() scales it down to the
 * rows the relation's own clauses leave, which for an fk of a few rows per key
 * under a filter of a few percent made every key eight times as large as it
 * is (the 2026-09-27 benchmark: 21.9 rows a key where there are 2.75).
 */
double
lion_fkjoin_fk_ndistinct(PlannerInfo *root, RelOptInfo *rel, Var *fkvar)
{
	VariableStatData vardata;
	bool		isdefault;
	double		nd;

	examine_variable(root, (Node *) fkvar, rel->relid, &vardata);
	nd = get_variable_numdistinct(&vardata, &isdefault);
	ReleaseVariableStats(vardata);
	return Max(Min(nd, Max(rel->tuples, 1.0)), 1.0);
}

/*
 * How many participants a parallel plan's rows are divided among: core's
 * get_parallel_divisor() (costsize.c), which is static there.  The leader
 * takes part less the more workers it has to serve.
 */
double
lion_parallel_divisor(int workers)
{
	double		divisor = workers;

	if (parallel_leader_participation)
	{
		double		leader = 1.0 - (0.3 * workers);

		if (leader > 0)
			divisor += leader;
	}
	return divisor;
}

/*
 * The directory pages of the FK-side join's lookups when the child's rows are
 * looked up in key order (DESIGN.md §27, "Lookups in key order"): `rows` of
 * them, over a key column of `leaves` leaves in a directory `height` levels
 * above them, each row taking `rowbytes` of a batch.  From what the walk does
 * (lion_lookup_walk_find()):
 *
 *	- the rows are sorted in batches of what work_mem holds, and the first key
 *	  of each batch is a descent, height + 1 pages;
 *	- every row is the visit of its key's leaf, one page, and its place in the
 *	  batch, LION_FKJOIN_BATCH_ROW_COST;
 *	- the keys of a batch lie `leaves / keys` leaves apart on average.  While
 *	  that is at most the height, the walk steps over them, a page each; past
 *	  it a key descends, which costs the height of the directory over and
 *	  above its leaf.
 *
 * Against a descent per row, height + 1 pages, the walk is cheaper only where
 * the height is at least two and a batch holds more keys than there are
 * leaves between them: a directory of height 1 descends through the root
 * alone, which a row's place in the batch costs as much as.
 */
static Cost
lion_cost_fkjoin_walk(double rows, double leaves, double height,
					  double rowbytes)
{
	double		perbatch = Max(floor((double) work_mem * 1024.0 /
									 Max(rowbytes, 1.0)), 1.0);
	double		batches = ceil(rows / perbatch);
	double		keys = rows / Max(batches, 1.0);
	double		apart = leaves / Max(keys, 1.0);

	return batches * (height + 1.0) * LION_DESCENT_COST +
		rows * (Min(apart, height) * LION_DESCENT_COST +
				LION_FKJOIN_LOOKUP_COST + LION_FKJOIN_BATCH_ROW_COST);
}

/*
 * What one probe of a fact filter's set of `members` rows costs a count of the
 * FK-side join that probes it, the key's set lying in `cfk` containers
 * (DESIGN.md §27, "The per-key terms, refitted").  A set small enough for the
 * counts to copy into memory on its second use (lion_posting_set_materialize()
 * and LION_MATERIALIZE_MAX_*) is looked up there, LION_MEMORY_PROBE_COST.  Any
 * other is sought on its posting tree, LION_FKJOIN_PROBE_COST and the pages
 * the seek reads: a count's first probe descends the tree from its root,
 * where a NOPIN set's cursor starts, and a probe after it steps right while
 * the key's containers are close together - height + 2 pages a probe where
 * they lie eight container keys apart or more, a share of that where they are
 * closer.  That is what the counters of the refit's probing counts come to:
 * 2.9 to 3.7 posting pages a probe over one to ten containers a key, 0.34 to
 * 0.68 over 100 or all of the heap's 169 container keys.
 */
static Cost
lion_fkjoin_probe_one(double heap_pages, double members, double cfk)
{
	double		ckeys = Max(heap_pages / LION_BLOCKS_PER_CONTAINER, 1.0);
	double		containers = lion_containers_for(heap_pages, Max(members, 1.0));
	double		bytes = containers *
		(LION_CONTAINER_HDRSZ + Min(2.0 * Max(members, 1.0) / containers,
									(double) LION_BITSET_BYTES));
	double		leaves = Max(bytes / (double) LION_PAGE_CAPACITY, 1.0);

	if (bytes <= (double) LION_MATERIALIZE_MAX_BYTES &&
		bytes <= (double) work_mem * 1024.0)
		return LION_MEMORY_PROBE_COST;
	return LION_FKJOIN_PROBE_COST +
		(lion_posting_height(leaves) + 2.0) *
		Min(1.0, ckeys / (8.0 * Max(cfk, 1.0))) * LION_FKJOIN_PROBE_PAGE_COST;
}

/*
 * Cost the FK-side join (DESIGN.md §27) over the fact relation `rel`, for
 * `dimrows` dimension rows, `found` of which have an entry in the fk index;
 * the child plan's own cost is the caller's to add.  A key without an entry
 * costs its descent and nothing more.  Every caller but the forward semi join
 * over a non-unique key takes every row to find one (found = dimrows), which
 * is what an fk into a dimension key is expected to do.
 *
 * What the node does per dimension row: one lookup of the key in the fk
 * index - a directory descent, whose leaf is charged at lion_heap_page_cost()'s
 * interpolated cost over as many distinct leaves as the lookups can touch, or
 * a step of a walk of the leaves in key order where that is cheaper (*walk,
 * lion_cost_fkjoin_walk(); `rowbytes` is what one child row takes of a batch)
 * - then one count of that key's set ANDed with the fact filters: a merge set up
 * and torn down (LION_FKJOIN_COUNT_COST), the set's containers at §10's two
 * cpu_operator_cost each, and the set's chain pages, which are none at all
 * when the fk entries are INLINE.  The rows the node hands up are the
 * caller's to charge (LION_FKJOIN_ROW_COST), which knows how many there are
 * (lion_add_fkjoin_paths()).  A semi or anti join's
 * count is an existence test (`exists`), which reads the share of the set's
 * containers lion_exists_fraction() expects before it finds a visible row.
 *
 * The fact filters are read one of two ways, and *collect says which one is
 * cheaper:
 *
 *	- PROBED per count: every count seeks each filter source at the fk set's
 *	  container keys (lion_fkjoin_probe_one(): LION_FKJOIN_PROBE_COST a probe
 *	  and the posting pages it reads, or a probe in memory).  A source that
 *	  is a UNION - an IN list, an OR across columns, a multi-key clause whose
 *	  query is several keys - has each of its sets set up at every count
 *	  (LION_FKJOIN_SET_COST) and sought at the probed keys.  That was the
 *	  2026-09-23 review's finding for a thousand-value IN list, and the
 *	  2026-09-27 benchmark's for a tsquery of four lexemes, priced as one set
 *	  and chosen at half the hash join's cost for a plan sixteen times slower;
 *	  it was priced as the union's merge again at every count until the refit
 *	  of 2026-09-29 (DESIGN.md §27, "The per-key terms, refitted"), since the
 *	  counts probe a union rather than build it (§29.11);
 *	- or COLLECTED once (lion_sources_collect()): one merge of the filters
 *	  over all of their containers, as a single count of them would make, and
 *	  a private copy of what survives, which every count then looks up at its
 *	  own containers' keys (LION_FKJOIN_COPY_PROBE_COST a lookup and its AND).
 *	  Only when the copy is expected to fit in a hash join's memory
 *	  (get_hash_memory_limit(), which is also what the executor gives it) - a
 *	  container's members at two bytes each, a bitset's 4 kB at most - because
 *	  past it the executor gives up and probes.
 *
 *	  In a parallel plan (workers above zero) the participants collect ONE
 *	  copy together (DESIGN.md §27, "One copy per query"), so what it takes
 *	  to make is divided among them as the dimension rows are
 *	  (lion_parallel_divisor()), and it may take a hash table's memory for
 *	  each of them, a Parallel Hash's.  Its lookups stay each participant's:
 *	  every one of them locates the filters for itself.
 *
 * Either way the filters are located once for the whole scan, each one lookup
 * and one walk of its chain, as a single count prices them.
 *
 * And the heap the visibility map cannot vouch for, exactly as §10 prices it:
 * the candidates are the fact rows the dimension rows reach, which is their
 * keys' rows - `rows per fk value` each, and never more than the table - times
 * what the fact filters leave of them, on dirty pages fetched once per query.
 * Nothing here charges the whole fact heap: that is what the node exists not
 * to read.
 *
 * With `parts` the terms the two choices above are made between are handed
 * back as well, and the rest beside them (LionFkJoinCost): a partitioned fact
 * table is priced one leaf partition at a time, and the choices are then the
 * plan's, made over the sum of its partitions (lion_cost_fkjoin_path()).
 *
 * With `force` the two choices are not made but taken from *collect and *walk
 * (a copy is made only where there is one to make): the price of a part of
 * the rows - the first batch of a semi or anti join path, which its startup
 * cost holds - under the choices the whole run made.
 */
typedef struct LionFkJoinCost
{
	Cost		other;			/* everything but the terms below */
	Cost		descents;		/* the lookups, a descent each */
	Cost		walked;			/* ... or a walk of the leaves in key order */
	Cost		probed;			/* the fact filters, probed by every count */
	Cost		collected;		/* ... or collected once, and looked up */
	Cost		located;		/* the part of `other` locating the filters */
	double		copybytes;		/* what the collected copy is expected to take */
	bool		cancollect;		/* there is anything to collect at all */
} LionFkJoinCost;

static Cost
lion_cost_fkjoin_rel(PlannerInfo *root, RelOptInfo *rel, List *whereidx,
					 List *wherecol, Var *fkvar, int joinclause,
					 List *whereclauses, List *wherekinds, List *ors,
					 double dimrows, double found, bool exists,
					 double rowbytes, int workers, bool *collect, bool *walk,
					 bool force, LionFkJoinCost *parts)
{
	bool		wantcollect = force && *collect;
	bool		wantwalk = force && *walk;
	double		heap_pages = Max((double) rel->pages, 1.0);
	double		dirtyfrac = 1.0 - rel->allvisfrac;
	double		tuples = Max(rel->tuples, 1.0);
	double		wheresel = Min(Max(rel->rows, 1.0) / tuples, 1.0);
	IndexOptInfo *fkidx = (IndexOptInfo *) list_nth(whereidx, joinclause);
	AttrNumber	fkcol = (AttrNumber) list_nth_int(wherecol, joinclause);
	int			nclause = list_length(whereclauses);
	int		   *orgrp = lion_or_group_map(ors, nclause);
	int			nsrc = list_length(ors);
	double	   *srcsets = (double *) palloc0(sizeof(double) * (nclause + nsrc + 1));
	double	   *srcprobesets = (double *) palloc0(sizeof(double) * (nclause + nsrc + 1));
	double	   *srcmembers = (double *) palloc0(sizeof(double) * (nclause + nsrc + 1));
	double		probesets;
	double		nd;
	double		perkey;
	double		share;
	double		height = 0;
	double		dirpages;
	double		container_pages;
	double		lookups;
	Cost		descents;
	Cost		walked;
	double		cfk;
	double		readshare = 1.0;
	double		drive = -1.0;
	double		matched;
	double		recheck_tids;
	double		recheck_pages;
	double		filtered;
	double		copyckeys;
	double		copybytes;
	int			npositive = 0;
	Cost		run = 0;
	Cost		located = 0;
	Cost		probed = 0;
	Cost		collected = 0;
	int			ci = 0;
	int			sno;
	int		   *rangelead;
	ListCell   *lc1;
	ListCell   *lc2;
	ListCell   *lc3;
	ListCell   *lc4;

	dimrows = Max(dimrows, 1.0);
	found = Min(Max(found, 1.0), dimrows);
	*collect = false;
	*walk = false;

	/* How many rows one key of the fk column has, and in how many containers. */
	nd = lion_fkjoin_fk_ndistinct(root, rel, fkvar);
	perkey = tuples / nd;
	cfk = lion_containers_for(heap_pages, perkey);

	/*
	 * An existence test stops at the first container that shows a row the
	 * snapshot sees (§26), and a key's set holds `perkey x wheresel` of those
	 * among its cfk containers.
	 */
	if (exists)
		readshare = lion_exists_fraction(cfk, perkey * wheresel);

	/* ---- one lookup and one count per dimension row ---- */
	share = lion_index_column_share(root, rel, fkidx, fkcol);
	dirpages = lion_index_dir_pages(fkidx, &height);
	container_pages = Max(((double) fkidx->pages - 1.0 - dirpages -
						   lion_index_store_pages(fkidx, NULL)) *
						  lion_index_column_posting_share(root, rel, fkidx,
														  fkcol), 0.0);
	dirpages = Max(dirpages * share, 1.0);

	lookups = Min(dimrows, dirpages);
	run += lookups * lion_heap_page_cost(root, rel, lookups,
										 Max((double) fkidx->pages, 1.0));

	/*
	 * A descent per row, or the walk in key order when it reads fewer pages
	 * (DESIGN.md §27, "Lookups in key order").  The leaves themselves are
	 * charged above either way, each once: that is what the walk reads, and
	 * what the descents read too while the directory stays in the cache.
	 */
	descents = dimrows * (height + 1.0) * LION_DESCENT_COST;
	walked = lion_cost_fkjoin_walk(dimrows, dirpages, height, rowbytes);
	if (force ? wantwalk : walked < descents)
	{
		*walk = true;
		run += walked;
	}
	else
		run += descents;
	if (parts != NULL)
	{
		memset(parts, 0, sizeof(LionFkJoinCost));
		parts->descents = descents;
		parts->walked = walked;
	}
	run += Min(found * container_pages / nd, container_pages) * seq_page_cost;

	/* ---- the fact filters: located once, each a source of every count ---- */
	rangelead = lion_rangesrc_leaders(whereidx, wherecol, wherekinds,
									  ors, nclause);
	forfour(lc1, whereidx, lc2, whereclauses, lc3, wherekinds,
			lc4, wherecol)
	{
		IndexOptInfo *idx = (IndexOptInfo *) lfirst(lc1);
		Node	   *clause = (Node *) lfirst(lc2);
		int			kind = lfirst_int(lc3);
		Selectivity sel;
		double		nkeys = 1.0;
		double		cshare;
		double		cdir;
		double		cheight = 0;
		double		cpages;

		if (ci == joinclause || !LION_CLAUSE_IS_POSITIVE(kind))
		{
			ci++;
			continue;
		}

		/*
		 * A range taken as a source (DESIGN.md §32): collected once, by the
		 * first of its bounds, and read from memory by every count - or walked
		 * by each, when it is too large to collect (lion_cost_range_source()).
		 */
		if (kind == LION_CLAUSE_RANGESRC)
		{
			List	   *bounds = NIL;
			int			j;

			if (rangelead[ci] != ci)
			{
				ci++;
				continue;
			}
			for (j = ci; j < nclause; j++)
				if (rangelead[j] == ci)
					bounds = lappend(bounds, list_nth(whereclauses, j));
			sel = lion_rel_clauses_selectivity(root, rel, bounds);

			sno = (orgrp[ci] >= 0) ? orgrp[ci] : nsrc + ci;
			srcsets[sno] += 1.0;
			srcmembers[sno] += tuples * sel;
			located += lion_cost_range_source(root, rel, idx,
											  (AttrNumber) lfirst_int(lc4),
											  bounds, sel, dimrows,
											  orgrp[ci] >= 0);
			list_free(bounds);
			ci++;
			continue;
		}

		sel = clause_selectivity(root, clause, 0, JOIN_INNER, NULL);
		probesets = 0.0;
		if (IsA(clause, ScalarArrayOpExpr))
		{
			ScalarArrayOpExpr *saop = (ScalarArrayOpExpr *) clause;
			Node	   *array = lion_strip((Node *) lsecond(saop->args));

#if PG_VERSION_NUM >= 170000
			nkeys = Max(estimate_array_length(root,
											  (Node *) lsecond(saop->args)),
						1.0);
#else
			nkeys = Max(estimate_array_length((Node *) lsecond(saop->args)),
						1.0);
#endif

			/*
			 * An array whose length the planner cannot see - a parameter; an
			 * expression comes here as its plan-time estimate
			 * (lion_analyze_leaf()) - is located and collected once for
			 * whatever it turns out to hold, and priced once at
			 * estimate_array_length()'s guess.  But a count that PROBES the
			 * filters builds its union again per dimension row, so there it
			 * is priced as the longest list a literal may be: underpricing a
			 * scan's one-off work costs a little, underpricing a dimension
			 * row's costs that many times over (the 2026-09-23 review's
			 * 43,000 values over 20,000 dimension rows ran for minutes).
			 */
			if (array == NULL ||
				(!IsA(array, Const) && !IsA(array, ArrayExpr)))
				probesets = Max(nkeys, (double) LION_MAX_ARRAY_ELEMS);
		}
		else if (kind == LION_CLAUSE_MULTI)
			nkeys = lion_multikey_nkeys(idx, (AttrNumber) lfirst_int(lc4),
										clause);
		if (probesets <= 0.0)
			probesets = nkeys;

		/*
		 * Which source of the AND the clause's sets belong to: its OR's, or
		 * one of its own (numbered after the ORs).  A multi-key clause's sets
		 * are the lexemes or elements its query combines, each of them at
		 * least as frequent as the clause itself where they are ORed: they
		 * are priced as nkeys sets holding the clause's rows nkeys times
		 * over, which is a floor.
		 */
		sno = (orgrp[ci] >= 0) ? orgrp[ci] : nsrc + ci;
		srcsets[sno] += nkeys;
		srcprobesets[sno] += probesets;
		srcmembers[sno] += tuples * ((kind == LION_CLAUSE_MULTI) ?
									 Min(sel * nkeys, 1.0) : sel);
		ci++;

		/* Located once: a lookup per set, and the walk of their chains. */
		cshare = lion_index_column_share(root, rel, idx,
										 (AttrNumber) lfirst_int(lc4));
		cdir = lion_index_dir_pages(idx, &cheight);
		cpages = Max(((double) idx->pages - 1.0 - cdir -
					  lion_index_store_pages(idx, NULL)) *
					 lion_index_column_posting_share(root, rel, idx,
													 (AttrNumber) lfirst_int(lc4)),
					 0.0);

		located += Min(nkeys, Max(cdir * cshare, 1.0)) * random_page_cost +
			nkeys * (cheight + 1.0) * LION_DESCENT_COST;
		located += Max(Min(nkeys, cpages), cpages * sel) * seq_page_cost;
	}
	run += located;

	/*
	 * PROBED: every count seeks each source at the container keys the fk set
	 * has, and a seek finds a container only where the source has one.  A
	 * union source's sets are each set up and sought at every count.
	 *
	 * COLLECTED: the same sources merged once over all of their containers -
	 * driven by the sparsest, the others probed at its keys, a union's image
	 * built at each of its keys once - and the survivors copied.
	 */
	for (sno = 0; sno < nclause + nsrc; sno++)
	{
		double		call;
		double		cs;

		if (srcsets[sno] <= 0.0)
			continue;
		npositive++;
		call = lion_containers_for(heap_pages, srcmembers[sno]);
		cs = Min(cfk, call);

		/*
		 * A union source - an IN list, an OR, a multi-key query of several
		 * keys - is met at the probed keys with its sets sought there, each
		 * a seek of its posting tree, no more of them than its sets have
		 * containers (lion_merge_cpu_cost_sets()'s sought union); since
		 * unions are probed (§29.11) a count builds none of them for the
		 * few rows of a key's container, and what it pays besides the seeks
		 * is each set's cursor set up (LION_FKJOIN_SET_COST).  It used to
		 * be charged the union's merge, lion_merge_ops() prorated to the
		 * probed keys, at 5 to 7 times what the counts took (DESIGN.md §27,
		 * "The per-key terms, refitted").
		 */
		{
			double		setm = srcmembers[sno] / Max(srcsets[sno], 1.0);
			double		setc = lion_containers_for(heap_pages, setm);
			double		seeks = cs;

			if (srcprobesets[sno] > 1.0)
			{
				seeks = Min(cs * srcprobesets[sno],
							Max(srcprobesets[sno] * setc, cs));
				probed += found * srcprobesets[sno] * LION_FKJOIN_SET_COST;
			}
			probed += found * seeks * readshare *
				lion_fkjoin_probe_one(heap_pages, setm, cfk);
		}

		/*
		 * Collected, a union is read whole once and built at each of its
		 * keys: its sets' containers read, and the union's image at each key
		 * with its members set in it (LION_UNION_KEY_COST,
		 * LION_UNION_MEMBER_COST, as the leapfrog builds a driving union);
		 * not lion_merge_ops()'s k-way comparisons, which the image has
		 * replaced.
		 */
		if (srcsets[sno] > 1.0)
			collected += srcsets[sno] * LION_FKJOIN_SET_COST +
				srcsets[sno] * lion_containers_for(heap_pages,
												   srcmembers[sno] / srcsets[sno]) *
				LION_FKJOIN_COLLECT_CONTAINER_COST +
				call * LION_UNION_KEY_COST +
				srcmembers[sno] * LION_UNION_MEMBER_COST;
		if (drive < 0.0 || call < drive)
			drive = call;
	}
	pfree(orgrp);
	pfree(rangelead);
	pfree(srcsets);
	pfree(srcprobesets);
	pfree(srcmembers);

	/*
	 * What the collected copy holds and how large it is: the filters' rows, in
	 * as many containers as those rows can occupy, each at most a bitset.
	 */
	filtered = tuples * wheresel;
	copyckeys = lion_containers_for(heap_pages, filtered);
	copybytes = copyckeys * (LION_CONTAINER_HDRSZ + sizeof(LionContainer *) +
							 Min(2.0 * filtered / copyckeys,
								 (double) LION_BITSET_BYTES));
	probed += found * LION_FKJOIN_COUNT_COST;
	if (npositive > 0)
	{
		collected += drive * LION_FKJOIN_COLLECT_CONTAINER_COST +
			drive * (npositive - 1) * LION_FKJOIN_PROBE_COST +
			copyckeys * LION_FKJOIN_COPY_CONTAINER_COST;

		/* ... made once, by all the participants of a parallel plan */
		if (workers > 0)
			collected /= lion_parallel_divisor(workers);
		collected += found * (LION_FKJOIN_COPY_COUNT_COST +
							  Min(cfk, copyckeys) * readshare *
							  (LION_FKJOIN_COPY_PROBE_COST +
							   perkey / cfk * LION_FKJOIN_COPY_MEMBER_COST));

		/*
		 * A hot standby never makes the copy (lion_join_collect()), so there
		 * every count probes, and is priced so (2026-09-28 review): a plan
		 * made there as if the copy would be made chose the node where each
		 * dimension row then built the filters' unions again.
		 */
		if (force ? wantcollect :
			(copybytes <= (double) get_hash_memory_limit() * (workers + 1) &&
			 collected < probed && !RecoveryInProgress()))
			*collect = true;
		if (parts != NULL)
		{
			parts->cancollect = true;
			parts->copybytes = copybytes;
		}
	}
	run += *collect ? collected : probed;
	if (parts != NULL)
	{
		parts->probed = probed;
		parts->collected = collected;
		parts->located = located;
	}

	/*
	 * The fk set's containers at every count, read and counted as any count's
	 * are (lion_merge_cpu_cost(): LION_CONTAINER_COST, and LION_MEMBER_COST a
	 * member of them).
	 */
	run += found * cfk * readshare *
		(LION_CONTAINER_COST + LION_MEMBER_COST * Min(perkey / cfk,
													   LION_MEMBER_CAP));

	/* ---- the heap the visibility map cannot vouch for ---- */
	matched = Min(found * perkey, tuples) * wheresel * readshare;
	recheck_tids = matched * dirtyfrac;
	recheck_pages = Min(recheck_tids, heap_pages * dirtyfrac);
	run += recheck_pages * lion_heap_page_cost(root, rel, recheck_pages,
											   heap_pages);
	run += recheck_tids * LION_RECHECK_TID_COST;

	if (parts != NULL)
		parts->other = run - (*walk ? walked : descents) -
			(*collect ? collected : probed);
	return run;
}

/*
 * What the FK-side join does over the fact relation `rel` (DESIGN.md §27),
 * for `dimrows` dimension rows `found` of which have fact rows: its lookups
 * and counts (lion_cost_fkjoin_rel()), and the heap recheck of a multi-key
 * fact filter the node only has at run time (DESIGN.md §17, "A query known
 * only at run time") - the fact rows the keys reach, each count reading its
 * own.  *collect and *walk are the plan's two choices - or, with `force`,
 * the choices to price, made already (lion_cost_fkjoin_rel()).
 *
 * A partitioned fact table (DESIGN.md §27, "A partitioned fact table") is
 * every key looked up in every leaf partition, each with its own fk index,
 * its own fact filters - less the ones its bounds imply (§16) - and its own
 * copy of them, so each leaf is priced as a table of its own and the costs
 * add up.  A key finds rows in a leaf as often as the fk's distinct values
 * are there: the share of the parent's that the leaf's own statistics count.
 * The choices are the plan's, one for all the leaves, made over the sums:
 * the keys are read a batch at a time whatever is chosen, since each batch
 * is taken to every leaf in turn, so a batch's place costs a row once and
 * not once a leaf; and the leaves' copies of the fact filters are all held
 * for the whole scan, so together they have to fit where one would.  A plan
 * that probes the filters locates each leaf's again at every turn - a leaf
 * keeps nothing located from one batch to the next - so their locating is
 * charged once a batch, where a plain table's is charged once.
 *
 * Not every key goes to every leaf (lion_join_count_parts(), 2026-09-29): a
 * leaf whose fact filters select nothing has no turn - its filters are
 * located, and no key is looked up in it - and an existence test looks a key
 * up only in the leaves before the first that matches it.  So each leaf is
 * priced for the keys it looks up: the dimension rows times the chance that
 * it has a turn (lion_leaf_turn_share()) and, for an existence test, the
 * share of the keys no earlier leaf has matched - a key found in a leaf
 * matching there unless none of its rows passes the leaf's filters, which
 * for its rows there spread as the leaf's estimate says is exp(-rows).  Every
 * key used to be priced in every leaf: an EXISTS over eight leaves, two of
 * which its time range left empty, at 605,000 lookups and counts where the
 * node made 188,000 (DESIGN.md §27, "A partitioned fact table").
 */
Cost
lion_cost_fkjoin_path(PlannerInfo *root, RelOptInfo *rel, List *targets,
					  Var *fkvar, int joinclause, List *whereclauses,
					  List *wherekinds, List *ors, double dimrows, double found,
					  bool exists, double rowbytes, int workers, bool *collect,
					  bool *walk, bool force)
{
	LionCountTarget *first = (LionCountTarget *) linitial(targets);
	bool		wantcollect = force && *collect;
	bool		wantwalk = force && *walk;
	double		ndall;
	Cost		other = 0;
	Cost		descents = 0;
	Cost		walked = 0;
	Cost		probed = 0;
	Cost		collected = 0;
	Cost		located = 0;
	double		copybytes = 0;
	double		batches;
	double		unmatched = 1.0;
	double		looked = 0.0;
	Cost		run;
	ListCell   *lc;

	if (list_length(targets) == 1 && first->rel == rel)
	{
		double		tuples = Max(rel->tuples, 1.0);
		double		reach = Min(found * tuples /
								lion_fkjoin_fk_ndistinct(root, rel, fkvar),
								tuples);
		double		groups = lion_fact_groups(root, first);
		LionFkJoinCost parts;

		run = lion_cost_fkjoin_rel(root, rel, first->whereidx,
								   first->wherecol, fkvar, joinclause,
								   whereclauses, wherekinds, ors, dimrows,
								   found, exists, rowbytes, workers,
								   collect, walk, force, &parts);

		/*
		 * A fact column grouped by counts each key once per group
		 * ("Grouped by a fact column"): the counts, against the filters or
		 * their copy, that many times over - the key looked up once.
		 */
		if (groups > 1.0)
			run += (groups - 1.0) * (*collect ? parts.collected : parts.probed);
		run += lion_cost_recheck(root, rel, first->whereidx, first->wherecol,
								 whereclauses, wherekinds, ors,
								 reach * Min(Max(rel->rows, 1.0) / tuples, 1.0),
								 found, reach);
		return run;
	}

	ndall = lion_fkjoin_fk_ndistinct(root, rel, fkvar);
	foreach(lc, targets)
	{
		LionCountTarget *t = (LionCountTarget *) lfirst(lc);
		Var		   *leafvar = lion_child_var(root, t->rel->relid,
											 fkvar->varattno);
		double		tuples = Max(t->rel->tuples, 1.0);
		double		nd;
		double		tfound;
		double		tdim;
		double		turn;
		double		reach;
		int			tjoin = joinclause;
		List	   *tidx;
		List	   *tcol;
		List	   *tclauses;
		List	   *tkinds;
		List	   *tors;
		LionFkJoinCost parts;
		bool		tcollect;
		bool		twalk;
		double		tgroups;

		if (leafvar == NULL)
			leafvar = fkvar;	/* lion_collect_targets() found the column */
		lion_target_lists(t, whereclauses, wherekinds, ors, &tidx, &tcol,
						  &tclauses, &tkinds, &tors, &tjoin);
		nd = lion_fkjoin_fk_ndistinct(root, t->rel, leafvar);

		/* the keys this leaf's turn looks up, and those it finds */
		turn = lion_leaf_turn_share(root, t->rel);
		tdim = Max(dimrows * unmatched * turn, 1.0);
		tfound = clamp_row_est(found * unmatched * turn *
							   Min(nd / ndall, 1.0));
		looked += tdim;
		if (exists)
			unmatched *= 1.0 - turn * Min(nd / ndall, 1.0) *
				(1.0 - exp(-(tuples / nd) *
						   Min(Max(t->rel->rows, 1.0) / tuples, 1.0)));

		(void) lion_cost_fkjoin_rel(root, t->rel, tidx, tcol, leafvar, tjoin,
									tclauses, tkinds, tors, tdim, tfound,
									exists, rowbytes, workers, &tcollect,
									&twalk, false, &parts);

		/* ... and a fact column's groups count each key once per group */
		tgroups = lion_fact_groups(root, t);
		other += parts.other;
		descents += parts.descents;
		walked += parts.walked;
		probed += parts.probed * tgroups;
		located += parts.located;
		if (parts.cancollect)
		{
			collected += parts.collected * tgroups;
			copybytes += parts.copybytes;
		}
		else
			collected += parts.probed * tgroups;

		reach = Min(tfound * tuples / nd, tuples);
		other += lion_cost_recheck(root, t->rel, tidx, tcol, tclauses, tkinds,
								   tors,
								   reach * Min(Max(t->rel->rows, 1.0) / tuples,
											   1.0),
								   tfound, reach);
	}

	/*
	 * a batch's place in it is a row's once, however many leaves look the
	 * row up - each leaf's walk charged it for those it does
	 */
	walked += (dimrows - looked) * LION_FKJOIN_BATCH_ROW_COST;
	descents += dimrows * LION_FKJOIN_BATCH_ROW_COST;

	/* each batch after the first locates every leaf's filters again */
	batches = ceil(Max(dimrows, 1.0) /
				   Max(floor((double) work_mem * 1024.0 / Max(rowbytes, 1.0)),
					   1.0));
	probed += (batches - 1.0) * located;

	if (force)
	{
		*walk = wantwalk;
		*collect = (wantcollect && copybytes > 0);
	}
	else
	{
		*walk = (walked < descents);
		*collect = (copybytes > 0 &&
					copybytes <= (double) get_hash_memory_limit() * (workers + 1) &&
					collected < probed && !RecoveryInProgress());
	}
	return other + (*walk ? walked : descents) +
		(*collect ? collected : probed);
}
