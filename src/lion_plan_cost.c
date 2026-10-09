/*-------------------------------------------------------------------------
 *
 * lion_plan_cost.c
 *		The cost model of a LionCount path.
 *
 * Part of the LionCount custom scan: lion_customscan.h describes the node
 * and declares what its files share.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "lion_customscan.h"

static LionQueryMode lion_multikey_cost_mode_ex(IndexOptInfo *idx,
												AttrNumber col, Node *clause,
												double *nkeys, bool *isunion);
static LionQueryMode lion_multikey_cost_mode(IndexOptInfo *idx, AttrNumber col,
											 Node *clause, double *nkeys);
static Cost lion_range_recheck(PlannerInfo *root, RelOptInfo *rel,
							   double tids, double entries, double corr);

/*
 * Cost the pushdown (DESIGN.md section 10).
 *
 * Proportional rather than exact.  What a count actually reads:
 *
 *	- for each WHERE key: ONE DESCENT of the entry directory (height + 1
 *	  pages, DESIGN.md §21), then that key's own container chain, which was
 *	  written sequentially and is a fraction of the index's container pages
 *	  proportional to the clause selectivity.  An IN list is one lookup per
 *	  element (DESIGN.md §15), but its values are SORTED first and located in
 *	  one left-to-right walk, so a long list pays for the leaves it crosses
 *	  and not for a descent each;
 *	- for a GROUP BY: every page of the group index;
 *	- one O(1) step per container per participating source (the visibility map
 *	  is read per container);
 *	- the heap the visibility map cannot vouch for: the TIDs on blocks that
 *	  are not all-visible (pg_class.relallvisible via RelOptInfo.allvisfrac),
 *	  each resolved against the snapshot, on as many distinct blocks as there
 *	  can be - each of them fetched once per query.
 *
 * The directory pages are NOT charged wholesale: they would price a
 * single-key count on a small table above a sequential scan of the whole
 * table.
 *
 * It has to beat Agg-over-BitmapHeapScan when the pushdown really is cheaper
 * and lose when it is not; it is not meant to be comparable with core cost
 * estimates to the last decimal.
 *
 * A partitioned table is priced as the sum of its live leaf partitions, each
 * with its own pages / allvisfrac / rows and its own indexes (DESIGN.md §16).
 * numgroups is the parent's estimate throughout: a partition may hold rows of
 * every group.
 */
/*
 * The pages of the entry directory, and how deep it is (DESIGN.md §21).  Both
 * come off the meta page, which lion_get_state() has cached in rd_amcache, as
 * btcostestimate reads the tree height from btree's metapage; the directory
 * page count is maintained exactly by ambuild and by every split.
 */
double
lion_index_dir_pages(IndexOptInfo *idx, double *height)
{
	Relation	indexrel = index_open(idx->indexoid, AccessShareLock);
	LionMetaPageData meta;
	double		dirpages;

	lion_read_meta(indexrel, &meta);
	dirpages = (double) meta.dirpages;
	if (height != NULL)
		*height = (double) meta.height;

	index_close(indexrel, AccessShareLock);
	return Max(dirpages, 1.0);
}

/* A key column's n_distinct, or -1 for an expression column. */
static double
lion_index_column_nd(PlannerInfo *root, RelOptInfo *rel, IndexOptInfo *idx,
					 int i)
{
	RangeTblEntry *rte = root->simple_rte_array[rel->relid];
	AttrNumber	attno = idx->indexkeys[i];
	VariableStatData vardata;
	Var		   *var;
	double		nd;
	bool		isdefault;

	/* An expression column has no heap attribute to ask about. */
	if (attno <= 0)
		return -1.0;

	var = makeVar(rel->relid, attno, get_atttype(rte->relid, attno), -1,
				  get_typcollation(get_atttype(rte->relid, attno)), 0);
	examine_variable(root, (Node *) var, 0, &vardata);
	nd = get_variable_numdistinct(&vardata, &isdefault);
	ReleaseVariableStats(vardata);
	pfree(var);

	return Max(nd, 1.0);
}

/* Is the relation one whose statistics the shares below can ask about? */
static bool
lion_index_shares_known(PlannerInfo *root, RelOptInfo *rel, IndexOptInfo *idx)
{
	RangeTblEntry *rte;

	if (idx->nkeycolumns <= 1)
		return false;
	if (rel->relid == 0 || rel->relid >= (Index) root->simple_rel_array_size)
		return false;
	rte = root->simple_rte_array[rel->relid];
	return rte != NULL && rte->rtekind == RTE_RELATION;
}

/*
 * ONE KEY COLUMN's share of a multicolumn lion index (DESIGN.md §24).
 *
 * Everything the model prices an index by - `idx->pages` and
 * lion_index_dir_pages() - is PER RELATION, and a multicolumn index is one
 * relation holding n independent sets of entries.  Charging a column the whole
 * directory and the whole page count would price `WHERE b = 1` on `(a, b, c)`
 * as three times what the same query on a single-column index of `b` costs,
 * and the node would be refused for a query it answers exactly as fast.  §24
 * says a column's set should be priced "as a single-column index of that
 * column", so the page terms are scaled by this column's share of the
 * relation's entries.
 *
 * The planner cannot count a column's entries: the meta page carries the
 * directory's shape for the whole relation and nothing per column.  What it
 * does have is the HEAP column's n_distinct, which is the same order of
 * magnitude as that column's entry count (a scalar opclass makes one entry per
 * distinct value, plus the reserved ones), so the share is
 *
 *		n_distinct(this column) / sum of n_distinct over the index's columns
 *
 * taken through examine_variable()/get_variable_numdistinct(), the same pair
 * estimate_num_groups() uses, against a Var built from this relation's own
 * attribute numbers - which for a partition are the partition's (§16).
 *
 * THE LIMITATION, and it is a real one: n_distinct is not entries.  A
 * multi-key column (§17) has one entry per LEXEME and not per row value, so
 * its share is understated - usually far - and the scalar columns beside it
 * are charged for its directory.  A column with no statistics at all falls
 * back to DEFAULT_NUM_DISTINCT for that column alone, which makes the split
 * equal when NO column has statistics and biased when only some do.  Both
 * errors are bounded by the number of columns, which is why this correction is
 * worth making at all: without it the error is exactly that factor, always,
 * and always against the node.
 */
double
lion_index_column_share(PlannerInfo *root, RelOptInfo *rel, IndexOptInfo *idx,
					   AttrNumber col)
{
	double		total = 0;
	double		mine = 0;
	int			i;

	if (!lion_index_shares_known(root, rel, idx))
		return 1.0;

	for (i = 0; i < idx->nkeycolumns; i++)
	{
		double		nd = lion_index_column_nd(root, rel, idx, i);

		if (nd < 0.0)
			return 1.0 / (double) idx->nkeycolumns;
		total += nd;
		if (i == col - 1)
			mine = nd;
	}

	if (total <= 0.0 || mine <= 0.0)
		return 1.0 / (double) idx->nkeycolumns;

	return Min(mine / total, 1.0);
}

/*
 * ONE KEY COLUMN's share of a multicolumn lion index's CONTAINER pages
 * (DESIGN.md §24).  lion_index_column_share() splits the relation by entries,
 * which is right for the directory - a column has an entry per distinct
 * value - and wrong for the posting sets: every row is under one entry of
 * each scalar column, so a column's container pages follow its ROWS and what
 * they cost to store, not its entries.  Split by n_distinct, the three values
 * of a status column beside a column of 2,000 got 0.14% of a 110 MB index's
 * pages where they hold a tenth of it, and `status = 'val2'` - a million
 * rows over every container key - was priced at 16 for a 1 ms count.
 *
 * So each column is weighed by the bytes its sets take: nd entries of N / nd
 * rows each, which lie in Min(container keys, N / nd) containers of a header
 * each (16 bytes with its line pointer) and two bytes a member up to a
 * bitset's 4 kB, or about six bytes a row where the rows are too few a
 * container for one (a sparse segment, §13).  On that 110 MB index the
 * estimate comes to 114 MB, the 2,000-value column 44% of it.
 */
double
lion_index_column_posting_share(PlannerInfo *root, RelOptInfo *rel,
								IndexOptInfo *idx, AttrNumber col)
{
	double		tuples = Max(rel->tuples, 1.0);
	double		ckeys = Max((double) rel->pages / LION_BLOCKS_PER_CONTAINER, 1.0);
	double		total = 0;
	double		mine = 0;
	int			i;

	if (!lion_index_shares_known(root, rel, idx))
		return 1.0;

	for (i = 0; i < idx->nkeycolumns; i++)
	{
		double		nd = lion_index_column_nd(root, rel, idx, i);
		double		rows;
		double		conts;
		double		bytes;

		if (nd < 0.0)
			return 1.0 / (double) idx->nkeycolumns;
		rows = tuples / nd;
		conts = Min(ckeys, rows);
		if (rows / conts <= 2.0)
			bytes = nd * rows * 6.0;
		else
			bytes = nd * (conts * 16.0 +
						  Min(2.0 * rows, conts * (double) LION_BITSET_BYTES));
		total += bytes;
		if (i == col - 1)
			mine = bytes;
	}

	if (total <= 0.0 || mine <= 0.0)
		return 1.0 / (double) idx->nkeycolumns;

	return Min(mine / total, 1.0);
}

/*
 * Does this index order its entries by the KEY TYPE's own order (DESIGN.md
 * §21)?  Two things have to hold, and both are about what the planner is
 * allowed to conclude from the entry scan coming out in directory order:
 *
 *	- the index is ordered at all, i.e. its opclass has support function 4 (or
 *	  its key type has a default btree opclass to borrow one from);
 *	- and that ordering IS the key type's default btree ordering, because that
 *	  is the order an `ORDER BY col` asks for.  An opclass free to define its
 *	  own comparison is free to define a different one.
 *
 * The collation is not checked here: §10 already requires the index's
 * collation to equal the grouping column's, which is the same rule the
 * planner's IndexCollMatchesExprColl() applies to an index scan.
 */
bool
lion_index_orders_naturally(IndexOptInfo *idx, AttrNumber col)
{
	Relation	indexrel = index_open(idx->indexoid, AccessShareLock);
	LionState  *state = lion_index_column_state(indexrel, col);
	bool		ok = false;

	if (state->ordered)
	{
		TypeCacheEntry *typentry = lookup_type_cache(state->typid,
													 TYPECACHE_CMP_PROC);

		ok = OidIsValid(typentry->cmp_proc) &&
			typentry->cmp_proc == state->cmpproc.fn_oid;
	}

	index_close(indexrel, AccessShareLock);
	return ok;
}

/*
 * `var`'s n_distinct over the WHOLE table - a walk visits an entry whatever
 * the other clauses leave of it - which is how many entries a scalar column's
 * index has.  It is taken as examine_variable() gives it rather than through
 * estimate_num_groups(), which would scale it down by every clause of the
 * relation, a range included, and count the range twice.
 */
static double
lion_var_ndistinct(PlannerInfo *root, RelOptInfo *rel, Var *var)
{
	VariableStatData vardata;
	double		ndistinct;
	bool		isdefault;

	examine_variable(root, lion_vcol_unvar((Node *) var), rel->relid,
					 &vardata);
	ndistinct = get_variable_numdistinct(&vardata, &isdefault);
	ReleaseVariableStats(vardata);

	return Max(1.0, ndistinct);
}

/*
 * How many entries of `var`'s index a range walk visits (DESIGN.md §28): its
 * n_distinct times the range's own selectivity, at least one.
 */
double
lion_range_entries(PlannerInfo *root, RelOptInfo *rel, Var *var,
				  Selectivity sel)
{
	return Max(1.0, lion_var_ndistinct(root, rel, var) * sel);
}

/*
 * The correlation between `var`'s values and the heap's physical order, as
 * lioncostestimate() reads it for a plain scan (lion_var_heap_correlation(),
 * DESIGN.md §29.11): ANALYZE's number less the sum of the values' squared
 * frequencies, which is what it comes out at for values placed at random;
 * 0 when there is none.  Read raw, a column of a few values placed at random
 * looked as though each value's rows were packed on a share of the heap, and
 * the recheck of a count over one was priced on too few pages.
 */
static double
lion_var_correlation(PlannerInfo *root, RelOptInfo *rel, Var *var)
{
	return lion_var_heap_correlation(root, rel->relid,
									 lion_vcol_unvar((Node *) var));
}

/*
 * How many containers a posting set of `members` members can span: one per
 * LION_BLOCKS_PER_CONTAINER heap pages, and never more than one per member.
 */
double
lion_containers_for(double heap_pages, double members)
{
	return Max(1.0, Min(heap_pages / LION_BLOCKS_PER_CONTAINER, members));
}

/*
 * Does key column col of idx have summary posting sets (DESIGN.md §32)?  What
 * the index's meta page says, read through its cached state as a count reads
 * it; a column built without them has none until the next REINDEX.
 */
static bool
lion_index_col_summarized(IndexOptInfo *idx, AttrNumber col)
{
	Relation	indexrel;
	bool		summarized;

	indexrel = index_open(idx->indexoid, AccessShareLock);
	summarized = lion_index_column_state(indexrel, col)->summarized;
	index_close(indexrel, AccessShareLock);
	return summarized;
}

/*
 * ... and how many the rows of ONE KEY of index column `col` do lie in: as
 * many as they can when the column's values are placed at random, their
 * share of the heap's container keys when it is stored in value order, and
 * between the two by the correlation's square, as cost_index() interpolates
 * pages (lion_var_heap_correlation(); lion_cost_range_sum() does the same for
 * a range's entries).  A day of a table loaded in time order is one or two
 * containers, not one at every container key: priced as scattered, a GROUP
 * BY over 365 such days cost 4,650 for 0.6 ms.  An expression or a multi-key
 * column has no correlation to go by and is taken as scattered.
 */
static double
lion_key_containers(PlannerInfo *root, RelOptInfo *rel, IndexOptInfo *idx,
					AttrNumber col, double members)
{
	double		heap_pages = Max((double) rel->pages, 1.0);
	double		scattered = lion_containers_for(heap_pages, members);
	double		inorder;
	double		corr;
	AttrNumber	attno;
	RangeTblEntry *rte;
	Var		   *var;

	if (col < 1 || col > idx->nkeycolumns || scattered <= 1.0)
		return scattered;
	attno = idx->indexkeys[col - 1];
	if (attno <= 0 || rel->relid == 0 ||
		rel->relid >= (Index) root->simple_rel_array_size)
		return scattered;
	rte = root->simple_rte_array[rel->relid];
	if (rte == NULL || rte->rtekind != RTE_RELATION ||
		lion_opfamily_is_multikey(idx->opfamily[col - 1],
								  idx->opcintype[col - 1]))
		return scattered;

	var = makeVar(rel->relid, attno, get_atttype(rte->relid, attno), -1,
				  idx->indexcollations[col - 1], 0);
	corr = lion_var_correlation(root, rel, var);
	pfree(var);

	inorder = Max(1.0, members * (heap_pages / LION_BLOCKS_PER_CONTAINER) /
				  Max(rel->tuples, 1.0));
	return Max(1.0, scattered + (inorder - scattered) * corr * corr);
}

/*
 * The share of an intersection's work an EXISTENCE test does (DESIGN.md §26).
 *
 * The test stops at the first container that shows a visible row.  An
 * intersection expected to hold `survivors` rows spread over `containers`
 * containers has a row in about one container in containers/survivors, so the
 * test reads about containers/survivors + 1 of them - the whole thing when it
 * is expected to be empty, which is also when it has to be read to the end to
 * say so.  The same share applies to the recheck candidates the test queues,
 * because it rechecks one container at a time and stops with them.
 */
double
lion_exists_fraction(double containers, double survivors)
{
	containers = Max(containers, 1.0);
	if (survivors < 1.0)
		return 1.0;
	return Min(1.0, (containers / survivors + 1.0) / containers);
}

/*
 * How tall the posting tree of a set that occupies `leaves` container pages is
 * (DESIGN.md §22): 0 while it fits on one page, and one level for every
 * LION_POSTING_FANOUT pages above that.
 *
 * The planner cannot read it anywhere: the meta page carries the height of the
 * entry DIRECTORY, not of any one key's posting tree, and asking a key's own
 * root for it would be a page read per estimate.  So it is derived from the
 * shape the tree is built with - an internal page holds
 * LION_PAGE_CAPACITY / (MAXALIGN(sizeof(LionPostingPivot)) + sizeof(ItemIdData))
 * = 679 downlinks (lion_posting.c) - which is exact for a bulk-built tree and
 * an underestimate of at most one level for a tree grown by splits.
 */
#define LION_POSTING_FANOUT \
	((double) (LION_PAGE_CAPACITY / (MAXALIGN(LION_POSTING_PIVOT_SIZE) + \
									 sizeof(ItemIdData))))

double
lion_posting_height(double leaves)
{
	double		height = 0;

	for (leaves = Max(leaves, 1.0); leaves > 1.0; leaves /= LION_POSTING_FANOUT)
		height += 1.0;

	return height;
}

/*
 * How many pages of one posting tree that occupies `leaves` container pages
 * `probes` seeks into it read (DESIGN.md §22).
 *
 * A source that does not drive the leapfrog join is never walked: it is sought
 * to the container keys the driver produces, and one seek is a descent - one
 * internal page per level and the leaf the key lives on - or, when the key is
 * a page or two ahead, a step right, which the seek takes only while it is no
 * dearer than the descent it saves (`LION_POSTING_SEEK_STEPS`).  So a probe
 * costs `height + 1` pages and the source as a whole costs that many times the
 * probes, never more than its whole chain, which is what a walk reads.
 *
 * Measured on the benchmark's one-million-row `fact` (release build,
 * 2026-09-22), counting the node's buffer accesses: `c2 = 1` alone walks its
 * 151 container pages and touches 154 buffers; probed by `c20k = 77`, which
 * has a container at about 40 of the heap's 301 container keys, it touches 118
 * - a descent's worth per probe and a page or so of stepping - against the 100
 * this charges and the 151 a walk would.  Probed by `c1m = 12345`, which has
 * one container key, it touches 2.
 */
static double
lion_probed_pages(double leaves, double probes, double height)
{
	return Min(Max(leaves, 1.0), Max(probes, 0.0) * (height + 1.0));
}

/*
 * What the k-way union of `nkeys` posting sets holding `members` rows between
 * them costs, in cpu_operator_cost units (DESIGN.md §15).
 *
 * Two terms, because lion_ecursor_build() does two different things:
 *
 *	- a MIN-HEAP sift per container, log2(k) deep.  The heap holds the
 *	  sub-cursors, not the members, so this is counted per container and not
 *	  per row: a set of `members / nkeys` rows has that many containers at most,
 *	  and never more than the heap has container keys;
 *	- and the UNION of the containers standing at one key, which is a bitset
 *	  image once there are LION_OR_BITSET_MIN of them - a fixed pass over the
 *	  key's range, plus one bit set per member - and a pairwise fold below
 *	  that, which touches the members once per fold.
 *
 * Charging `members x log2(k)` for all of it, as if every row were compared
 * its way through the heap, asked 24750 of the 26481 cost units for `c200 IN
 * (1000 values)` at one million rows - a query the node answers in 6.3 ms
 * against the B-tree index-only scan's 68 - and refused it.
 */
static double
lion_merge_ops(double heap_pages, double members, double nkeys)
{
	double		containers = nkeys * lion_containers_for(heap_pages,
														members / nkeys);
	double		sifts = containers * log2(nkeys);

	if (nkeys >= (double) LION_OR_BITSET_MIN)
		return sifts + lion_containers_for(heap_pages, members) *
			LION_BITSET_WORDS + members;

	return sifts + members * log2(nkeys);
}

/*
 * What a merge that counts the AND of `nsrc` sources costs in CPU (DESIGN.md
 * §10, "The units"; §22, §25): members[i] rows lying in containers[i]
 * containers (a union's are its sets' together), of a table of `tuples` rows.
 *
 * The source with the fewest members drives: each of its containers is read
 * and counted (LION_CONTAINER_COST, and LION_MEMBER_COST a member, up to a
 * bitset's worth).  At each of its container keys the others are sought in
 * ascending order of members, a probe each (LION_PROBE_COST, or
 * LION_MEMORY_PROBE_COST for a source inmem[] says is a copy in memory) that
 * ANDs the running intersection's members with what it finds
 * (LION_AND_MEMBER_COST).
 * A key is abandoned as soon as the intersection empties (§25), so the k'th
 * source is sought only at the keys where the ones before it left a row: at
 * 1 - exp(-lambda) of them, lambda the rows the intersection is expected to
 * hold at a key when the columns are independent.  Without that the third
 * source of `c20k = 77 AND c200 = 17 AND c2 = 1` was charged a probe at each
 * of 219 keys, when c200 empties all but two of them, and the node was refused
 * at 777 against a BitmapAnd's 237 for 0.03 ms against 2.8.
 *
 * isect is what the intersection probe measured of the same AND: the factor
 * its rows are over (or under) the product of the sources' selectivities,
 * 1 where nothing was measured (DESIGN.md §29.11, "Correlated sets").  The
 * sources of a correlated filter do not empty the intersection as the
 * product says they do: twenty sets that hold a few thousand rows of six
 * million together, where the product says one, kept a row at every key the
 * driver produced, and the merge sought every source there - 27,365
 * containers for 1,374 keys, 73 seeks avoided.  The factor is spread over the
 * sources after the driver evenly, as each source's share of it (isect to the
 * power 1 / (nsrc - 1)), so that the intersection shrinks towards what the
 * probe found rather than to nothing.
 *
 * probes[i], when probes is not NULL, is set to how often source i is sought
 * (0 for the driver), for the pages those probes read.
 */
double
lion_merge_cpu_cost(int nsrc, const double *members, const double *containers,
					const bool *inmem, double tuples, double isect,
					double *probes)
{
	return lion_merge_cpu_cost_sets(nsrc, members, containers, NULL, NULL,
									NULL, inmem, tuples, isect, probes);
}

/*
 * ... with the UNIONS among the sources priced as the leapfrog meets them
 * (DESIGN.md §29.11, "Unions probed"): nsets[i] the posting sets of source i -
 * an IN list's values, a multi-key `&&`'s keys, an OR's leaves; 1 for a set
 * of its own - lying in setcontainers[i] containers together, where
 * containers[i] counts the container keys the source has (at most the
 * heap's).  With nsets NULL every source is taken for one set, as it was
 * before unions were probed; a grouped count and the FK-side join price
 * their unions themselves (LION_UNION_SET_COST, lion_merge_ops()).
 *
 *	- A union that DRIVES reads every container of its sets, and builds their
 *	  union at each of its keys: LION_UNION_KEY_COST a key and
 *	  LION_UNION_MEMBER_COST a member, the bitset image cleared, filled and
 *	  counted (lion_or_hot_raw()).
 *	- A union that is SOUGHT seeks each of its sets that has a container at or
 *	  before the key, and no more of them than its sets have containers: a
 *	  probe each (LION_PROBE_COST) for a set on posting pages of its own, and
 *	  what reading the container it lands on costs (LION_CONTAINER_COST) for
 *	  one INLINE on its entry, whose seek steps over the items of a payload in
 *	  memory - setpages[i], the posting pages of its sets, says which; the
 *	  pages themselves are the caller's (lion_cost_set_pages()).  Then it ANDs
 *	  the running intersection with the union the cheaper
 *	  way at each key, as lion_or_probe_pays() chooses: the union built and
 *	  ANDed (LION_UNION_KEY_COST, and LION_UNION_MEMBER_COST each of its
 *	  members at the key and each of the intersection's, a bit set and a bit
 *	  tested in its image), or the intersection's members looked up in each
 *	  of its sets' containers there, LION_AND_MEMBER_COST a member a
 *	  container.  An intersection of more members than an ARRAY holds is
 *	  never probed.
 *
 * Fitted on the release build of PostgreSQL 18 (2026-09-28, a heap of 70
 * rows a block, 8M rows, lists of 2 to 32 values and `&&` of 3 and 6 keys
 * ANDed with a set of a few rows a key, of a few hundred, and with dense
 * ones): a set of a list read or sought costs 110 to 400 ns a key, of which
 * the seek is 130 and a member of the union 1.2 to 1.5 ns; an intersection's
 * member looked up in a set's container 1.5 to 3.6 ns.
 */
double
lion_merge_cpu_cost_sets(int nsrc, const double *members,
						 const double *containers, const double *nsets,
						 const double *setcontainers, const double *setpages,
						 const bool *inmem, double tuples, double isect,
						 double *probes)
{
	int		   *order;
	int			i;
	int			k;
	double		keys;
	double		lambda;
	double		alive = 1.0;
	double		share = 1.0;
	double		cost;

	if (nsrc <= 0)
		return 0.0;

	order = (int *) palloc(sizeof(int) * nsrc);
	for (i = 0; i < nsrc; i++)
	{
		double		m = members[i];

		/* insertion sort by members, ascending: the order the merge seeks in */
		for (k = i; k > 0 && members[order[k - 1]] > m; k--)
			order[k] = order[k - 1];
		order[k] = i;
	}

	keys = Max(containers[order[0]], 1.0);
	lambda = Max(members[order[0]], 0.0) / keys;
	cost = keys * (LION_CONTAINER_COST +
				   LION_MEMBER_COST * Min(lambda, LION_MEMBER_CAP));
	if (nsets != NULL && nsets[order[0]] > 1.0)
	{
		/* a driving union: its sets' other containers, and its union built */
		cost += Max(setcontainers[order[0]] - keys, 0.0) * LION_CONTAINER_COST;
		cost += keys * LION_UNION_KEY_COST +
			Max(members[order[0]], 0.0) * LION_UNION_MEMBER_COST;
	}
	if (probes != NULL)
		probes[order[0]] = 0.0;
	if (nsrc > 1 && isect > 0.0 && isect != 1.0)
		share = pow(isect, 1.0 / (double) (nsrc - 1));

	for (k = 1; k < nsrc; k++)
	{
		int			j = order[k];
		double		sought = keys * alive;
		Cost		seek = (inmem != NULL && inmem[j]) ?
			LION_MEMORY_PROBE_COST : LION_PROBE_COST;
		double		lam = Min(lambda, LION_MEMBER_CAP);

		if (nsets != NULL && nsets[j] > 1.0)
		{
			/* a sought union: its sets sought, then built or probed */
			double		ukeys = Max(containers[j], 1.0);
			double		hot = Max(setcontainers[j] / ukeys, 1.0);
			double		mu = Max(members[j], 0.0) / ukeys;
			Cost		built = LION_UNION_KEY_COST +
				LION_UNION_MEMBER_COST * (mu + lam);
			Cost		probed = LION_AND_MEMBER_COST * lam * hot;

			double		chain = Min(setpages[j] / nsets[j], 1.0);

			cost += Min(sought * nsets[j], Max(setcontainers[j], sought)) *
				(LION_CONTAINER_COST +
				 (seek - LION_CONTAINER_COST) * Max(chain, 0.0));
			cost += sought * ((lambda > (double) LION_ARRAY_MAX_CARD) ?
							  built : Min(built, probed));
		}
		else
		{
			cost += sought * seek;
			cost += sought * LION_AND_MEMBER_COST * lam;
		}
		if (probes != NULL)
			probes[j] = sought;

		lambda *= Min(Max(members[j], 0.0) / Max(tuples, 1.0) * share, 1.0);
		alive = 1.0 - exp(-lambda);
	}

	pfree(order);
	return cost;
}

/*
 * The cost of ONE fetch of each of `pages` distinct heap pages of a relation
 * that has `heap_pages` pages altogether, per page.
 *
 * A recheck pass is not a sequence of random disk reads when the pages it
 * touches are in memory, and two things say that they are:
 *
 *	- the working set's share of the cache.  effective_cache_size is what the
 *	  planner is told about the memory available for caching, and
 *	  index_pages_fetched() already prorates it over the pages of the query's
 *	  relations; a dirty working set that fits in this relation's share of it
 *	  is read from memory rather than from the device, which is what makes a
 *	  count that visits each dirty page at most once per query (DESIGN.md §9)
 *	  cheap even when it returns to those pages for every group;
 *	- and the set's density.  A set that covers most of the relation is read
 *	  in physical order whatever the cache holds, which is the interpolation
 *	  cost_bitmap_heap_scan() makes between the two page costs.
 *
 * Whichever of the two argues for sequential access more strongly decides,
 * and the answer moves between seq_page_cost and random_page_cost - so a
 * dirty working set far larger than the cache is still charged as random
 * I/O, which is the case a blanket preference for this node would get wrong.
 */
Cost
lion_heap_page_cost(PlannerInfo *root, RelOptInfo *rel, double pages,
				   double heap_pages)
{
	double		spc_random_page_cost;
	double		spc_seq_page_cost;
	double		total_pages;
	double		cache_pages;
	double		resident;
	double		density;
	double		seqness;

	if (pages <= 0.0)
		return 0.0;

	get_tablespace_page_costs(rel->reltablespace,
							  &spc_random_page_cost,
							  &spc_seq_page_cost);

	/* This relation's prorated share of the cache, as index_pages_fetched(). */
	total_pages = Max(root->total_table_pages, heap_pages);
	cache_pages = Max((double) effective_cache_size * heap_pages / total_pages,
					  1.0);

	resident = Min(cache_pages / pages, 1.0);
	density = sqrt(Min(pages / heap_pages, 1.0));
	seqness = Max(resident, density);

	return spc_random_page_cost -
		(spc_random_page_cost - spc_seq_page_cost) * seqness;
}

/*
 * THE THIRD RUNG (DESIGN.md §22, "The open item"; §39, "Resident index
 * pages"): what a page of a lion index costs a custom path that reads it - a
 * container page walked or sought, a directory leaf an entry walk reads in
 * order - priced at `device` as I/O until now.
 *
 * lion_heap_page_cost() argues a page down from random_page_cost to
 * seq_page_cost from residency and never below, because for HEAP pages the
 * competing plan reads the same pages and pays the same convention.  Lion's
 * index is not read by its competitor: a BitmapAnd of btrees is charged CPU
 * per TID for the same rows, a hash aggregate its rows, and a page of a
 * small, hot index charged as a read from a device made the node several
 * times dearer per microsecond than the BitmapAnd §22 measured it against.
 * So an index the query's tables and it fit in effective_cache_size with -
 * core's own measure of what stays cached, index_pages_fetched()'s proration
 * - has its pages priced as the buffer hits they are, LION_RESIDENT_PAGE_COST
 * a page, and one that does not fit has the share that does: resident =
 * effective_cache_size / (the query's table pages + the index's), at most 1.
 * Never above `device`: a page core prices lower keeps core's price.
 *
 * The one directory leaf of a single-key lookup keeps random_page_cost, as
 * btcostestimate() charges btree's leaf and lion's own index scans charge
 * theirs; so do the heap pages of the recheck.
 */
Cost
lion_index_page_cost(PlannerInfo *root, double idxpages, Cost device)
{
	double		total = Max(root->total_table_pages + Max(idxpages, 1.0), 1.0);
	double		resident = Min((double) effective_cache_size / total, 1.0);
	Cost		hit = LION_RESIDENT_PAGE_COST;

	if (hit >= device)
		return device;
	return device - resident * (device - hit);
}

/*
 * ONE CLAUSE'S POSTING SETS, as the cost model prices them wherever they are
 * ANDed (DESIGN.md §22; §29.11, "One price for the AND of sets"): a WHERE
 * source of the count pushdown, and a qual a lion index scan answers.  The
 * same clause is located, read and sought the same way by both, so both take
 * these terms from here - lion_cost_count_rel() clause by clause, and
 * lion_cost_set_and() for a plain AND.  What the caller decides is how the
 * terms combine: whether the sets are read whole (an IN list's union, an OR's
 * leaf) or only sought, which source drives the leapfrog, and how often the
 * others are sought.
 *
 *	sel			the clause's own selectivity
 *	nkeys		the sets it locates: an IN list's values, 1 otherwise (a
 *				multi-key query is priced as one set, as it always was)
 *	members		its rows, every set of it together
 *	containers	the containers they lie in (lion_key_containers()), a list's
 *				counted set by set
 *	pages		the posting pages a WALK of its sets reads
 *	leaves		... of ONE of its sets, and that set's posting tree's height
 *	idxpages	the index's pages, which lion_heap_page_cost() prices its reads
 *				against
 *	randompages	directory leaves read at random_page_cost: one for a lookup
 *	lookuppages	an IN list's directory leaves, read in key order ...
 *	lookup		... and what they cost
 *	descent		the lookups' comparisons, LION_DESCENT_COST a level
 *	readall		reading every container of its sets, for a union or a sum
 *	unionops	the k-way union of an IN list's sets, in cpu_operator_cost
 *	nsets		the sets an AND meets it as the union of: an IN list's values,
 *				a multi-key `&&`'s keys (lion_merge_cpu_cost_sets()); 1 for a
 *				set of its own
 *	setcontainers	... and the containers those lie in together
 */
typedef struct LionSetClause
{
	double		sel;
	double		nkeys;
	double		nsets;
	double		setcontainers;
	double		members;
	double		containers;
	double		pages;
	double		leaves;
	double		height;
	double		idxpages;
	double		randompages;
	double		lookuppages;
	Cost		lookup;
	Cost		descent;
	Cost		readall;
	double		unionops;
} LionSetClause;


static void
lion_cost_set_clause(PlannerInfo *root, RelOptInfo *rel, IndexOptInfo *idx,
					 AttrNumber col, Node *clause, LionSetClause *sc)
{
	double		heap_pages = Max((double) rel->pages, 1.0);
	double		tuples = Max(rel->tuples, 1.0);
	Node	   *bare = IsA(clause, RestrictInfo) ?
		(Node *) ((RestrictInfo *) clause)->clause : clause;
	double		share;
	double		dirpages;
	double		height = 0;
	double		container_pages;
	double		nkeys = 1.0;
	double		sel;

	memset(sc, 0, sizeof(*sc));
	sel = clause_selectivity(root, lion_vcol_unvar(clause), 0, JOIN_INNER,
							 NULL);
	sc->sel = sel;

	/*
	 * The page terms belong to ONE KEY COLUMN of this index (DESIGN.md §24):
	 * the directory it descends is its own share of the relation's, and so
	 * are the container pages its chains lie on.  The DEPTH is not scaled -
	 * the descent passes through the upper levels the columns share - and
	 * neither is the index's own size below, which is what the caching
	 * argument of lion_heap_page_cost() is about and is a property of the
	 * relation.
	 */
	share = lion_index_column_share(root, rel, idx, col);
	dirpages = lion_index_dir_pages(idx, &height);

	container_pages = ((double) idx->pages - 1.0 - dirpages) *
		lion_index_column_posting_share(root, rel, idx, col);
	container_pages = Max(container_pages, 0.0);
	dirpages = Max(dirpages * share, 1.0);

	/*
	 * An IN list costs one lookup per element (DESIGN.md §15).  Each of them
	 * lands on a directory leaf of its own - at most one per leaf, so a list
	 * longer than the column has leaves shares them - walks a chain of its
	 * own, whose pages are its share of the container pages but never fewer
	 * than one, and contributes a sub-cursor of its own to the union the
	 * merge evaluates.  A union of k sets merges the members of all of them,
	 * which costs log2(k) comparisons per member however the merge is
	 * organised, and that is the term that makes a long list lose: at one
	 * million rows a thousand-element list took 15 ms against the B-tree
	 * index-only scan's 3.1 ms and was chosen anyway, because every element
	 * was priced as one bucket page (the 2026-09-21 follow-up review).  A
	 * single-key clause has k = 1 and pays nothing for a merge it does not
	 * make.
	 */
	if (IsA(bare, ScalarArrayOpExpr))
	{
		ScalarArrayOpExpr *saop = (ScalarArrayOpExpr *) bare;

#if PG_VERSION_NUM >= 170000
		nkeys = Max(estimate_array_length(root, (Node *) lsecond(saop->args)),
					1.0);
#else
		nkeys = Max(estimate_array_length((Node *) lsecond(saop->args)), 1.0);
#endif
	}

	if (nkeys > 1.0)
	{
		/*
		 * A list's lookups are NOT a sequence of random reads.
		 * lion_posting_set_lookup_many() sorts the values into the directory
		 * order and walks the leaves left to right (DESIGN.md §21), so the
		 * same argument lion_heap_page_cost() makes about a recheck's heap
		 * pages applies to them, and more strongly than it did to the hash
		 * directory this replaced: a set of pages that is dense in the index,
		 * or that fits in the cache, is read at something near seq_page_cost.
		 * Charging a thousand-element list five hundred RANDOM reads of a
		 * one-megabyte index is what kept the node from being chosen for a
		 * query it answers in 2.9 ms against the B-tree index-only scan's
		 * 3.7.
		 *
		 * The chain is capped at the container pages the index has: a list
		 * cannot read more of them than exist, and an index whose entries are
		 * all INLINE (which is what a high-cardinality column looks like since
		 * DESIGN.md §13) has none to read at all - its payloads are on the
		 * leaves already charged.
		 */
		double		idx_pages = Max((double) idx->pages, 1.0);

		sc->lookuppages = Min(nkeys, dirpages);
		sc->lookup = sc->lookuppages *
			lion_heap_page_cost(root, rel, sc->lookuppages, idx_pages);

		/*
		 * ... and each value's search: a binary search of its leaf, and the
		 * levels above it too where the values are too sparse for the walk to
		 * step from one to the next (a descent each, §21).  Free before
		 * 2026-09-27, which chose a thousand-value list over a near-unique
		 * column at 7.6 ms against the btree index-only scan's 1.8.
		 */
		sc->descent = nkeys * LION_DESCENT_COST *
			(1.0 + height * Min(1.0, dirpages / nkeys));
		sc->pages = Min(Max(nkeys, container_pages * sel), container_pages);
	}
	else
	{
		/*
		 * One descent.  Only the LEAF is charged as a page read: the root and
		 * the internal pages above it are a handful of blocks that every
		 * lookup touches, so they stay in cache, which is exactly the argument
		 * btcostestimate() makes about a btree's upper levels.  What the
		 * descent does cost is the comparisons, one page's worth per level.
		 */
		sc->randompages = 1.0;
		sc->descent = (height + 1.0) * LION_DESCENT_COST;
		sc->pages = Max(1.0, container_pages * sel);
	}

	/*
	 * What a walk of this clause's sets would read; how much of it is really
	 * read depends on whether it drives the leapfrog join, which is the
	 * caller's to say (lion_cost_set_pages()).  One of its sets - a list has
	 * nkeys of them - occupies this many container pages, and its posting
	 * tree is that tall.
	 */
	sc->nkeys = nkeys;
	sc->leaves = Max(sc->pages / nkeys, 1.0);
	sc->height = lion_posting_height(sc->leaves);
	sc->idxpages = Max((double) idx->pages, 1.0);

	/*
	 * Its rows lie in this many containers, and reading every one of them -
	 * what a union is built of (§15, §19), or a sum adds up - costs the
	 * merge's price of a container the driver reads (lion_merge_cpu_cost()).
	 */
	sc->members = tuples * sel;
	sc->containers = nkeys * lion_key_containers(root, rel, idx, col,
												 tuples * sel / nkeys);
	sc->readall = sc->containers *
		(LION_CONTAINER_COST +
		 LION_MEMBER_COST * Min(sc->members / sc->containers, LION_MEMBER_CAP));
	if (nkeys > 1.0)
		sc->unionops = lion_merge_ops(heap_pages, sc->members, nkeys);

	/*
	 * The union an AND meets it as (DESIGN.md §29.11, "Unions probed"): an
	 * IN list's sets, or a multi-key query that ORs its keys, `&&`, whose
	 * keys' sets the executor locates and unites as a list's (§17); its rows
	 * are shared out among them, as a list's are.
	 */
	sc->nsets = nkeys;
	sc->setcontainers = sc->containers;
	if (nkeys <= 1.0 && IsA(bare, OpExpr))
	{
		double		mkeys;
		bool		isunion;

		(void) lion_multikey_cost_mode_ex(idx, col, bare, &mkeys, &isunion);
		if (isunion && mkeys > 1.0)
		{
			sc->nsets = mkeys;
			sc->setcontainers = mkeys *
				lion_key_containers(root, rel, idx, col,
									sc->members / mkeys);
		}
	}
}

/*
 * The directory leaves the single-key lookups of an AND read (DESIGN.md
 * §29.11, "One price for the AND of sets"): one a lookup, at
 * random_page_cost (lion_cost_set_clause()), but no more of one index than
 * its directory has - two lookups into a directory of one leaf read that
 * leaf once, and the second is a buffer hit.  An index is asked as often as
 * the AND has clauses on it: of its columns, or several of one column.
 */
static double
lion_cost_leaf_pages(int n, IndexOptInfo **idxs, const LionSetClause *sc)
{
	double		pages = 0;
	int			i;
	int			j;

	for (i = 0; i < n; i++)
	{
		double		lookups = 0;
		bool		seen = false;

		if (idxs[i] == NULL || sc[i].randompages <= 0.0)
			continue;
		for (j = 0; j < i && !seen; j++)
			seen = (idxs[j] != NULL && sc[j].randompages > 0.0 &&
					idxs[j]->indexoid == idxs[i]->indexoid);
		if (seen)
			continue;			/* counted with the first lookup of its index */
		for (j = i; j < n; j++)
		{
			if (idxs[j] != NULL && idxs[j]->indexoid == idxs[i]->indexoid)
				lookups += sc[j].randompages;
		}
		pages += Min(lookups, lion_index_dir_pages(idxs[i], NULL));
	}
	return pages;
}

/*
 * What a clause's sets read of their posting trees in the AND (DESIGN.md
 * §22): the DRIVER - the source with the fewest members - walks its sets end
 * to end, and pays for its whole share of the index's container pages, in
 * order (added to *seqpages); every other source is SOUGHT to the container
 * keys the driver produces, `probes` times, and pays for the pages those
 * probes touch (lion_probed_pages()), never for more than a walk of it would
 * have cost, priced as lion_heap_page_cost() prices a read of them (added to
 * *probedpages; the price is returned).
 *
 * A custom path's count passes *seqcost: then the walked pages are priced
 * into it, and both kinds at the index's third rung (lion_index_page_cost(),
 * §39).  An index scan's AND passes NULL and prices its walked pages itself,
 * as core prices its own scans' pages.
 */
static Cost
lion_cost_set_pages(PlannerInfo *root, RelOptInfo *rel,
					const LionSetClause *sc, bool walked, double probes,
					double *seqpages, double *probedpages, Cost *seqcost)
{
	double		pages = sc->pages;
	Cost		price;

	if (pages <= 0.0)
		return 0.0;
	if (walked)
	{
		*seqpages += pages;
		if (seqcost != NULL)
			*seqcost += pages * lion_index_page_cost(root, sc->idxpages,
													 seq_page_cost);
		return 0.0;
	}
	pages = Min(pages, sc->nkeys * lion_probed_pages(sc->leaves, probes,
													 sc->height));
	*probedpages += pages;
	price = lion_heap_page_cost(root, rel, pages, sc->idxpages);
	if (seqcost != NULL)
		price = lion_index_page_cost(root, sc->idxpages, price);
	return pages * price;
}

/*
 * THE AND OF POSTING SETS (DESIGN.md §29.11, "One price for the AND of
 * sets"): what locating the sets of n clauses on rel's lion indexes, building
 * the unions of the IN lists among them and intersecting them costs.  It is
 * the price lion_cost_count_rel() charges an ungrouped count for the same
 * WHERE sources, term for term and from the same functions - the lookups (a
 * directory leaf at random_page_cost and a descent each; an IN list's leaves
 * in key order), the unions (every container of a list's sets read, and
 * their k-way merge), the AND (lion_merge_cpu_cost(): the smallest source
 * drives, the others are sought at its keys) and the posting pages (the
 * driver's walked in order, the others' as far as the seeks reach) - and the
 * one lioncostestimate() charges a scan that ANDs the same sets.  Only the
 * index's work: the heap, and what a count does in its place, are each
 * path's own.
 *
 * The parts come back in *out, so that a caller that repeats the AND - a
 * nested loop's inner scan - can amortize its pages over the repetitions.
 * isect is what the intersection probe found of the AND's rows against the
 * product of the clauses' selectivities (lion_merge_cpu_cost()).
 */
Cost
lion_cost_set_and(PlannerInfo *root, RelOptInfo *rel, int n,
				  IndexOptInfo **idxs, const AttrNumber *cols, Node **clauses,
				  double isect, LionAndCost *out)
{
	LionSetClause *sc;
	double	   *members;
	double	   *containers;
	double	   *nsets;
	double	   *setcontainers;
	double	   *setpages;
	double	   *probes;
	double		ckeys = Max((double) rel->pages / LION_BLOCKS_PER_CONTAINER,
							1.0);
	double		seqpages = 0;
	double		probedpages = 0;
	double		randompages = 0;
	Cost		probed = 0;
	Cost		lookup = 0;
	int			driver = -1;
	int			i;

	memset(out, 0, sizeof(*out));
	if (n <= 0)
		return 0.0;

	sc = (LionSetClause *) palloc(sizeof(LionSetClause) * n);
	members = (double *) palloc(sizeof(double) * n);
	containers = (double *) palloc(sizeof(double) * n);
	nsets = (double *) palloc(sizeof(double) * n);
	setcontainers = (double *) palloc(sizeof(double) * n);
	setpages = (double *) palloc(sizeof(double) * n);
	probes = (double *) palloc0(sizeof(double) * n);

	for (i = 0; i < n; i++)
	{
		lion_cost_set_clause(root, rel, idxs[i], cols[i], clauses[i], &sc[i]);

		lookup += sc[i].lookup;
		out->leafpages += sc[i].lookuppages;
		out->cpu += sc[i].descent;

		members[i] = sc[i].members;
		containers[i] = Min(sc[i].setcontainers, ckeys);
		nsets[i] = sc[i].nsets;
		setcontainers[i] = sc[i].setcontainers;
		setpages[i] = sc[i].pages;
		if (driver < 0 || members[i] < members[driver])
			driver = i;
	}

	/*
	 * The unions among them - IN lists, multi-key `&&` - are read, built or
	 * probed as the leapfrog meets them (lion_merge_cpu_cost_sets(), DESIGN.md
	 * §29.11, "Unions probed").
	 */
	randompages = lion_cost_leaf_pages(n, idxs, sc);
	out->leafpages += randompages;
	out->cpu += lion_merge_cpu_cost_sets(n, members, containers, nsets,
										 setcontainers, setpages, NULL,
										 Max(rel->tuples, 1.0), isect, probes);

	for (i = 0; i < n; i++)
		probed += lion_cost_set_pages(root, rel, &sc[i], i == driver,
									  probes[i], &seqpages, &probedpages,
									  NULL);

	out->nsrc = n;
	out->leafcost = randompages * random_page_cost + lookup;
	out->setpages = seqpages + probedpages;
	out->setcost = seqpages * seq_page_cost + probed;

	pfree(sc);
	pfree(members);
	pfree(containers);
	pfree(nsets);
	pfree(setcontainers);
	pfree(setpages);
	pfree(probes);

	return out->leafcost + out->setcost + out->cpu;
}


/*
 * Does an IN list's source take the disjoint-sum short-circuit of DESIGN.md
 * §15, and does it drive the groups?  Both questions are about the SHAPE of
 * the query and are answered here so that the price matches what the executor
 * will do (lion_count_sources_cached(), lion_next_group_inlist()).
 *
 *	*sumshort	the list is the only positive source and nothing else drives
 *				the count, so its union MAY never be built: each entry is
 *				counted on its own and the counts are added up.  Whether that is
 *				also the cheaper way depends on the entries' density and is
 *				decided by the caller, which has the estimates.
 *	*groupdrive	the list is on the very column the GROUP BY drives, so the
 *				listed values ARE the groups: the index's entry scan does not
 *				happen, there are at most as many groups as listed values, and
 *				the list is not a source (a group intersected with the union of
 *				a disjoint list is the group).
 *
 * eqdrives says that an EQUALITY on the driving column drives the entries as
 * a list of one would, which is what the count(DISTINCT k) walk of DESIGN.md
 * §26 does with `k = c` (a GROUP BY never sees one: the planner folds a
 * grouping column an equality pins).
 *
 * Returns the clause index of the list, or -1 when neither applies.
 *
 * *groupdrive has to be the executor's answer and not an approximation of
 * it, because it is also what decides whether the path may claim pathkeys:
 * the entry walk emits its groups in directory order, the list's sets in
 * whatever order they were located in.  lion_locate_where() takes as the
 * driver the FIRST clause - in clause order, OR leaves skipped - that is a
 * list (or, with eqdrives, an equality) on the driving index's own key
 * column, whatever else the WHERE holds, and so does this function.  It used
 * to give up at the second list anywhere in the WHERE ("neither is THE one"),
 * which with `g IN (...) AND h IN (...) GROUP BY g` priced a walk of every
 * entry of g and promised `ORDER BY g` a sorted output the executor never
 * built: it drove the groups from g's list all the same (the 2026-09-25
 * review).  The listed sets come out in key order for every opfamily this
 * extension ships, which is why no test caught it; a family whose lookup
 * falls back to an unsorted probe emits them in hash order.
 */
int
lion_inlist_shape(IndexOptInfo *groupidx, AttrNumber groupcol,
				 IndexOptInfo *groupidx2,
				 List *whereidx, List *wherecol, List *whereclauses,
				 List *wherekinds,
				 List *ors, bool eqdrives, bool *sumshort, bool *groupdrive)
{
	int			nclause = list_length(whereclauses);
	bool	   *inor = lion_or_leaf_map(ors, nclause);
	int			firstlist = -1; /* the first list anywhere */
	int			nlist = 0;
	int			groupci = -1;	/* the first one on the driving column */
	int			npos = 0;
	int			ci = 0;
	ListCell   *lc1;
	ListCell   *lc2;
	ListCell   *lc3;
	ListCell   *lc4;

	*sumshort = false;
	*groupdrive = false;

	forfour(lc1, whereidx, lc2, whereclauses, lc3, wherekinds, lc4, wherecol)
	{
		IndexOptInfo *idx = (IndexOptInfo *) lfirst(lc1);
		bool		ondriver = (groupidx != NULL &&
								idx->indexoid == groupidx->indexoid &&
								(AttrNumber) lfirst_int(lc4) == groupcol);

		if (!inor[ci] && LION_CLAUSE_IS_POSITIVE(lfirst_int(lc3)))
		{
			npos++;
			if (IsA((Node *) lfirst(lc2), ScalarArrayOpExpr) ||
				(eqdrives && lfirst_int(lc3) == LION_CLAUSE_EQ && ondriver))
			{
				nlist++;
				if (firstlist < 0)
					firstlist = ci;
				if (groupci < 0 && ondriver)
					groupci = ci;
			}
		}
		ci++;
	}
	pfree(inor);

	/*
	 * The list drives the groups only when its entries ARE the groups: the
	 * same index AND the same key column (DESIGN.md §24).  Two columns of one
	 * multicolumn index are two independent sets of entries, so a list on `b`
	 * says nothing about the groups of `a` - which is also how the executor
	 * decides it (lion_locate_where(), by index and by heap attno).  Any other
	 * list, or a second one on the same column, is an ordinary source.
	 *
	 * The sum of a list's entries stands for its union only when nothing else
	 * is in the AND, so that needs the list to be the one positive clause.
	 */
	if (groupidx == NULL && groupidx2 == NULL && ors == NIL && npos == 1 &&
		nlist == 1)
	{
		*sumshort = true;
		return firstlist;
	}
	if (groupidx != NULL && groupidx2 == NULL && groupci >= 0)
	{
		*groupdrive = true;
		return groupci;
	}
	return -1;
}

/*
 * The Var of the WHERE clause when it is exactly one positive clause and that
 * clause is a plain equality `col = value` on a column of rel; NULL otherwise
 * (a partition's clauses name the parent, and are left alone).
 */
static Var *
lion_single_eq_var(RelOptInfo *rel, List *whereclauses, List *wherekinds)
{
	Node	   *eq = NULL;
	int			npos = 0;
	ListCell   *lc1;
	ListCell   *lc2;
	Node	   *arg;

	forboth(lc1, whereclauses, lc2, wherekinds)
	{
		if (!LION_CLAUSE_IS_POSITIVE(lfirst_int(lc2)))
			continue;
		npos++;
		if (lfirst_int(lc2) == LION_CLAUSE_EQ)
			eq = (Node *) lfirst(lc1);
	}
	if (npos != 1 || eq == NULL)
		return NULL;
	if (IsA(eq, RestrictInfo))
		eq = (Node *) ((RestrictInfo *) eq)->clause;
	if (!IsA(eq, OpExpr) || list_length(((OpExpr *) eq)->args) != 2)
		return NULL;

	arg = (Node *) linitial(((OpExpr *) eq)->args);
	while (arg != NULL && IsA(arg, RelabelType))
		arg = (Node *) ((RelabelType *) arg)->arg;
	if (arg == NULL || !IsA(arg, Var) || (Index) ((Var *) arg)->varno != rel->relid)
	{
		arg = (Node *) lsecond(((OpExpr *) eq)->args);
		while (arg != NULL && IsA(arg, RelabelType))
			arg = (Node *) ((RelabelType *) arg)->arg;
	}
	if (arg == NULL || !IsA(arg, Var) || (Index) ((Var *) arg)->varno != rel->relid ||
		((Var *) arg)->varattno <= 0)
		return NULL;
	return (Var *) arg;
}

/*
 * THE SUM OVER A RANGE (DESIGN.md §28, "The cost of a summed range"):
 * `count(*) WHERE <range on k> [AND F]`, priced as lion_sumall_relation()
 * runs it.
 *
 *	- ONE side of the range is walked: the entries it selects, or - when F
 *	  has a positive source that can drive a count, which a range taken as a
 *	  source cannot, and k's directory is ordered - the entries below and
 *	  above it, plus one count of F minus k's NULL entry (the complement).
 *	  The executor takes the side with fewer leaves, and so does this.
 *	- Each entry walked costs a fixed amount (LION_RANGE_ENTRY_COST for one
 *	  counted on its own, LION_RANGE_UNION_ENTRY_COST for a small one counted
 *	  with the rest of its leaf) plus its OWN containers - the rows of one key
 *	  of k, not the rows that survive F: the entry drives its count when it is
 *	  the smaller, and F is then probed at each of its container keys, so the
 *	  key steps are the smaller of the two containers' worth, once for every
 *	  source.  How many containers one key's rows lie in follows the column's
 *	  correlation with the heap, as cost_index() interpolates pages.
 *	- When k has SUMMARIES (DESIGN.md §32), a side that covers whole buckets
 *	  walks only the keys of the buckets at its ends, entry by entry, and
 *	  counts each whole bucket's summary once instead of its keys
 *	  (lion_cost_range_side()).
 *	- The leaves under the entries walked are read once each; with a
 *	  complement to choose, both sides are first stepped a leaf at a time up
 *	  to the smaller one's end, counting only (lion_range_choose()).
 *	- The heap the visibility map cannot vouch for: F's candidates and the
 *	  entries' for the complement, the entries' for the range, each page
 *	  fetched at most twice (the per-entry revisits of DESIGN.md §28).
 *
 * Before this the walk was charged the range's entries at LION_RANGE_ENTRY_COST
 * and ONE container each - the rows of the whole count spread over them - so a
 * range over 2,000 keys of a thousand rows each beside a selective equality
 * was priced at 1,344 for 920,000 containers, and a range over 1.9 million keys
 * at 813,000 whether or not the keys outside it were a handful.
 */

/*
 * What the walk of a range collected as a source hands its union (DESIGN.md
 * §32, "What collecting a range costs"): the entries it walks one by one and
 * the rows of each, the summaries of the whole buckets and the rows of each,
 * and the column's order in the heap.  lion_cost_range_sum() fills it in for
 * lion_cost_range_union().
 */
typedef struct LionRangeWalk
{
	double		sides;			/* in: the range's bounded sides, 1 or 2 */
	double		entries;		/* entries walked one by one */
	double		entryrows;		/* ... the rows of each */
	double		sums;			/* summaries of whole buckets */
	double		bucketrows;		/* ... the rows of each */
	double		corr;			/* the column's correlation with the heap */
} LionRangeWalk;

/*
 * A summarized column (DESIGN.md §32), as the cost of a walk over it sees it:
 * how many keys one bucket holds, and what one bucket's summary costs to
 * count and to read.
 */
typedef struct LionSumModel
{
	double		keysper;		/* keys of k in one bucket */
	double		persum;			/* one summary's count: fixed + containers */
	double		sumpages;		/* ... and the pages it reads */
} LionSumModel;

/*
 * What one side of a summed range walk costs (DESIGN.md §28, §32): nkeys
 * entries of k in one run, each `perentry` and `pageper` pages - or, when k is
 * summarized (sum != NULL), the keys of the partial buckets at the run's
 * `nedges` ends one by one and each bucket it covers whole as ONE count of
 * its summary.  A run of n keys over buckets of `keysper` keys covers about
 * n / keysper - 1 of them whole when both its ends fall inside a bucket: half
 * a bucket is left over at each end on average.  With no whole bucket the
 * executor walks the keys as it always did (lion_entry_scan_plan_sum()), and
 * so does this.
 *
 * *counts is how many counts the side makes - entries and summaries - which
 * is what the heap recheck is spread over; *pages is what it reads.
 */
static Cost
lion_cost_range_side(double nkeys, double nedges, const LionSumModel *sum,
					 double perentry, double pageper, double *counts,
					 double *pages)
{
	double		whole = 0.0;
	double		edgekeys;
	Cost		cost;

	if (sum != NULL)
		whole = Max(0.0, nkeys / sum->keysper - 0.5 * nedges);
	if (sum == NULL || whole < 1.0)
	{
		*counts = nkeys;
		*pages = nkeys * pageper;
		return nkeys * (perentry + pageper * seq_page_cost);
	}

	edgekeys = Max(0.0, nkeys - whole * sum->keysper);
	*counts = edgekeys + whole;
	*pages = edgekeys * pageper + whole * sum->sumpages;
	cost = edgekeys * (perentry + pageper * seq_page_cost) +
		whole * (sum->persum + sum->sumpages * seq_page_cost);

	/* the phases after the first each start with a descent of their own */
	return cost + LION_SUMMARY_PHASE_DESCENTS * random_page_cost;
}

/*
 * The same side beside the other sources F when the executor may PROBE it
 * (DESIGN.md §32, "Summed ranges: dense and probed"): `probe` and
 * `probeentry` are what a summary and an entry cost probed rather than
 * counted, siderows the rows the side holds and frows F's.  A walk that uses
 * summaries turns to probing once it has handed out LION_PROBE_SWITCH times
 * F's rows (lion_sum_probe_due()); the sets before that are counted as they
 * always were, those after it probed, and F is counted twice more - once
 * collected, once ANDed with what the probe marked - which is `fcount` each.
 * Without a whole bucket, or when the side never gets that far, it is
 * lion_cost_range_side()'s.  *counts is the counts the heap recheck is spread
 * over: the ones before the switch, and the probe's one.
 */
static Cost
lion_cost_range_side_probed(double nkeys, double nedges,
							const LionSumModel *sum, const LionSumModel *probe,
							double perentry, double probeentry,
							double pageper, double siderows, double frows,
							Cost fcount, double *counts, double *pages)
{
	Cost		counted = lion_cost_range_side(nkeys, nedges, sum, perentry,
											   pageper, counts, pages);
	Cost		probed;
	double		pcounts;
	double		ppages;
	double		before;

	if (sum == NULL || probe == NULL ||
		nkeys / sum->keysper - 0.5 * nedges < 1.0 ||
		siderows < LION_PROBE_SWITCH * frows)
		return counted;

	probed = lion_cost_range_side(nkeys, nedges, probe, probeentry, pageper,
								  &pcounts, &ppages);
	before = Min(1.0, LION_PROBE_SWITCH * frows / Max(siderows, 1.0));
	*counts = before * *counts + 1.0;
	return before * counted + (1.0 - before) * probed + 2.0 * fcount;
}

static Cost
lion_cost_range_sum(PlannerInfo *root, RelOptInfo *rel, IndexOptInfo *groupidx,
					AttrNumber groupcol, Var *rangevar, Selectivity rangesel,
					List *whereclauses, List *wherekinds, List *ors,
					LionRangeWalk *collect)
{
	double		heap_pages = Max((double) rel->pages, 1.0);
	double		tuples = Max(rel->tuples, 1.0);
	double		matching = Max(lion_probe_rel_rows(root, rel), 1.0);
	double		ckeys = Max(heap_pages / LION_BLOCKS_PER_CONTAINER, 1.0);
	double		dirtyfrac = 1.0 - rel->allvisfrac;
	double		sel = Min(Max(rangesel, 1e-10), 1.0);
	double		nd = lion_var_ndistinct(root, rel, rangevar);
	double		nin = Max(1.0, nd * sel);
	double		nout = Max(0.0, nd - nin);
	double		rowsper = Max(tuples * sel / nin, 1.0);
	double		corr = lion_var_correlation(root, rel, rangevar);
	double		scattered = lion_containers_for(heap_pages, rowsper);
	double		inorder = Max(1.0, rowsper * ckeys / tuples);
	double		percont = scattered + (inorder - scattered) * corr * corr;
	double		pageper;
	double		frows = Min(tuples, matching / sel);
	double		fcont = lion_containers_for(heap_pages, frows);
	double		fm = Min(frows / Max(fcont, 1.0), LION_MEMBER_CAP);
	double		m = Min(rowsper / Max(percont, 1.0), LION_MEMBER_CAP);
	double		steps;
	double		perentry;
	double		probeentry = 0.0;
	Cost		fcount;
	int			nsrc = list_length(ors);
	int			nplain = list_length(ors);
	int		   *orgrp = lion_or_group_map(ors, list_length(whereclauses));
	int			ci = 0;
	LionState  *colstate;
	bool		ordered;
	bool		summarized;
	uint32		bucket_tids;
	Relation	indexrel;
	ListCell   *lc;
	LionSumModel summodel;
	LionSumModel *sum = NULL;
	LionSumModel probemodel;
	LionSumModel *probe = NULL;
	LionSumShape shape;
	double		incounts;
	double		inpages;
	double		outcounts;
	double		outpages;
	Cost		inside;
	Cost		outside;

	/*
	 * The sources of F: one per OR restriction, one per positive clause - and
	 * of those, the ones that can drive a count of F (nplain): not a range
	 * taken as a source, which is collected into a copy that carries no pin
	 * (§32) and that the executor never lets drive (lion_source_drives()).
	 */
	foreach(lc, wherekinds)
	{
		if (LION_CLAUSE_IS_POSITIVE(lfirst_int(lc)) && orgrp[ci] < 0)
		{
			nsrc++;
			if (lfirst_int(lc) != LION_CLAUSE_RANGESRC)
				nplain++;
		}
		ci++;
	}
	pfree(orgrp);

	/* One count of F: its containers, each of its sources probed at them. */
	fcount = fcont * (LION_CONTAINER_COST + LION_MEMBER_COST * fm +
					  nsrc * LION_PROBE_COST);

	indexrel = index_open(groupidx->indexoid, AccessShareLock);
	colstate = lion_index_column_state(indexrel, groupcol);
	ordered = colstate->ordered;
	summarized = colstate->summarized;
	bucket_tids = lion_get_index_state(indexrel)->meta.summary_tids;
	memset(&shape, 0, sizeof(shape));
	if (summarized && ordered && bucket_tids > 0)
		lion_summary_shape(indexrel, colstate, &shape);
	index_close(indexrel, AccessShareLock);

	/*
	 * One entry: its fixed cost, and its containers' key steps against every
	 * source of F.  Small ones are counted a leaf at a time.
	 */
	{
		double		keys = (nsrc > 0) ? Min(percont, fcont) : percont;

		/*
		 * the driver's containers, and each of F's sources probed at them -
		 * in memory, where a walk copies them on their second use (§9)
		 */
		steps = keys * (LION_CONTAINER_COST + LION_MEMBER_COST * m) +
			keys * nsrc * (LION_MEMORY_PROBE_COST + LION_AND_MEMBER_COST * m);
	}
	perentry = ((percont <= (double) LION_SUM_UNION_MAX_ITEMS) ?
				LION_RANGE_UNION_ENTRY_COST : LION_RANGE_ENTRY_COST) + steps;

	/* The leaves under one entry: its share of the column's pages. */
	pageper = Max(1.0, (double) groupidx->pages *
				  lion_index_column_share(root, rel, groupidx, groupcol)) / nd;

	/*
	 * The summaries (DESIGN.md §32).  A bucket closes at the first key that
	 * brings it to summary_tids rows, so it holds that many rows - or one
	 * key's, when a key alone has more - and bucketrows / rowsper keys.  Its
	 * summary is ONE set of those rows, which lies in as many containers as
	 * that many rows spread over the heap as the column's order says, and
	 * takes about the bytes of its keys' sets with the containers they share
	 * merged: their pages, scaled by the containers the union saves.  It is
	 * counted on its own (LION_RANGE_ENTRY_COST), as a large entry is.
	 *
	 * That is a column whose keys arrive in order.  Keys that arrive in
	 * descending order all go into the first bucket (the open one, on an
	 * index built empty), and keys in no order into the middle buckets, which
	 * never split: the buckets are then far larger than summary_tids, and a
	 * range walks the keys of the ones at its ends.  The rows the column's
	 * summaries hold per summary, read off the index (lion_summary_shape()),
	 * say how large they really are, and once that is more than twice what
	 * the reloption says it is what the model takes - so that one giant
	 * bucket makes a range that covers a bucket whole the rare case it is.
	 * The model used to count whole buckets the data did not have, and priced
	 * such a range as a sum of summaries that the executor then walked key by
	 * key.  A column with no summary at all - `on` over an empty table - is
	 * walked, as the executor walks it.
	 */
	if (summarized && ordered && bucket_tids > 0 && shape.nsummaries > 0)
	{
		double		bucketrows = Max((double) bucket_tids, rowsper);
		double		realrows = shape.rows / shape.nsummaries;
		double		sscattered;
		double		sinorder;
		double		sc;
		double		sm;
		double		skeys;

		if (realrows > 2.0 * bucketrows)
			bucketrows = realrows;
		sscattered = lion_containers_for(heap_pages, bucketrows);
		sinorder = Max(1.0, bucketrows * ckeys / tuples);
		sc = sscattered + (sinorder - sscattered) * corr * corr;
		sm = Min(bucketrows / Max(sc, 1.0), LION_MEMBER_CAP);
		skeys = (nsrc > 0) ? Min(sc, fcont) : sc;

		summodel.keysper = Max(1.0, bucketrows / rowsper);

		/* the summary's containers, and F's sources probed at them, as above */
		summodel.persum = LION_RANGE_ENTRY_COST +
			skeys * (LION_CONTAINER_COST + LION_MEMBER_COST * sm) +
			skeys * nsrc * (LION_MEMORY_PROBE_COST + LION_AND_MEMBER_COST * sm);
		summodel.sumpages = Max(1.0, summodel.keysper * pageper *
								Min(1.0, sc / (summodel.keysper * percont)));
		sum = &summodel;

		/*
		 * ... and PROBED at F's rows instead (DESIGN.md §32, "Summed ranges:
		 * dense and probed"), which the executor turns to part of the way
		 * through a side that holds more rows than F, when F has a source
		 * that carries a pin and a copy of it fits work_mem
		 * (lion_sum_probe_due()): each summary and each entry a set with no
		 * count of its own, its containers stepped over at
		 * LION_PROBE_STEP_COST and the ones at F's keys marked against F's
		 * members, the fewer of the two sides' per container.
		 */
		if (collect == NULL && nplain > 0 &&
			frows * 2.0 * sizeof(uint16) <= (double) work_mem * 1024.0)
		{
			probemodel = summodel;
			probemodel.persum = LION_RANGE_UNION_ENTRY_COST +
				sc * LION_PROBE_STEP_COST +
				Min(sc, fcont) * LION_AND_MEMBER_COST * Min(sm, fm);
			probeentry = LION_RANGE_UNION_ENTRY_COST +
				percont * LION_PROBE_STEP_COST +
				Min(percont, fcont) * LION_AND_MEMBER_COST * Min(m, fm);
			probe = &probemodel;
		}
	}

	inside = lion_cost_range_side_probed(nin,
										 (collect != NULL) ? collect->sides : 2.0,
										 sum, probe, perentry,
										 probeentry, pageper, tuples * sel,
										 frows, fcount, &incounts, &inpages);

	/*
	 * A range collected as a source (DESIGN.md §32) reads the same walk and
	 * counts nothing, so it rechecks nothing either; what its union costs is
	 * lion_cost_range_union()'s, from what the walk hands it: the keys of the
	 * partial buckets one by one - all of the range's keys when no bucket is
	 * whole - and the summaries of the whole ones, split as
	 * lion_cost_range_side() splits them.
	 */
	if (collect != NULL)
	{
		double		whole = (sum != NULL) ?
			Max(0.0, nin / sum->keysper - 0.5 * collect->sides) : 0.0;

		if (whole < 1.0)
			whole = 0.0;
		collect->sums = whole;
		collect->entries = (whole > 0.0) ?
			Max(0.0, nin - whole * sum->keysper) : nin;
		collect->entryrows = rowsper;
		collect->bucketrows = (whole > 0.0) ?
			rowsper * sum->keysper : 0.0;
		collect->corr = corr;
		return inside;
	}
	inside += lion_range_recheck(root, rel, matching * dirtyfrac, incounts,
								 corr);

	/*
	 * No complement without a source of F that can drive |F - NULL(k)|, which
	 * the executor asks of F before it steps the two sides at all
	 * (lion_range_choose_on()): beside nothing but ranges taken as sources -
	 * `count(*) WHERE mid > 10 AND hi > 10`, one collected, the other summed -
	 * the inside is walked whatever it holds.  This used to ask for any
	 * source (nsrc), and priced the complement of two ranges over nearly
	 * every row at a tenth of the inside walk that ran (2026-09-29 review).
	 * A range too large to collect is walked at every count instead, and can
	 * drive; it is taken as collected here all the same, which prices such a
	 * count at its inside - what it costs at most.
	 */
	if (nplain == 0 || !ordered)
		return inside;

	/*
	 * The complement: the entries outside - a run at each end of the column,
	 * with a partial bucket at the range's side of each - the count of F minus
	 * the NULL entry (F's containers once more, and a descent), and F's
	 * candidates on top of the outside entries' for the recheck.
	 */
	outside = lion_cost_range_side_probed(nout, 2.0, sum, probe, perentry,
										  probeentry, pageper, nout * rowsper,
										  frows, fcount, &outcounts,
										  &outpages) +
		fcount + random_page_cost +
		lion_range_recheck(root, rel, frows * (2.0 - sel) * dirtyfrac,
						   Max(outcounts, 1.0), corr);

	/* ... and the leaf-by-leaf race that picks the side, counting only. */
	return Min(inside, outside) +
		2.0 * Min(inpages, outpages) * seq_page_cost;
}

/*
 * The column of idx's key column col, as a Var of rel: what the statistics
 * of a range on it are looked up by.  NULL for an expression column.
 */
static Var *
lion_index_col_var(PlannerInfo *root, RelOptInfo *rel, IndexOptInfo *idx,
				   AttrNumber col)
{
	RangeTblEntry *rte;
	AttrNumber	attno = idx->indexkeys[col - 1];
	Oid			type;

	if (attno <= 0 || rel->relid == 0 ||
		rel->relid >= (Index) root->simple_rel_array_size)
		return NULL;
	rte = root->simple_rte_array[rel->relid];
	if (rte == NULL || rte->rtekind != RTE_RELATION)
		return NULL;
	type = get_atttype(rte->relid, attno);
	return makeVar(rel->relid, attno, type, -1, get_typcollation(type), 0);
}

/*
 * Does leaf partition `rel`'s bound leave none of its rows to `clauses` (over
 * its own columns: RestrictInfos or bare clauses) at the values they have
 * when the plan is made?  A stable expression - `now() - interval '400
 * days'` - proves nothing about the rows a run will see, so no partition is
 * left out for it (DESIGN.md §16), and the node locates such a leaf's filters
 * and finds they select nothing (DESIGN.md §27, "A partitioned fact table").
 * But the planner's ESTIMATES take such an expression at its present value
 * (estimate_expression_value()), and so does this: a price, never a plan.
 * The histogram cannot say it - past its ends it gives a hundredth of a bin,
 * a hundred rows of a million-row leaf - where the bound can.
 */
bool
lion_leaf_refuted_now(PlannerInfo *root, RelOptInfo *rel, List *clauses)
{
	RangeTblEntry *rte;
	Relation	relation;
	List	   *partqual;
	List	   *est = NIL;
	ListCell   *lc;

	if (rel->reloptkind != RELOPT_OTHER_MEMBER_REL || clauses == NIL)
		return false;
	rte = planner_rt_fetch(rel->relid, root);
	if (rte->rtekind != RTE_RELATION)
		return false;
	relation = table_open(rte->relid, NoLock);
	partqual = lion_leaf_partition_qual(relation, rel->relid);
	table_close(relation, NoLock);
	if (partqual == NIL)
		return false;
	foreach(lc, clauses)
	{
		Node	   *clause = (Node *) lfirst(lc);

		if (IsA(clause, RestrictInfo))
			clause = (Node *) ((RestrictInfo *) clause)->clause;
		if (contain_volatile_functions(clause) || contain_subplans(clause))
			continue;
		est = lappend(est, estimate_expression_value(root, clause));
	}
	return est != NIL && predicate_refuted_by(est, partqual, true);
}

/*
 * clauselist_selectivity() of `clauses` over `rel`, a table or a leaf
 * partition, with the ends of the columns they bound past the histogram read
 * from the lion index there (DESIGN.md §28, "The endpoint probe"), as the
 * count pushdown's own paths over one table are priced: a time range above
 * everything a yearly leaf holds is none of its rows, where the histogram
 * alone says a hundredth of a bin.  No enclosing scope does it for the
 * FK-side join's fact, whose paths are a join rel's, nor for a leaf.
 */
Selectivity
lion_probed_selectivity(PlannerInfo *root, RelOptInfo *rel, List *clauses)
{
	volatile Selectivity sel = 1.0;

	clauses = (List *) lion_vcol_unvar((Node *) clauses);
	if (!lion_probe_begin(root, rel, clauses))
		return clauselist_selectivity(root, clauses, 0, JOIN_INNER, NULL);
	PG_TRY();
	{
		sel = clauselist_selectivity(root, clauses, 0, JOIN_INNER, NULL);
	}
	PG_FINALLY();
	{
		lion_probe_end();
	}
	PG_END_TRY();
	return sel;
}

/*
 * The selectivity of `clauses` - the bounds of a range taken as a source,
 * written over the query's relation - in `rel`, which may be a leaf partition
 * of it (DESIGN.md §16): mapped onto the leaf's columns, through every level of
 * partitioning between them, so that the leaf's own statistics estimate them,
 * as they estimate the leaf's rows for core.  Estimated over the partitioned
 * table instead, a range on a column the leaves are partitioned by was the
 * same share of every leaf: `ts >= now() - interval '400 days'` a quarter of
 * each yearly leaf, where it is none of the older ones and all of the last.
 */
Selectivity
lion_rel_clauses_selectivity(PlannerInfo *root, RelOptInfo *rel,
							 List *clauses)
{
	Relids		varnos = pull_varnos(root, (Node *) clauses);
	int			varno;

	if (rel->reloptkind == RELOPT_OTHER_MEMBER_REL &&
		bms_get_singleton_member(varnos, &varno) &&
		(Index) varno != rel->relid)
		clauses = (List *)
			adjust_appendrel_attrs_multilevel(root, (Node *) clauses, rel,
											  find_base_rel(root, varno));
	if (lion_leaf_refuted_now(root, rel, clauses))
		return 0.0;
	return lion_probed_selectivity(root, rel, clauses);
}

/*
 * WHAT COLLECTING A RANGE COSTS (DESIGN.md §32, "What collecting a range
 * costs", 2026-09-29): the union lion_range_collect() builds of the sets its
 * walk hands out, over and above the walk (lion_cost_range_sum()).  Every
 * container of every set is ORed into the union's container of its key - a
 * hash lookup, and the OR in place or the members set aside - at
 * LION_CONTAINER_COST; what makes a collection dear is the FOLDS, at
 * LION_RANGE_FOLD_COST each: a container of the union that is not a bitset
 * taking what came for it through a bitset image, or merging a larger one in,
 * and a union made a bitset to OR into being optimized again
 * (lion_range_union_cb()).  How many there are depends on how the column lies
 * in the heap, which the correlation's square interpolates between, as
 * cost_index() does:
 *
 *	- IN ORDER, the rows of a set are a run of the heap and the union of a
 *	  container key a RUN, which is no ARRAY and no bitset: the container
 *	  that comes to a key after its first WIDENS it into a bitset, every one
 *	  after that is ORed in place, and it is optimized back once the walk is
 *	  over - LION_RANGE_WIDEN_FOLDS for each key that takes more than one.
 *	  A set's rows span one container key more than they fill.
 *	- SCATTERED, every set has a container at nearly every container key of
 *	  the heap (lion_containers_for()), and the union of a key ends up with
 *	  its rows' share of the range.  Containers of at most
 *	  LION_RANGE_UNION_PEND_MIN members wait and are folded in when the
 *	  waiting members come to half the union's, so the union grows by half at
 *	  least between two folds: 1 + log1.5(members / 32) folds a key, until it
 *	  is a bitset past LION_ARRAY_MAX_CARD members - or a RUN, which is
 *	  widened into one - and takes the rest in place.  Larger ones are merged
 *	  in one by one until then, each merge moving the union's members too
 *	  (LION_AND_MEMBER_COST each, half the bitset's worth on average).  Which
 *	  are larger is a Poisson count's tail (lion_poisson_above()): a set's
 *	  rows fall at a key at random.
 *
 * The model used to charge a union the walk and a cpu_operator_cost a
 * container of it, and so priced a range of two million scattered rows at a
 * tenth of the time the node spent collecting it (DESIGN.md §27, "A
 * partitioned fact table").  Until the unions were widened (2026-09-30) the
 * union of a key in heap order was folded once for every container after
 * its first, as it was then priced, and a dense union of scattered rows -
 * a RUN, a heap block's rows being a run - was folded once for every
 * container, which the model never priced: 1.9 million rows took 12.7 s,
 * priced at a third of a second (2026-09-29 review).
 */
/*
 * P(X > k) for X a Poisson count of mean m: what share of the container keys
 * a set of m rows a key on average, spread at random, has more than k rows
 * at (lion_cost_range_union()).
 */
static double
lion_poisson_above(double m, int k)
{
	double		term;
	double		cdf;
	int			i;

	if (m <= 0.0)
		return 0.0;
	if (m > 4.0 * k + 50.0)
		return 1.0;
	term = exp(-m);
	cdf = term;
	for (i = 1; i <= k; i++)
	{
		term *= m / i;
		cdf += term;
	}
	return Min(Max(1.0 - cdf, 0.0), 1.0);
}

static Cost
lion_cost_range_union(double heap_pages, double tuples, double rows,
					  const LionRangeWalk *w)
{
	double		ckeys = Max(heap_pages / LION_BLOCKS_PER_CONTAINER, 1.0);
	double		perckey = Max(tuples / ckeys, 1.0);
	double		c2 = w->corr * w->corr;
	double		escat = Max(lion_containers_for(heap_pages, w->entryrows), 1.0);
	double		sscat = Max(lion_containers_for(heap_pages, w->bucketrows), 1.0);
	double		eord = Min(escat, 1.0 + w->entryrows / perckey);
	double		sord = Min(sscat, 1.0 + w->bucketrows / perckey);
	double		inord = w->entries * eord + w->sums * sord;
	double		inscat = w->entries * escat + w->sums * sscat;
	double		uord = Min(ckeys, Max(1.0, rows / perckey));
	double		uscat = Max(lion_containers_for(heap_pages, rows), 1.0);
	double		ukey = Min(rows / uscat, (double) LION_ARRAY_MAX_CARD);
	double		small = 0.0;
	double		big = 0.0;
	double		bigrows = 0.0;
	double		pbig;
	double		folds;
	double		merges = 0.0;
	Cost		ordered;
	Cost		scattered;

	/* in order: each key that takes a second container is widened once */
	ordered = inord * LION_CONTAINER_COST +
		Min(uord, Max(0.0, inord - uord)) * LION_RANGE_WIDEN_FOLDS *
		LION_RANGE_FOLD_COST;

	/*
	 * scattered: the small containers wait, the larger ones are merged - a
	 * set's rows fall at a container key as a Poisson count does, so some of
	 * a set of 28 rows a key on average are larger than 32
	 */
	pbig = lion_poisson_above(w->entryrows / ckeys, LION_RANGE_UNION_PEND_MIN);
	small += w->entries * escat * (1.0 - pbig);
	big += w->entries * escat * pbig;
	bigrows += w->entries * escat * pbig *
		Max(w->entryrows / escat, LION_RANGE_UNION_PEND_MIN + 1.0);
	if (w->sums > 0.0)
	{
		pbig = lion_poisson_above(w->bucketrows / ckeys,
								  LION_RANGE_UNION_PEND_MIN);
		small += w->sums * sscat * (1.0 - pbig);
		big += w->sums * sscat * pbig;
		bigrows += w->sums * sscat * pbig *
			Max(w->bucketrows / sscat, LION_RANGE_UNION_PEND_MIN + 1.0);
	}
	folds = Min(small / uscat,
				1.0 + log(Max(ukey, (double) LION_RANGE_UNION_PEND_MIN) /
						  LION_RANGE_UNION_PEND_MIN) / log(1.5));
	if (big > 0.0)
		merges = Min(big / uscat, ukey / Max(bigrows / big, 1.0) + 1.0);
	scattered = inscat * LION_CONTAINER_COST +
		uscat * (folds + merges) * LION_RANGE_FOLD_COST +
		uscat * merges * 0.5 * ukey * LION_AND_MEMBER_COST;

	return scattered + (ordered - scattered) * c2;
}

/*
 * The sides a range's bounds close, one or two: a range with a bound on one
 * side only - `ts >= now() - interval '30 days'` - has a partial bucket at
 * that end alone, whose keys its walk reads one by one, and the summaries run
 * to the column's other end (DESIGN.md §32, "Readers").  Two for anything
 * not a plain comparison of the column.
 */
static double
lion_range_sides(IndexOptInfo *idx, AttrNumber col, List *bounds)
{
	bool		lower = false;
	bool		upper = false;
	ListCell   *lc;

	foreach(lc, bounds)
	{
		Node	   *clause = (Node *) lfirst(lc);
		OpExpr	   *op;
		Oid			opno;

		if (IsA(clause, RestrictInfo))
			clause = (Node *) ((RestrictInfo *) clause)->clause;
		if (!IsA(clause, OpExpr) || list_length(((OpExpr *) clause)->args) != 2)
			return 2.0;
		op = (OpExpr *) clause;
		opno = op->opno;
		if (!IsA(lion_strip((Node *) linitial(op->args)), Var))
			opno = get_commutator(opno);
		if (!OidIsValid(opno))
			return 2.0;
		switch (get_op_opfamily_strategy(opno, idx->opfamily[col - 1]))
		{
			case LION_STRAT_LT:
			case LION_STRAT_LE:
				upper = true;
				break;
			case LION_STRAT_GE:
			case LION_STRAT_GT:
				lower = true;
				break;
			default:
				return 2.0;
		}
	}
	return (lower && upper) ? 2.0 : 1.0;
}

/*
 * A RANGE TAKEN AS A SOURCE (DESIGN.md §32, "A range as a source"): the rows
 * whose key lies in it, which the executor collects into memory once per
 * relation - the walk of the range, summaries and all, priced as the walk of
 * a summed range with nothing to AND and nothing to recheck, and the union it
 * builds of them (lion_cost_range_union()) - and then reads as any other set.
 * What it takes is a container per container key its rows lie in: about two
 * bytes a row until those fill up and at most a bitset each when they lie all
 * over the heap, a RUN each when they lie in its order - the column's
 * correlation interpolates - and each container's entry in the union's hash
 * table.  One that would not fit in a hash table's memory
 * (get_hash_memory_limit(), which the executor shares among the relation's
 * ranges) is walked instead, at every count it is part of - `counts` of them -
 * and an OR's leaf, which cannot be walked, is priced out of the plan.
 */
Cost
lion_cost_range_source(PlannerInfo *root, RelOptInfo *rel, IndexOptInfo *idx,
					   AttrNumber col, List *bounds, Selectivity sel,
					   double counts, bool inor)
{
	double		heap_pages = Max((double) rel->pages, 1.0);
	double		tuples = Max(rel->tuples, 1.0);
	double		rows = tuples * sel;
	double		conts = lion_containers_for(heap_pages, rows);
	double		ckeys = Max(heap_pages / LION_BLOCKS_PER_CONTAINER, 1.0);
	double		runs = Min(ckeys, Max(1.0, rows * ckeys / tuples));
	double		bytes = Min(rows * sizeof(uint16) +
							conts * LION_CONTAINER_HDRSZ,
							conts * LION_CONTAINER_MAX_SIZE);
	Var		   *var = lion_index_col_var(root, rel, idx, col);
	LionRangeWalk w;
	double		c2;
	Cost		walk;

	if (var == NULL)
		return disable_cost;
	w.sides = lion_range_sides(idx, col, bounds);
	walk = lion_cost_range_sum(root, rel, idx, col, var, sel, NIL, NIL, NIL,
							   &w);
	c2 = w.corr * w.corr;
	bytes += (runs * LION_RANGE_RUN_BYTES - bytes) * c2;
	bytes += (conts + (runs - conts) * c2) * LION_RANGE_UNION_ENTRY_BYTES;
	if (bytes <= (double) get_hash_memory_limit())
		return walk + lion_cost_range_union(heap_pages, tuples, rows, &w);
	if (inor)
		return disable_cost;
	return walk * Max(counts, 1.0);
}

/*
 * The heap recheck of a range walk (DESIGN.md §28): `tids` candidates on the
 * pages the visibility map cannot vouch for, counted `entries` at a time.  The
 * visibility cache fetches a dirty page once per query - but it answers a page
 * only from its second visit on, and each count flushes its own recheck batch,
 * so a page that holds candidates of several entries is fetched for each of
 * them until the cache has it: never more than twice, and never more often
 * than the entries' candidates lie on pages.  How many pages one entry's
 * candidates lie on follows the column's order, as cost_index() interpolates
 * it with the correlation's square: one per row scattered, their share of the
 * heap in order.
 */
static Cost
lion_range_recheck(PlannerInfo *root, RelOptInfo *rel, double tids,
				   double entries, double corr)
{
	double		heap_pages = Max((double) rel->pages, 1.0);
	double		tuples = Max(rel->tuples, 1.0);
	double		dirtyfrac = 1.0 - rel->allvisfrac;
	double		dirty_pages = Min(heap_pages * dirtyfrac, heap_pages);
	double		rowsper;
	double		scattered;
	double		inorder;
	double		perentry;
	double		pages;

	if (tids <= 0.0 || dirtyfrac <= 0.0)
		return 0.0;

	/* the candidates of one entry, dirty pages or not */
	rowsper = tids / dirtyfrac / Max(entries, 1.0);
	scattered = Min(rowsper, heap_pages);
	inorder = Max(1.0, rowsper * heap_pages / tuples);
	perentry = scattered + (inorder - scattered) * corr * corr;

	pages = Min(tids, dirty_pages);
	pages = Min(Min(tids, entries * perentry * dirtyfrac), 2.0 * pages);

	return pages * lion_heap_page_cost(root, rel, pages, heap_pages) +
		tids * LION_RECHECK_TID_COST;
}

/*
 * Will a GROUP BY's WHERE be collected into one set (DESIGN.md §10, "The
 * WHERE sets, collected once")?  lion_where_describe()'s rule, from the
 * planner's sources: two or more, or one that is a union; a lone set only
 * where no count would keep a copy of it, being too large to materialize
 * (lion_posting_set_rewalked()).  Over groups that span the heap the
 * executor collects before the first group; over groups of a few rows, once
 * they have read as much as collecting reads, which prices about the same.
 */
static bool
lion_cost_where_collected(int nsrc, const double *members,
						  const double *containers, const bool *isunion)
{
	if (nsrc <= 0)
		return false;
	if (nsrc > 1 || isunion[0])
		return true;
	return containers[0] > (double) LION_MATERIALIZE_MAX_CONTAINERS &&
		members[0] > (double) (LION_MATERIALIZE_MAX_BYTES / sizeof(uint16));
}

/*
 * The page each set reads again at a range's first key: the leaf its descent
 * lands on, as I/O like every page the model charges - with the entry decoded
 * again (LION_ENTRY_COUNT_COST) and the descent itself (LION_PROBE_COST).
 * Fitted on the synthetic repro to what one participant counting a GROUP BY
 * as 16 ranges took over the same count made whole: 83 us a range for 20
 * groups under a WHERE of 1,500 rows, and 500 us for 150 groups under a WHERE
 * of four clauses - 42 and 250 units a range, priced at 35 and 245.
 */
#define LION_RANGE_DESCENT_PAGES	1.0

/*
 * What the phases of lion_cost_count_rel() share: its arguments, the sizes of
 * the relation, the arrays it keeps per WHERE clause and per source of the
 * AND, and the running terms its cost is summed from.
 */
typedef struct LionCountRelCost
{
	/* lion_cost_count_rel()'s arguments */
	PlannerInfo *root;
	RelOptInfo *rel;
	IndexOptInfo *groupidx;
	AttrNumber	groupcol;
	IndexOptInfo *groupidx2;
	AttrNumber	groupcol2;
	List	   *whereidx;
	List	   *wherecol;
	List	   *whereclauses;
	List	   *wherekinds;
	List	   *ors;
	double		numgroups;
	double		outer_entries;
	double		inner_entries;
	int			distinct;
	double		drivefrac;
	Var		   *rangevar;
	bool		rangesum;
	LionRangeCost *rc;

	/* the relation */
	double		heap_pages;
	double		dirtyfrac;
	double		matching;
	double		tuples;

	/* the terms of the cost */
	double		random_pages;	/* directory leaves, one a lookup (§29.11) */
	Cost		descent_cost;	/* comparisons on the way down (§21) */
	double		seq_pages;		/* container chains, read in order */
	Cost		seq_cost;		/* ... and their price, each at its index's
								 * third rung (lion_index_page_cost()) */
	Cost		lookup_cost;	/* an IN list's bucket pages, in order */
	Cost		probe_cost;		/* what the SOUGHT sources read (§22) */
	Cost		read_cpu;		/* containers read whole: unions, lists */
	Cost		merge_cpu;		/* the AND's driver and probes (§10) */
	double		merge_ops;		/* comparisons a union of k sets makes */
	double		recheck_tids;
	double		recheck_pages;
	double		probed_pages;	/* the pages the sought sources read */
	Cost		union_read;		/* read_cpu of the union sources' sets */
	double		union_ops;		/* merge_ops of their unions */
	Cost		pair_cost;		/* §20: the (outer, inner) group pairs */
	Cost		rangecost;		/* §28: the walk of a summed range */
	Cost		rangesrc_cost;	/* §32: collecting the ranges taken as
								 * sources */

	/* the WHERE clauses, by their position in the lists */
	int			nclause;
	int			ci;				/* the clause being priced */
	int		   *orgrp;			/* each clause's OR restriction, or -1 */
	double	   *clausesel;		/* each clause's own selectivity, for §19 */
	LionSetClause *clauseset;	/* its sets as the AND prices them (§22) */
	IndexOptInfo **clauseindex; /* ... in this index, for its lookup */
	int		   *clausesrc;		/* the AND source it is part of, or -1 */
	int		   *rangelead;		/* each clause's range source, or -1 */

	/* the sources of the AND */
	int			nsrc;
	int			driver;			/* the source that drives the leapfrog */
	double	   *srcmembers;		/* members of each AND source */
	double	   *srccontainers;	/* and the containers they lie in */
	bool	   *srcunion;		/* is it a union of several sets? */
	double	   *srcsets;		/* ... of how many */
	double	   *srcnsets;		/* the sets an AND meets it as the union of */
	double	   *srcsetcont;		/* ... and the containers they lie in */
	double	   *srcsetpages;	/* ... and the posting pages they fill */
	double	   *srcprobes;		/* how often each is sought */

	/* the groups */
	bool		groupdrive;		/* §15: the IN list is the GROUP BY driver */
	int			inlistci;
	double		ingroups;		/* groups the node really emits */
	double		recheckshare;	/* §26: what the existence tests recheck */
	double		listrows;		/* rows of one entry of a group-driving list */
	double		walked;			/* counts the merge runs: groups, pairs */
	bool		whereonce;		/* the WHERE is collected once and the
								 * groups counted in batches (§10) */
	double		topk;			/* §36: the candidates counted, or 0 */
	double		topkrows;		/* ... and the rows of their entries */
} LionCountRelCost;

/*
 * The arrays lion_cost_count_rel() keeps per WHERE clause and per source of
 * the AND, zeroed, and the OR restriction of each clause.
 */
static void
lion_count_rel_alloc(LionCountRelCost *c)
{
	c->clausesel = (double *) palloc0(sizeof(double) * Max(c->nclause, 1));
	c->clauseset = (LionSetClause *) palloc0(sizeof(LionSetClause) *
											 Max(c->nclause, 1));
	c->clauseindex = (IndexOptInfo **) palloc0(sizeof(IndexOptInfo *) *
											   Max(c->nclause, 1));
	c->clausesrc = (int *) palloc0(sizeof(int) * Max(c->nclause, 1));
	c->srcmembers = (double *) palloc0(sizeof(double) * Max(c->nclause, 1));
	c->srccontainers = (double *) palloc0(sizeof(double) *
										  Max(c->nclause, 1));
	c->srcunion = (bool *) palloc0(sizeof(bool) * Max(c->nclause, 1));
	c->srcsets = (double *) palloc0(sizeof(double) * Max(c->nclause, 1));
	c->srcnsets = (double *) palloc0(sizeof(double) * Max(c->nclause, 1));
	c->srcsetcont = (double *) palloc0(sizeof(double) * Max(c->nclause, 1));
	c->srcsetpages = (double *) palloc0(sizeof(double) *
										Max(c->nclause, 1));
	c->srcprobes = (double *) palloc0(sizeof(double) *
									  (Max(c->nclause, 1) + 2));
	c->orgrp = lion_or_group_map(c->ors, c->nclause);
}

/*
 * A range taken as a source (DESIGN.md §32), priced at the first of its
 * bounds and added to the sources of the AND.  c->ci is the clause's
 * position, and is left past it.
 */
static void
lion_count_rel_range_source(LionCountRelCost *c, IndexOptInfo *idx,
							AttrNumber col)
{
	LionSetClause *sc = &c->clauseset[c->ci];
	Selectivity sel;
	List	   *bounds = NIL;
	double		clc;
	int			j;

	if (c->rangelead[c->ci] != c->ci)
	{
		c->ci++;
		return;
	}
	for (j = c->ci; j < c->nclause; j++)
		if (c->rangelead[j] == c->ci)
			bounds = lappend(bounds, list_nth(c->whereclauses, j));
	sel = lion_rel_clauses_selectivity(c->root, c->rel, bounds);
	c->clausesel[c->ci++] = sel;

	c->rangesrc_cost += lion_cost_range_source(c->root, c->rel, idx, col,
											   bounds, sel,
											   Max(c->ingroups, 1.0),
											   c->orgrp[c->ci - 1] >= 0);
	list_free(bounds);
	sc->nkeys = 1.0;
	sc->leaves = 1.0;
	sc->idxpages = Max((double) idx->pages, 1.0);

	/*
	 * As one set of the AND, it drives the merge or is probed at the
	 * driver's keys (lion_merge_cpu_cost()); as an OR's leaf, its
	 * containers are all read into the union.
	 */
	clc = lion_key_containers(c->root, c->rel, idx, col, c->tuples * sel);
	if (c->orgrp[c->ci - 1] >= 0)
	{
		Cost		readleaf = clc * (LION_CONTAINER_COST + LION_MEMBER_COST *
									  Min(c->tuples * sel / clc,
										  LION_MEMBER_CAP));

		c->read_cpu += readleaf;
		c->union_read += readleaf;
	}
	c->clausesrc[c->ci - 1] = (c->orgrp[c->ci - 1] >= 0) ?
		c->orgrp[c->ci - 1] : c->nsrc++;
	c->srcmembers[c->clausesrc[c->ci - 1]] += c->tuples * sel;
	c->srccontainers[c->clausesrc[c->ci - 1]] += clc;
	c->srcsets[c->clausesrc[c->ci - 1]] += 1.0;
	c->srcnsets[c->clausesrc[c->ci - 1]] += 1.0;
	c->srcsetcont[c->clausesrc[c->ci - 1]] += clc;
	if (c->orgrp[c->ci - 1] >= 0)
		c->srcunion[c->clausesrc[c->ci - 1]] = true;
}

/*
 * A WHERE clause's sets (lion_cost_set_clause()): their lookups charged, and
 * the clause added to the source of the AND it is part of.  c->ci is the
 * clause's position, and is left past it.
 */
static void
lion_count_rel_set_clause(LionCountRelCost *c, IndexOptInfo *idx,
						  AttrNumber col, Node *clause)
{
	LionSetClause *sc = &c->clauseset[c->ci];
	Selectivity sel;
	double		nkeys;

	/*
	 * The clause's sets, priced as every AND of them is priced
	 * (lion_cost_set_clause()): its lookups - a directory leaf and a
	 * descent, or an IN list's leaves in key order and a search per value
	 * - are charged here, and what a walk of them reads once the driver
	 * is known, below.
	 */
	lion_cost_set_clause(c->root, c->rel, idx, col, clause, sc);
	sel = sc->sel;
	nkeys = sc->nkeys;
	c->clauseindex[c->ci] = idx;
	c->clausesel[c->ci++] = sel;
	c->lookup_cost += sc->lookup;
	c->descent_cost += sc->descent;

	/*
	 * A single set ANDed with the others is read only where it drives the
	 * merge, and PROBED at the driver's container keys elsewhere
	 * (DESIGN.md §22), which lion_merge_cpu_cost() prices below.  The
	 * containers of an OR leaf and of an IN list's sets are all READ: a
	 * union is built of them (§15, §19) or they are summed.  A list that
	 * drives the groups is read one entry a group, below.
	 */
	if (!(c->groupdrive && c->ci - 1 == c->inlistci) &&
		(c->orgrp[c->ci - 1] >= 0 || nkeys > 1.0))
	{
		c->read_cpu += sc->readall;
		c->union_read += sc->readall;
	}

	/*
	 * Which source of the AND this clause belongs to: the union of its OR
	 * restriction, or one of its own.  A list that drives the groups is
	 * not a source at all - each group IS one of its entries - so it
	 * neither drives the leapfrog nor is sought by it.
	 */
	if (c->groupdrive && c->ci - 1 == c->inlistci)
		c->clausesrc[c->ci - 1] = -1;
	else
	{
		c->clausesrc[c->ci - 1] = (c->orgrp[c->ci - 1] >= 0) ?
			c->orgrp[c->ci - 1] : c->nsrc++;
		c->srcmembers[c->clausesrc[c->ci - 1]] += sc->members;
		c->srccontainers[c->clausesrc[c->ci - 1]] += sc->containers;
		c->srcsets[c->clausesrc[c->ci - 1]] += nkeys;
		c->srcnsets[c->clausesrc[c->ci - 1]] += sc->nsets;
		c->srcsetcont[c->clausesrc[c->ci - 1]] += sc->setcontainers;
		c->srcsetpages[c->clausesrc[c->ci - 1]] += sc->pages;
		if (c->orgrp[c->ci - 1] >= 0 || nkeys > 1.0)
			c->srcunion[c->clausesrc[c->ci - 1]] = true;
	}

	/*
	 * Does the merge build this clause's union?  ... unless there is no
	 * union to build.  A list that drives the groups is not a source at
	 * all, and a list that is the only positive source is SUMMED instead
	 * - its entries counted one at a time and added up (the disjoint-sum
	 * short-circuit, DESIGN.md §15), which leaves the per-element lookup
	 * and container work priced above and nothing for a merge that does
	 * not happen: what lets a thousand-value list on a high-cardinality
	 * column be chosen, 1.5 ms against the B-tree's 4.5 at one million
	 * rows.  But only while the sum is the cheaper of the two, which is
	 * lion_sum_is_cheaper() in the executor and the same test from
	 * estimates here: dense entries, enough of them for the merge's
	 * bitset image, and the merge runs after all.
	 */
	if (nkeys > 1.0)
	{
		double		ckeys = Max(c->heap_pages / LION_BLOCKS_PER_CONTAINER,
								1.0);
		bool		merged = true;

		if (c->ci - 1 == c->inlistci)
			merged = (!c->groupdrive &&
					  nkeys >= (double) LION_OR_BITSET_MIN &&
					  (c->tuples * sel / nkeys) / ckeys >
					  (double) LION_SUM_MAX_DENSITY);

		if (merged)
		{
			c->merge_ops += sc->unionops;
			c->union_ops += sc->unionops;
		}
	}

	/*
	 * A list that drives the groups is not a source: each group is one of
	 * its entries, and the rest of the list has nothing to say about that
	 * group's rows.  So it is not intersected with anything, and the
	 * per-group work below is over the listed values rather than over
	 * every entry of the index.
	 */
	if (c->groupdrive && c->ci - 1 == c->inlistci)
	{
		c->ingroups = Min(nkeys, c->ingroups);
		c->listrows = c->tuples * sel / nkeys;
	}
}

/*
 * Number the sources of the AND, and price the WHERE clauses' lookups clause
 * by clause.
 */
static void
lion_count_rel_clauses(LionCountRelCost *c)
{
	ListCell   *lc1;
	ListCell   *lc2;
	ListCell   *lc3;
	ListCell   *lc4;

	/*
	 * The sources of the AND: one per OR restriction (a union is one source,
	 * DESIGN.md §19) and one per positive clause outside them.  Which of them
	 * drives the leapfrog join decides what the others read, so they are
	 * numbered here and the page terms are charged once the driver is known.
	 */
	c->nsrc = list_length(c->ors);
	for (c->ci = 0; c->ci < c->nclause; c->ci++)
		c->clausesrc[c->ci] = -1;
	c->ci = 0;
	c->rangelead = lion_rangesrc_leaders(c->whereidx, c->wherecol,
										 c->wherekinds, c->ors, c->nclause);

	forfour(lc1, c->whereidx, lc2, c->whereclauses, lc3, c->wherekinds,
			lc4, c->wherecol)
	{
		IndexOptInfo *idx = (IndexOptInfo *) lfirst(lc1);
		Node	   *clause = (Node *) lfirst(lc2);
		double		nkeys;

		if (!LION_CLAUSE_IS_POSITIVE(lfirst_int(lc3)))
		{
			/*
			 * `IS NOT NULL` selects no rows of its own; it has been left out
			 * of this estimate since before partitions existed.
			 */
			c->ci++;
			continue;
		}

		/*
		 * Nor does a multi-key query the count will answer with every row
		 * (DESIGN.md §17, "A query known only at run time"): a value no key
		 * narrows, or one with no estimate to go by.  The executor locates no
		 * set for it - it is a source with nothing to subtract - and tests it
		 * on each candidate the other sources leave (lion_locate_where(),
		 * lion_build_filter()), which lion_cost_recheck() prices; it neither
		 * drives the AND nor is sought in it, as the scan's AND of sets leaves
		 * the same query out (lion_scan_set_quals()).  Whether it is such a
		 * query is lion_multikey_cost_mode()'s answer, the one the recheck is
		 * priced by: a superset of its rows, as the count extracts a run-time
		 * value, and not the scan's exact extraction, which would also leave
		 * out a phrase or a NULL element that the count answers from the
		 * other lexemes' or elements' sets.  An OR's leaf is a literal
		 * (lion_analyze_leaf()) and is never one.
		 */
		if (lfirst_int(lc3) == LION_CLAUSE_MULTI && c->orgrp[c->ci] < 0 &&
			lion_multikey_cost_mode(idx, (AttrNumber) lfirst_int(lc4), clause,
									&nkeys) == LION_QMODE_ALL)
		{
			c->ci++;
			continue;
		}

		/*
		 * A range taken as a source (DESIGN.md §32): one source for all the
		 * bounds of its conjunction, priced once, by the first of them.  It
		 * is read from memory, so it is sought at no page cost; what it costs
		 * is collecting it (lion_cost_range_source()).
		 */
		if (lfirst_int(lc3) == LION_CLAUSE_RANGESRC)
		{
			lion_count_rel_range_source(c, idx, (AttrNumber) lfirst_int(lc4));
			continue;
		}

		lion_count_rel_set_clause(c, idx, (AttrNumber) lfirst_int(lc4),
								  clause);
	}
}

/*
 * The merge each OR across columns makes of its arms' sets (DESIGN.md §19);
 * frees the per-clause arrays no later phase reads.
 */
static void
lion_count_rel_or_unions(LionCountRelCost *c)
{
	ListCell   *lc1;
	int			i;

	/*
	 * An OR across columns (DESIGN.md §19) is the union of its arms, and a
	 * union of k sub-cursors costs what §15's IN list does: log2(k)
	 * comparisons per member, over the members of all of them together.  The
	 * lookups and the chains of its leaves have already been charged above,
	 * one per leaf, exactly as if they had been separate clauses; the term
	 * here is the merge they take part in and nothing else.
	 */
	foreach(lc1, c->ors)
	{
		List	   *one = (List *) lfirst(lc1);
		int			first = linitial_int(one);
		int			narms = lsecond_int(one);
		double		members = 0;
		int			nleaves = 0;

		for (i = 0; i < narms; i++)
			nleaves += list_nth_int(one, 2 + i);
		for (i = first; i < first + nleaves && i < c->nclause; i++)
			members += c->tuples * c->clausesel[i];

		if (nleaves > 1)
		{
			c->merge_ops += members * log2((double) nleaves);
			c->union_ops += members * log2((double) nleaves);
		}
	}
	pfree(c->clausesel);
	pfree(c->orgrp);
	pfree(c->rangelead);
}

/*
 * The source that drives the leapfrog - the one with the fewest members - and
 * each source's containers bounded by the heap's container keys.
 */
static void
lion_count_rel_driver(LionCountRelCost *c)
{
	/*
	 * WHAT THE SOURCES READ (DESIGN.md §22).  The same leapfrog decides it: the
	 * DRIVER - the source with the fewest members - is the only one walked end
	 * to end, and it pays for its whole share of the index's container pages,
	 * as every source did before the posting tree existed.  Every other source
	 * is SOUGHT to the container keys the driver produces, so it pays for the
	 * pages those probes touch (lion_probed_pages()) and never for more than a
	 * walk of it would have cost.
	 *
	 * That bound is the whole of this section's planner change.  `c20k = 77 AND
	 * c200 = 17 AND c2 = 1` at one million rows probes `c2` at about 50 of the
	 * heap's 300 container keys, a descent each, and the count really does
	 * touch 118 of that index's buffers rather than the 154 a walk of the set
	 * takes; charging it the whole 152-page walk asked 168.8 cost units for a
	 * count the node answers in 0.25 ms, against 117.6 now.  The BitmapAnd it
	 * still loses to is priced at 62.1 and takes 0.88 ms - what remains between
	 * them is the unit and not the count of pages, which DESIGN.md §22 records
	 * as the open item.
	 *
	 * With a GROUP BY the probing happens once per group - the group's own
	 * posting set is a source like any other, and the smaller one of it and the
	 * WHERE sources drives - so the probes are counted over all the groups
	 * together.  That is more probes than a WHERE set has pages many times
	 * over, which is exactly why a grouped count is priced as it was: one read
	 * of each WHERE set, which is also what the materialized copy of it costs
	 * (DESIGN.md §9 - the sets a GROUP BY intersects with every group are
	 * copied out on their second use and are probed in memory after that).
	 */
	/*
	 * Which one drives is decided from the MEMBERS, as lion_run_merge() decides
	 * it from the entries' `ntids` - not from the containers, which for a union
	 * of k sets are counted k times over and would hand the merge to whichever
	 * source happens to lie in the fewest of them.  What the driver then costs
	 * the others is its CONTAINER KEYS, of which there are no more than the
	 * heap has.
	 */
	if (c->nsrc > 0)
	{
		double		ckeys = Max(c->heap_pages / LION_BLOCKS_PER_CONTAINER,
								1.0);
		int			s;

		for (s = 0; s < c->nsrc; s++)
		{
			if (c->driver < 0 || c->srcmembers[s] < c->srcmembers[c->driver])
				c->driver = s;
			c->srccontainers[s] = Min(c->srccontainers[s], ckeys);
		}
	}
}

/*
 * The groups of a walk counted together against the WHERE collected once
 * (DESIGN.md §10), and what a range of a parallel walk pays again of it.
 */
static void
lion_count_rel_where_once(LionCountRelCost *c, double *mem, double *keys,
						  double gm)
{
	/*
	 * THE GROUPS OF THE WALK, COUNTED TOGETHER (DESIGN.md §10).
	 * The WHERE is collected once - a merge of its sources as an
	 * ungrouped count makes it, and a container of the copy
	 * written at each key it keeps - and the groups are counted
	 * against the copy in batches of LION_GROUP_BATCH_MAX, one
	 * walk of container keys a batch (lion_count_groups_copy()):
	 * each group's container at a key the copy has is read and
	 * its members tested against the copy's, made a bitset image
	 * once a batch, and a batch's cursors stand on a heap, a sift
	 * of log2(batch) a container.  A group is walked only as far
	 * as the copy's keys reach, which is all of them where the
	 * WHERE is dense.  The unions of the WHERE are built once, in
	 * the collection.  Priced as each group's merge with the
	 * WHERE's sets probed in memory, the synthetic repro's GROUP BY
	 * of 150 groups under four filters cost 217,000 for 68 ms; it
	 * now costs about the 500 units a millisecond of §10's
	 * reference.
	 */
	double		wkeys = lion_containers_for(c->heap_pages, c->matching);
	double		batch = Min(c->walked, (double) LION_GROUP_BATCH_MAX);
	double		visits;
	double		collect;
	double		per;
	int			s;

	for (s = 0; s < c->nsrc; s++)
		wkeys = Min(wkeys, keys[s]);
	wkeys = Max(wkeys, 1.0);
	visits = Min(keys[c->nsrc], wkeys);

	collect = lion_merge_cpu_cost(c->nsrc, mem, keys, NULL, c->tuples,
								  lion_probe_rel_factor(c->root, c->rel),
								  c->srcprobes) +
		wkeys * LION_FKJOIN_COPY_CONTAINER_COST;
	per = visits * (LION_CONTAINER_COST +
					LION_MEMBER_COST *
					Min(gm / Max(keys[c->nsrc], 1.0), LION_MEMBER_CAP) +
					log2(Max(batch, 2.0)) * cpu_operator_cost);
	c->merge_cpu = collect + c->walked * per +
		ceil(c->walked / batch) * wkeys *
		(LION_CONTAINER_COST + LION_MEMBER_COST *
		 Min(c->matching / wkeys, LION_MEMBER_CAP));
	c->whereonce = true;

	/*
	 * ... and what a range of a parallel one pays again of it: the leaves
	 * each set reads again, pages of the walked index (its third rung, §39)
	 */
	if (c->rc != NULL)
	{
		Cost		leaf = LION_RANGE_DESCENT_PAGES *
			lion_index_page_cost(c->root,
								 (c->groupidx != NULL) ?
								 (double) c->groupidx->pages : 0.0,
								 seq_page_cost);

		c->rc->batched = true;
		c->rc->perrange = c->walked * (LION_ENTRY_COUNT_COST +
									   LION_PROBE_COST + leaf) +
			c->nsrc * (LION_PROBE_COST + leaf);
	}
}

/*
 * The merges of a count over one GROUP BY column, one a group - or the WHERE
 * collected once and the groups counted against it - and each count's own
 * set-up.
 */
static void
lion_count_rel_one_column(LionCountRelCost *c, double *mem, double *keys,
						  bool *inmem, double gm, double unionsets)
{
	double		per;
	double		share = 1.0;
	int			s;

	/*
	 * The count(DISTINCT k) walk over k's entries (DESIGN.md §26)
	 * tests each entry for ONE visible row and stops there, so it
	 * reads the share of each entry's intersection
	 * lion_exists_fraction() expects - one container or two when the
	 * entries are dense, all of it when the WHERE leaves most of them
	 * empty - and rechecks that share of the candidates.
	 */
	if (c->distinct == LION_DISTINCT_EXISTS)
	{
		double		drvkeys = keys[c->nsrc];

		/* the test walks the merge's driver: the smallest source */
		for (s = 0; s < c->nsrc; s++)
			if (mem[s] < mem[c->nsrc])
				drvkeys = Min(drvkeys, keys[s]);
		share = lion_exists_fraction(drvkeys, c->matching / c->walked);
		c->recheckshare = share;
	}
	if (c->distinct == LION_DISTINCT_NONE && !c->groupdrive &&
		c->walked >= 2.0 &&
		lion_cost_where_collected(c->nsrc, c->srcmembers, c->srccontainers,
								  c->srcunion))
	{
		lion_count_rel_where_once(c, mem, keys, gm);
	}
	else
	{
		per = lion_merge_cpu_cost(c->nsrc + 1, mem, keys, inmem, c->tuples,
								  1.0, c->srcprobes) +
			unionsets * LION_UNION_SET_COST;
		c->merge_cpu = c->walked * per * share;
	}

	/*
	 * ... and each count's own set-up and tear-down (a memory context,
	 * the sources' cursors, the visibility-map state): a distinct
	 * walk's test prices it (LION_DISTINCT_TEST_COST, below), a range's
	 * walk its entry (LION_RANGE_ENTRY_COST).
	 */
	if (c->distinct == LION_DISTINCT_NONE && c->rangevar == NULL)
		c->merge_cpu += c->walked * (c->groupdrive ? LION_LIST_GROUP_COST :
									 LION_ENTRY_COUNT_COST);
}

/*
 * The merges of a count over two GROUP BY columns (DESIGN.md §20): one for
 * each (outer, inner) pair of their entries.
 */
static void
lion_count_rel_two_columns(LionCountRelCost *c, double *mem, double *keys,
						   bool *inmem, double unionsets)
{
	/*
	 * A second GROUP BY column (DESIGN.md §20): every (outer, inner)
	 * PAIR of the two indexes' entries is a count of its own, the
	 * two groups' sets and the WHERE's merged, whether or not the
	 * intersection comes out empty.  Two groups whose rows are
	 * scattered over the whole heap have a container at nearly every
	 * container key, which is what makes a pair expensive even when
	 * their intersection is empty.
	 */
	double		oe = Max(c->outer_entries, 1.0);
	double		ie = Max(c->inner_entries, 1.0);
	double		per;

	mem[c->nsrc] = c->tuples / oe;
	keys[c->nsrc] = lion_key_containers(c->root, c->rel, c->groupidx,
										c->groupcol, c->tuples / oe);
	mem[c->nsrc + 1] = c->tuples / ie;
	keys[c->nsrc + 1] = lion_key_containers(c->root, c->rel, c->groupidx2,
											c->groupcol2, c->tuples / ie);
	/*
	 * The early exit of a count(DISTINCT k)'s pair test is NOT
	 * discounted: a pair's time is its lookup and its merge's set-up
	 * far more than its members.  Discounted, 200 x 50 pairs over 100k
	 * rows were chosen at 10,566 for 45 ms against the sorting
	 * aggregate's 10,694 for 27.
	 */
	per = lion_merge_cpu_cost(c->nsrc + 2, mem, keys, inmem, c->tuples,
							  1.0, c->srcprobes) +
		unionsets * LION_UNION_SET_COST;
	c->walked = oe * ie;
	c->merge_cpu = c->walked * per;
}

/*
 * The merges of a grouped count: each group's own set located from the index,
 * and the WHERE's sources probed in memory by the counts.
 */
static void
lion_count_rel_group_merge(LionCountRelCost *c)
{
	double	   *mem = (double *) palloc(sizeof(double) * (c->nsrc + 2));
	double	   *keys = (double *) palloc(sizeof(double) * (c->nsrc + 2));
	bool	   *inmem = (bool *) palloc0(sizeof(bool) * (c->nsrc + 2));
	double		unionsets = 0;
	double		gnd = -1.0;
	double		gm;
	int			s;

	/*
	 * The WHERE's sets are copied into memory on their second use and
	 * probed there by every group after that (DESIGN.md §9); a group's
	 * own set is located from the index for its one count.
	 */
	for (s = 0; s < c->nsrc; s++)
	{
		mem[s] = c->srcmembers[s];
		keys[s] = c->srccontainers[s];
		inmem[s] = true;
	}

	/* the rows of one group's own set, and how many groups are counted */
	if (c->groupdrive)
	{
		gm = Max(c->listrows, 1.0);
		c->walked = Max(c->ingroups, 1.0);
	}
	else
	{
		if (c->groupcol >= 1 && c->groupcol <= c->groupidx->nkeycolumns &&
			c->rel->relid > 0 &&
			c->rel->relid < (Index) c->root->simple_rel_array_size &&
			c->root->simple_rte_array[c->rel->relid] != NULL &&
			c->root->simple_rte_array[c->rel->relid]->rtekind == RTE_RELATION)
			gnd = lion_index_column_nd(c->root, c->rel, c->groupidx,
									   c->groupcol - 1);
		if (c->topk > 0.0)
		{
			/* §36: the candidates alone, the largest entries */
			gm = c->topkrows / c->topk;
			c->walked = c->topk;
		}
		else if (gnd > 0.0)
		{
			gm = c->tuples / gnd;
			c->walked = Max(gnd * c->drivefrac, 1.0);
		}
		else
		{
			gm = c->tuples / Max(c->numgroups, 1.0);
			c->walked = Max(c->ingroups, 1.0);
		}
	}
	mem[c->nsrc] = gm;
	keys[c->nsrc] = lion_key_containers(c->root, c->rel, c->groupidx,
										c->groupcol, gm);

	/*
	 * A WHERE source that is a union - an IN list, an OR - is built again
	 * by every count, each of its sets' cursors set up and positioned
	 * over the copy in memory (LION_UNION_SET_COST a set).
	 */
	for (s = 0; s < c->nsrc; s++)
		if (c->srcunion[s])
			unionsets += c->srcsets[s];

	if (c->groupidx2 == NULL)
		lion_count_rel_one_column(c, mem, keys, inmem, gm, unionsets);
	else
		lion_count_rel_two_columns(c, mem, keys, inmem, unionsets);
	pfree(mem);
	pfree(keys);
	pfree(inmem);
}

/*
 * The CPU of the merges: an ungrouped count's one merge of the AND's sources,
 * or a grouped count's (lion_count_rel_group_merge()).
 */
static void
lion_count_rel_merge(LionCountRelCost *c)
{
	/*
	 * THE MERGE (DESIGN.md §10, "The units"; §22).  An ungrouped count is one
	 * merge of the AND's sources, driven by the smallest and the others sought
	 * at its keys (lion_merge_cpu_cost()) - unless its one source is a union,
	 * whose containers are all read above and are the whole of the work.  A
	 * grouped count runs one merge per GROUP, in which the group's own set is a
	 * source like any other - it drives whenever it is smaller than the WHERE's
	 * sets - and one per (outer, inner) PAIR of two group columns (§20).  Every
	 * entry of the grouping column is counted, whatever the WHERE leaves of it
	 * (lion_next_group() skips the empty ones only after counting them): so the
	 * merges are its n_distinct, not the groups the planner expects out.  The
	 * model before 2026-09-27 charged each group the containers of its share of
	 * the WHERE's rows, which for `c20k, count(*) ... WHERE c20 = 3 GROUP BY
	 * c20k` is 12 of the 219 each entry's own set lies in: 6,550 units for
	 * 1,091 ms, where the sequential aggregate is 142,000 for 905.
	 * A summed range prices its entries' merges itself (lion_cost_range_sum()).
	 */
	if (c->groupidx == NULL && !c->rangesum && c->nsrc > 0 &&
		(c->nsrc > 1 || !c->srcunion[0]))
	{
		/*
		 * The unions among the sources - IN lists, `&&`, ORs across columns -
		 * are met as the leapfrog meets them, read and built where they drive
		 * and sought, then built or probed, where they do not (DESIGN.md
		 * §29.11, "Unions probed"), which lion_merge_cpu_cost_sets() prices in
		 * place of reading every container of their sets and a k-way merge of
		 * them all.
		 */
		c->merge_cpu = lion_merge_cpu_cost_sets(c->nsrc, c->srcmembers,
												c->srccontainers, c->srcnsets,
												c->srcsetcont, c->srcsetpages,
												NULL, c->tuples,
												lion_probe_rel_factor(c->root,
																	  c->rel),
												c->srcprobes);
		c->read_cpu -= c->union_read;
		c->merge_ops -= c->union_ops;
	}
	else if (c->groupidx != NULL && !c->rangesum)
		lion_count_rel_group_merge(c);
}

/*
 * The pages each clause's sets read, walked or sought (lion_cost_set_pages()),
 * and the directory leaves of their lookups; frees the per-clause and
 * per-source arrays.
 */
static void
lion_count_rel_set_pages(LionCountRelCost *c)
{
	int			i;

	/*
	 * Walked: the driver, a group's own list, and what a range's entries are
	 * ANDed with; everything else sought (lion_cost_set_pages()).  A negated
	 * clause has nothing to read.  The single-key lookups' directory leaves,
	 * one a lookup and no more of an index than it has (lion_cost_leaf_pages()).
	 */
	for (i = 0; i < c->nclause; i++)
	{
		bool		walk = (c->clausesrc[i] < 0 ||
							c->clausesrc[i] == c->driver ||
							c->rangesum);

		c->probe_cost += lion_cost_set_pages(c->root, c->rel,
											 &c->clauseset[i], walk,
											 walk ? 0.0 :
											 c->srcprobes[c->clausesrc[i]] *
											 (c->whereonce ? 1.0 : c->walked),
											 &c->seq_pages, &c->probed_pages,
											 &c->seq_cost);
	}
	c->random_pages = lion_cost_leaf_pages(c->nclause, c->clauseindex,
										   c->clauseset);
	pfree(c->clauseset);
	pfree(c->clauseindex);
	pfree(c->clausesrc);
	pfree(c->srcmembers);
	pfree(c->srccontainers);
	pfree(c->srcunion);
	pfree(c->srcsets);
	pfree(c->srcnsets);
	pfree(c->srcsetcont);
	pfree(c->srcsetpages);
	pfree(c->srcprobes);
}

/*
 * The entry scan of the driving index.
 */
static void
lion_count_rel_entry_scan(LionCountRelCost *c)
{
	/*
	 * The entry scan of the driving index - unless an IN list on that very
	 * column drives the groups instead (DESIGN.md §15), in which case its
	 * elements' lookups and containers, charged above, ARE the per-group work
	 * and the index's entries are never walked.  A summed range has priced its
	 * walk already.
	 */
	if (c->groupidx != NULL && !c->groupdrive && !c->rangesum)
	{
		/*
		 * The entry scan walks ONE key column's entries and stops at the first
		 * entry of the next (DESIGN.md §24), so what it reads of a multicolumn
		 * index is that column's share of it and not the whole relation - and
		 * a range bounds the walk to drivefrac of those (§28), the share of
		 * the column's entries it selects.
		 */
		double		entrypages = Max(1.0, (double) c->groupidx->pages *
									 lion_index_column_share(c->root, c->rel,
															 c->groupidx,
															 c->groupcol) *
									 c->drivefrac);

		Cost		entryprice = entrypages *
			lion_index_page_cost(c->root, (double) c->groupidx->pages,
								 seq_page_cost);

		c->seq_pages += entrypages;
		c->seq_cost += entryprice;
		if (c->rc != NULL && c->rc->batched)
			c->rc->perrange += entryprice;

		/*
		 * Beside a second GROUP BY column a count(DISTINCT k) tests each
		 * group of the first for a row as well, which the pairs' tests come
		 * on top of (DESIGN.md §26).
		 */
		if (c->distinct != LION_DISTINCT_NONE && c->groupidx2 != NULL)
			c->recheckshare = lion_exists_fraction(
				lion_containers_for(c->heap_pages,
									c->matching / Max(c->numgroups, 1.0)),
				c->matching / Max(c->numgroups, 1.0));
	}
}

/*
 * A second GROUP BY column's entry scan and its (outer, inner) pairs, and the
 * fixed cost of a count(DISTINCT k)'s tests.
 */
static void
lion_count_rel_pair_cost(LionCountRelCost *c)
{
	/*
	 * A second GROUP BY column (DESIGN.md §20) is a nested loop over the two
	 * indexes' entries: the outer index's entries drive the scan and the
	 * inner index is read ONCE - its keys are kept in memory - but every
	 * (outer, inner) PAIR costs a lookup of the inner posting set and an
	 * attempt at the intersection, whether or not that comes out empty.
	 * Three terms, and the second is the one that decides:
	 *
	 *	- one cpu_tuple_cost per pair, for the lookup and the per-pair
	 *	  bookkeeping;
	 *	- the INTERSECTION itself.  ANDing two containers costs about the
	 *	  members of the smaller of them, so one pair costs about
	 *	  Min(rows/outer_entries, rows/inner_entries) member steps, and summed
	 *	  over all outer_entries x inner_entries pairs that is exactly
	 *	  `Min(outer_entries, inner_entries) x rows` - independent of which of
	 *	  the two drives the scan, which is why the choice of outer is about
	 *	  the entry scans and the memory and not about this;
	 *	- and the container bookkeeping of both sides at every pair, which is
	 *	  what makes a pair of WIDELY SPREAD groups expensive even when their
	 *	  intersection is empty: two groups whose rows are scattered over the
	 *	  whole heap have a container at nearly every container key, so the
	 *	  merge steps through all of them.
	 *
	 * Measured on 200k rows of a 100-byte-wide table, uncorrelated columns
	 * (2026-09-21, assert build): 20 x 2 groups is 5.6 ms against the
	 * sequential aggregate's 37.3 ms and is chosen; 200 x 20 is 60.7 ms
	 * against 36.8 ms and must NOT be, which the member term above is what
	 * says; 20000 x 200 is four million pairs and is refused by a wide
	 * margin.
	 */
	if (c->groupidx2 != NULL)
	{
		double		oe = Max(c->outer_entries, 1.0);
		double		ie = Max(c->inner_entries, 1.0);

		double		co = lion_containers_for(c->heap_pages, c->tuples / oe);
		double		cinner = lion_containers_for(c->heap_pages,
												 c->tuples / ie);

		double		innerpages = Max(1.0, (double) c->groupidx2->pages *
									 lion_index_column_share(c->root, c->rel,
															 c->groupidx2,
															 c->groupcol2));

		c->seq_pages += innerpages;
		c->seq_cost += innerpages *
			lion_index_page_cost(c->root, (double) c->groupidx2->pages,
								 seq_page_cost);
		c->pair_cost = Max(oe * ie, c->numgroups) *
			((c->distinct == LION_DISTINCT_NONE) ? LION_ENTRY_COUNT_COST :
			 cpu_tuple_cost);

		/*
		 * The (g, k) pairs of a count(DISTINCT k) per group (DESIGN.md §26)
		 * are these same pairs and are charged exactly as above - that is
		 * what makes a large |G| x |K| lose to the sorting aggregate core
		 * builds for a distinct count - plus the fixed cost of a test on
		 * each.  The early exit is NOT discounted from the pair terms: a
		 * pair's time is its lookup and its merge's setup far more than its
		 * members, and measured per pair an existence test was 6.5 us
		 * against 7.7 for a count (100k rows, 200 x 50 pairs, assert build).
		 * What it does save is heap rechecks, one container's worth per test
		 * instead of all of them, and that is charged as such below.
		 */
		if (c->distinct != LION_DISTINCT_NONE)
		{
			c->pair_cost += oe * ie * LION_DISTINCT_TEST_COST;
			c->recheckshare += (c->distinct == LION_DISTINCT_EXISTS) ?
				lion_exists_fraction(Min(co, cinner),
									 c->matching / (oe * ie)) :
				1.0;
		}
	}
	/*
	 * ... and the fixed cost of the tests of a count(DISTINCT k) that are not
	 * pairs (DESIGN.md §26): one per entry of k the walk visits - or per
	 * listed value, when a list on k drives it - and beside a GROUP BY one
	 * per group, the group's own test.
	 *
	 * Without a GROUP BY the walk tests EVERY entry of k it visits
	 * (c->walked), and not only the ones the WHERE leaves a row in, which
	 * are the values it emits (c->ingroups): an entry the WHERE empties is a
	 * test read to its end to say so (lion_exists_fraction()).  Charging the
	 * emitted values priced `count(DISTINCT UserID) WHERE MobilePhoneModel
	 * <> ''` (ClickBench, 5M rows) at 281,060 tests where it made 3,413,988,
	 * and chose it at 275k cost units against a bitmap scan's 437k: 9.1 s
	 * against 0.7.
	 */
	if (c->distinct != LION_DISTINCT_NONE)
		c->pair_cost += ((c->groupidx2 == NULL) ? Max(c->walked, c->ingroups) :
						 c->ingroups) *
			LION_DISTINCT_TEST_COST;
}

/*
 * The heap recheck: the candidate TIDs the visibility map cannot vouch for,
 * and the pages they lie on.
 */
static void
lion_count_rel_recheck(LionCountRelCost *c)
{
	double		dirty_pages;

	/*
	 * Rechecking is what makes the pushdown expensive, and the estimate has
	 * to say so: every TID whose heap block the visibility map cannot vouch
	 * for is resolved against the snapshot.
	 *
	 * The blocks that can be touched are only the ones the visibility map
	 * cannot vouch for - heap_pages * dirtyfrac, from the same
	 * relallvisible/relpages the TID estimate comes from - and each of them
	 * is FETCHED AT MOST ONCE per query, whatever brings the count back to
	 * it: the per-query visibility cache resolves every root line pointer of
	 * a dirty page on its first visit and answers every later visit out of
	 * memory (DESIGN.md §9).  Charging random reads across the whole heap
	 * instead made the model refuse the pushdown on freshly vacuumed tables,
	 * where it is at its best (the 2026-09-20 review, finding 5).
	 *
	 * A GROUP BY therefore pays for the same working set as a single count,
	 * once, and what it repeats per group is CPU: one visibility-bit lookup
	 * per candidate TID - and the candidates of all the groups together are
	 * the same matching * dirtyfrac - plus the per-group container
	 * bookkeeping already in ncontainers above.  Charging
	 * numgroups * dirty_pages RANDOM reads instead asked 1.8M cost units for
	 * a 200-group count of five million rows with 9% of the heap pages
	 * dirty, against the sequential aggregate's 175k, for a node that ran in
	 * 148 ms against 1174 ms over a resident 71 MiB working set with zero
	 * physical reads (the 2026-09-21 follow-up review).
	 */
	dirty_pages = Min(c->heap_pages * c->dirtyfrac, c->heap_pages);
	c->recheck_tids = c->matching * c->dirtyfrac * c->recheckshare;
	c->recheck_pages = Min(c->recheck_tids, dirty_pages);

	/* A summed range has priced its own (lion_cost_range_sum()). */
	if (c->rangesum)
		c->recheck_tids = c->recheck_pages = 0.0;

	/*
	 * ... except that a RANGE-bounded walk (DESIGN.md §28) does not visit a
	 * dirty page once per query: it counts entry by entry, each count flushes
	 * its own recheck batch, and the visibility cache only answers a page from
	 * its second visit on - so a page that holds rows of several entries is
	 * fetched for each of them.  How many pages one entry's candidates lie on
	 * depends on how the column follows the heap order, which is what
	 * cost_index() interpolates with the correlation's square: at most one per
	 * row when the values are scattered, and the rows' share of the heap when
	 * they are stored in order.  Measured at five million rows with every heap
	 * page dirty: a 30-day range over randomly placed days rechecked 68,384
	 * block visits for 37,133 pages (166 ms, against the bitmap heap scan's
	 * 46), and the same range over days stored in order 581 (3.8 ms, against
	 * the btree's 11.5).  Priced as one visit per dirty page the first was
	 * chosen and the second refused.  The cache does bound it: a page is
	 * resolved on its second visit and answered from memory after that, so
	 * while the cache has room no page is fetched more than twice - 2,454
	 * block visits for 1,148 pages when 21 entries of a 200-value column share
	 * every page of a small table.
	 */
	if (c->rangevar != NULL && c->recheck_tids > 0.0)
	{
		double		entries = lion_range_entries(c->root, c->rel, c->rangevar,
												 c->drivefrac);
		double		rowsper = c->matching / entries;
		double		corr = lion_var_correlation(c->root, c->rel, c->rangevar);
		double		scattered = Min(rowsper, c->heap_pages);
		double		inorder = Max(1.0, rowsper * c->heap_pages / c->tuples);
		double		perentry = scattered + (inorder - scattered) * corr * corr;

		c->recheck_pages = Min(Min(c->recheck_tids,
								   entries * perentry * c->dirtyfrac),
							   2.0 * c->recheck_pages);
	}

	/*
	 * ... and the rows of ONE value (a plain equality, the commonest count of
	 * all) lie on as many heap pages as the column's order says, which is
	 * what cost_index() prices a plain index scan's heap side with (§29.11):
	 * one per row when the values are scattered, the rows' share of the heap
	 * when they are stored in order, interpolated by the correlation's
	 * square.  The recheck visits each of those pages once, in block order,
	 * so charging one page per candidate TID made a count over a clustered
	 * value whose visibility map had gone stale (relallvisible is only
	 * refreshed by VACUUM, while updates and on-access pruning change the
	 * map) cost more than a plain index scan that fetches every row: 5036
	 * against 3252 units for 5000 rows on 32 pages at a million rows, and the
	 * node ran 2.5x faster than the scan that was chosen.
	 */
	if (c->rangevar == NULL && c->ors == NIL && c->recheck_tids > 0.0)
	{
		Var		   *eqvar = lion_single_eq_var(c->rel, c->whereclauses,
											   c->wherekinds);

		if (eqvar != NULL)
		{
			double		corr = lion_var_correlation(c->root, c->rel, eqvar);
			double		scattered = Min(c->matching, c->heap_pages);
			double		inorder = Max(1.0, c->matching * c->heap_pages /
									  c->tuples);
			double		spanned = scattered + (inorder - scattered) * corr * corr;

			c->recheck_pages = Min(c->recheck_pages,
								   Max(1.0, spanned * c->dirtyfrac));
		}
	}
}

/*
 * What a LionCount path costs over one relation, phase by phase: the WHERE
 * clauses' lookups and the sources of their AND, the merges that count them,
 * the pages they read, the entry scans and pairs of the GROUP BY, and the
 * heap recheck.
 */
static Cost
lion_cost_count_rel(PlannerInfo *root, RelOptInfo *rel,
				   IndexOptInfo *groupidx, AttrNumber groupcol,
				   IndexOptInfo *groupidx2, AttrNumber groupcol2,
				   List *whereidx, List *wherecol, List *whereclauses,
				   List *wherekinds,
				   List *ors, double numgroups,
				   double outer_entries, double inner_entries, int distinct,
				   double drivefrac, Var *rangevar, bool rangesum,
				   LionRangeCost *rc, double topk, double topkrows)
{
	LionCountRelCost c;
	bool		sumshort;		/* §15: the IN list is summed, not merged */
	Cost		run;

	memset(&c, 0, sizeof(c));
	c.root = root;
	c.rel = rel;
	c.groupidx = groupidx;
	c.groupcol = groupcol;
	c.groupidx2 = groupidx2;
	c.groupcol2 = groupcol2;
	c.whereidx = whereidx;
	c.wherecol = wherecol;
	c.whereclauses = whereclauses;
	c.wherekinds = wherekinds;
	c.ors = ors;
	c.numgroups = numgroups;
	c.outer_entries = outer_entries;
	c.inner_entries = inner_entries;
	c.distinct = distinct;
	c.drivefrac = drivefrac;
	c.rangevar = rangevar;
	c.rangesum = rangesum;
	c.rc = rc;
	c.heap_pages = Max((double) rel->pages, 1.0);
	c.dirtyfrac = 1.0 - rel->allvisfrac;
	c.matching = Max(lion_probe_rel_rows(root, rel), 1.0);
	c.tuples = Max(rel->tuples, 1.0);
	c.driver = -1;
	c.nclause = list_length(whereclauses);
	c.ingroups = numgroups;
	c.recheckshare = 1.0;
	c.walked = 1.0;

	/*
	 * The top k (DESIGN.md §36) counts its candidates' sets alone, and
	 * rechecks their rows alone.
	 */
	c.topk = topk;
	c.topkrows = Min(Max(topkrows, topk), c.tuples);
	if (topk > 0.0)
		c.recheckshare = c.topkrows / c.tuples;

	/*
	 * A sum over a range (DESIGN.md §28) prices its walk, its entries and its
	 * heap recheck itself (lion_cost_range_sum()); what is left here is the
	 * WHERE sources, located once and read once - materialized after their
	 * first use - and the one row.
	 */
	if (rangesum)
	{
		c.rangecost = lion_cost_range_sum(root, rel, groupidx, groupcol,
										  rangevar, drivefrac, whereclauses,
										  wherekinds, ors, NULL);
		c.ingroups = 1.0;
	}

	/*
	 * A count(DISTINCT k) without a GROUP BY walks k's entries, and a WHERE
	 * equality on k itself drives that walk as a list of one would (DESIGN.md
	 * §26).
	 */
	c.inlistci = lion_inlist_shape(groupidx, groupcol, groupidx2,
								   whereidx, wherecol, whereclauses,
								   wherekinds, ors,
								   distinct != LION_DISTINCT_NONE &&
								   groupidx2 == NULL,
								   &sumshort, &c.groupdrive);

	lion_count_rel_alloc(&c);
	lion_count_rel_clauses(&c);
	lion_count_rel_or_unions(&c);
	lion_count_rel_driver(&c);
	lion_count_rel_merge(&c);
	lion_count_rel_set_pages(&c);
	lion_count_rel_entry_scan(&c);
	lion_count_rel_pair_cost(&c);
	lion_count_rel_recheck(&c);

	run = c.random_pages * random_page_cost;
	run += c.descent_cost;
	run += c.lookup_cost;
	run += c.seq_cost;			/* c.seq_pages, each at its index's rung */
	run += c.probe_cost;
	run += c.read_cpu + c.merge_cpu;	/* containers, members, probes (§10) */
	run += c.merge_ops * cpu_operator_cost;
	run += c.recheck_pages * lion_heap_page_cost(root, rel, c.recheck_pages,
												 c.heap_pages);
	run += c.recheck_tids * ((groupidx != NULL && !rangesum) ?
							 LION_RECHECK_GROUP_TID_COST :
							 LION_RECHECK_TID_COST);
	run += c.ingroups * cpu_tuple_cost;
	run += c.pair_cost;
	run += c.rangecost;
	run += c.rangesrc_cost;

	return run;
}

/*
 * The clause lists as one relation counts them: without the clauses its
 * partition bounds imply (DESIGN.md §16, "Clauses the partition bounds
 * imply"), which it leaves out - a whole OR restriction at a time, or the arms
 * of one its bounds refute and the leaves of an arm they imply, which `ors`
 * then says without them, renumbered - and *joinclause, the FK-side join's
 * key, where it lands.  A relation that leaves nothing out counts the lists as
 * they are, and gets them back unchanged.
 */
void
lion_target_lists(const LionCountTarget *t, List *whereclauses,
				  List *wherekinds, List *ors, List **idx, List **col,
				  List **clauses, List **kinds, List **tors, int *joinclause)
{
	int			nclause = list_length(whereclauses);
	int		   *pos;
	int			n = 0;
	int			i;
	ListCell   *lc;

	*idx = t->whereidx;
	*col = t->wherecol;
	*clauses = whereclauses;
	*kinds = wherekinds;
	*tors = ors;
	if (t->ndropped == 0)
		return;

	*idx = NIL;
	*col = NIL;
	*clauses = NIL;
	*kinds = NIL;
	*tors = NIL;
	pos = (int *) palloc(sizeof(int) * Max(nclause, 1));
	for (i = 0; i < nclause; i++)
	{
		pos[i] = n;
		if (t->dropped[i])
			continue;
		*idx = lappend(*idx, list_nth(t->whereidx, i));
		*col = lappend_int(*col, list_nth_int(t->wherecol, i));
		*clauses = lappend(*clauses, list_nth(whereclauses, i));
		*kinds = lappend_int(*kinds, list_nth_int(wherekinds, i));
		n++;
	}
	foreach(lc, ors)
	{
		List	   *one = (List *) lfirst(lc);
		int			first = linitial_int(one);
		int			narms = lsecond_int(one);
		List	   *armlens = NIL;
		int			at = first;
		int			a;

		for (a = 0; a < narms; a++)
		{
			int			len = list_nth_int(one, 2 + a);
			int			kept = 0;
			int			j;

			for (j = at; j < at + len; j++)
			{
				if (!t->dropped[j])
					kept++;
			}
			if (kept > 0)
				armlens = lappend_int(armlens, kept);
			at += len;
		}
		if (armlens == NIL)
			continue;			/* left out whole */
		*tors = lappend(*tors,
						list_concat(list_make2_int(pos[first],
												   list_length(armlens)),
									armlens));
	}
	if (joinclause != NULL && *joinclause >= 0)
		*joinclause = pos[*joinclause];
	pfree(pos);
}

/*
 * Sum the per-relation costs over every relation the node will count and put
 * the result on the path.  The WHERE clauses that do not select rows
 * (`IS NOT NULL`, DESIGN.md §14) are left out of the per-relation estimate,
 * as they were before partitions existed - by lion_cost_count_rel() itself
 * rather than by filtering the lists here, because the OR structure of
 * DESIGN.md §19 names its leaves by their position in them.  A partition
 * prices only the clauses its bounds do not imply (lion_target_lists()).
 */
void
lion_cost_count_path(PlannerInfo *root, CustomPath *cpath, List *targets,
					List *whereclauses, List *wherekinds, List *ors,
					double numgroups, double outer_entries,
					double inner_entries, double outrows, int distinct,
					int ranged, double drivefrac, LionRangeCost *rc)
{
	Cost		run = 0;
	ListCell   *lc;

	if (rc != NULL)
	{
		rc->batched = false;
		rc->perrange = 0;
	}

	foreach(lc, targets)
	{
		LionCountTarget *t = (LionCountTarget *) lfirst(lc);
		int			tranged = ranged;
		double		tfrac = drivefrac;
		Var		   *rangevar;
		List	   *tidx;
		List	   *tcol;
		List	   *tclauses;
		List	   *tkinds;
		List	   *tors;

		lion_target_lists(t, whereclauses, wherekinds, ors, &tidx, &tcol,
						  &tclauses, &tkinds, &tors, NULL);

		/*
		 * The sum over every entry of a summarized column (DESIGN.md §32) is
		 * the sum over its summaries: a range over all of it, and priced as
		 * one.  Without summaries - in this relation: partitions differ - it
		 * is the walk over every entry it always was.
		 */
		if (tranged == LION_RANGED_SUMALL)
		{
			if (t->driveidx[0] != NULL &&
				lion_index_col_summarized(t->driveidx[0], t->drivecol[0]))
			{
				tranged = LION_RANGED_SUM;
				tfrac = 1.0;
			}
			else
				tranged = LION_RANGED_NONE;
		}
		rangevar = (tranged != LION_RANGED_NONE) ? t->drivevar[0] : NULL;

		run += lion_cost_count_rel(root, t->rel,
								  t->driveidx[0], t->drivecol[0],
								  t->driveidx[1], t->drivecol[1],
								  tidx, tcol, tclauses, tkinds, tors, numgroups,
								  outer_entries, inner_entries, distinct,
								  tfrac, rangevar,
								  tranged == LION_RANGED_SUM && rangevar != NULL &&
								  t->driveidx[0] != NULL,
								  list_length(targets) == 1 ? rc : NULL,
								  0.0, 0.0);

		/*
		 * A multi-key query the node only has at run time may need its
		 * candidates rechecked in the heap, one count - one group, one test -
		 * at a time (DESIGN.md §17, "A query known only at run time").
		 */
		run += lion_cost_recheck(root, t->rel, tidx, tcol, tclauses, tkinds,
								 tors, lion_probe_rel_rows(root, t->rel),
								 numgroups, t->rel->tuples);

		/*
		 * A range-bounded GROUP BY walk (DESIGN.md §28) pays a fixed cost per
		 * entry it visits, in each relation it walks - a partition's entries
		 * are its own.  A count(DISTINCT) walk already pays its per-test cost
		 * for each of them (§26), which is the same work, and a summed range
		 * has priced its entries in lion_cost_range_sum().
		 */
		if (tranged == LION_RANGED_WALK && distinct == LION_DISTINCT_NONE &&
			rangevar != NULL)
			run += lion_range_entries(root, t->rel, rangevar,
									  tfrac) * LION_RANGE_ENTRY_COST;
	}

	cpath->path.rows = outrows;
#if PG_VERSION_NUM >= 180000
	cpath->path.disabled_nodes = 0;
#endif

	/*
	 * Every form of the node streams its rows as it counts them - one per
	 * group, and with partitions one per group per partition (DESIGN.md §16:
	 * the partials go to a Finalize Agg above, which is costed by core) - so
	 * only the single-row forms have to do the whole scan before the first
	 * row comes out.
	 */
	cpath->path.startup_cost = (outrows <= 1.0) ? run : 0.0;
	cpath->path.total_cost = run;
}

/*
 * THE TOP k BY COUNT (DESIGN.md §36): one table's GROUP BY of one column,
 * `entries` entries of which the walk reads the headers of - the share
 * drivefrac of them a range on the column leaves - and `cand` candidates,
 * whose entries hold candrows rows, counted against the WHERE as the groups
 * of any walk are, each found again by a descent of the directory.  Nothing
 * comes out before the last candidate is counted.
 */
#define LION_TOPK_ENTRY_COST	(2.0 * cpu_operator_cost)

void
lion_cost_topk_path(PlannerInfo *root, CustomPath *cpath, List *targets,
					List *whereclauses, List *wherekinds, List *ors,
					double entries, double drivefrac, double cand,
					double candrows, double outrows)
{
	LionCountTarget *t = (LionCountTarget *) linitial(targets);
	List	   *tidx;
	List	   *tcol;
	List	   *tclauses;
	List	   *tkinds;
	List	   *tors;
	Cost		run;

	lion_target_lists(t, whereclauses, wherekinds, ors, &tidx, &tcol,
					  &tclauses, &tkinds, &tors, NULL);
	run = lion_cost_count_rel(root, t->rel, t->driveidx[0], t->drivecol[0],
							  NULL, 0, tidx, tcol, tclauses, tkinds, tors,
							  cand, cand, 0, LION_DISTINCT_NONE, drivefrac,
							  NULL, false, NULL, cand, candrows);
	run += lion_cost_recheck(root, t->rel, tidx, tcol, tclauses, tkinds, tors,
							 lion_probe_rel_rows(root, t->rel), cand,
							 t->rel->tuples);
	run += entries * drivefrac * LION_TOPK_ENTRY_COST;
	run += cand * (LION_PROBE_COST + LION_RANGE_DESCENT_PAGES *
				   lion_index_page_cost(root,
										(t->driveidx[0] != NULL) ?
										(double) t->driveidx[0]->pages : 0.0,
										seq_page_cost));

	cpath->path.rows = outrows;
#if PG_VERSION_NUM >= 180000
	cpath->path.disabled_nodes = 0;
#endif
	cpath->path.startup_cost = run;
	cpath->path.total_cost = run;
}

/*
 * THE AGGREGATES OVER LION COLUMNS' ENTRIES (DESIGN.md §37): one walk of each
 * column's entries, idxs[c] and key column cols[c], with naggs[c] arguments
 * evaluated at each entry.  On a heap the visibility map calls all-visible
 * the walk reads the entries' headers alone; elsewhere it counts each entry
 * as a GROUP BY of the column would.  Added to the sum over every row the
 * path already prices when the target list has counts too (counts), and in
 * place of it when it has not.
 */
#define LION_WAGG_ALLVISIBLE	0.999

void
lion_cost_wagg_path(PlannerInfo *root, CustomPath *cpath, RelOptInfo *rel,
					List *idxs, List *cols, List *naggs, bool counts)
{
	Cost		run = counts ? cpath->path.total_cost : 0.0;
	bool		fast = (rel->allvisfrac >= LION_WAGG_ALLVISIBLE);
	ListCell   *l1;
	ListCell   *l2;
	ListCell   *l3;

	forthree(l1, idxs, l2, cols, l3, naggs)
	{
		IndexOptInfo *idx = (IndexOptInfo *) lfirst(l1);
		AttrNumber	col = (AttrNumber) lfirst_int(l2);
		double		nagg = (double) lfirst_int(l3);
		double		nd = lion_index_column_nd(root, rel, idx, col - 1);
		double		pages;

		if (nd <= 0.0)
			nd = Max(rel->tuples, 1.0);
		pages = Max(1.0, (double) idx->pages *
					lion_index_column_share(root, rel, idx, col));
		run += nd * nagg * 2.0 * cpu_operator_cost;
		if (fast)
			run += pages * lion_index_page_cost(root, (double) idx->pages,
												seq_page_cost) +
				nd * LION_TOPK_ENTRY_COST;
		else
			run += lion_cost_count_rel(root, rel, idx, col, NULL, 0,
									   NIL, NIL, NIL, NIL, NIL, nd, nd, 0,
									   LION_DISTINCT_NONE, 1.0, NULL, false,
									   NULL, 0.0, 0.0);
	}

	cpath->path.rows = 1.0;
#if PG_VERSION_NUM >= 180000
	cpath->path.disabled_nodes = 0;
#endif
	cpath->path.startup_cost = run;
	cpath->path.total_cost = run;
}

/*
 * THE DECODED WALK (DESIGN.md §34): a GROUP BY of ncol columns, groupest[c]
 * values each by the planner's estimate, over one table.  It does what the
 * grouped count of its first column alone does - that column's sets walked
 * key by key against the collected WHERE, the map asked once a key and the
 * dirty rows rechecked (lion_cost_count_rel()) - and at every key the WHERE
 * leaves rows at, besides:
 *
 *	- every column's containers there decoded: a step to the key for each
 *	  value of it that has a container there - a seek, past the keys between,
 *	  where the WHERE leaves few - and a store per member, all the key's rows
 *	  once a column;
 *	- each of the WHERE's rows counted under its combination
 *	  (LION_DECODE_ROW_COST, or LION_DECODE_HASH_ROW_COST once an array of
 *	  every combination would take more than a quarter of hash_mem);
 *	- the posting pages of the other columns' sets, the share of them the
 *	  keys are.
 *
 * Nothing of it grows with the number of combinations, which is the point
 * (the pairs of §20 are priced by the product of the two entry counts).  A
 * column with more values than a pass takes makes more than one pass, each
 * reading the other columns again: priced as that many times their work.
 * The rows are partial counts, one per combination with rows, which the
 * Finalize Agg above adds up; all of them come out once the pass has run.
 */
void
lion_cost_decode_path(PlannerInfo *root, CustomPath *cpath, List *targets,
					  List *whereclauses, List *wherekinds, List *ors,
					  int ncol, const double *groupest, double numgroups,
					  double outrows)
{
	LionCountTarget *t = (LionCountTarget *) linitial(targets);
	RelOptInfo *rel = t->rel;
	double		heap_pages = Max((double) rel->pages, 1.0);
	double		tuples = Max(rel->tuples, 1.0);
	double		nkeys = Max(ceil(heap_pages / LION_BLOCKS_PER_CONTAINER), 1.0);
	double		rowsperkey = tuples / nkeys;
	double		whererows = Max(rel->rows, 1.0);
	double		keys;
	double		keyfrac;
	double		combos = 1.0;
	double		passes = 1.0;
	double		percap;
	Cost		perkey = 0;
	Cost		run;
	List	   *tidx;
	List	   *tcol;
	List	   *tclauses;
	List	   *tkinds;
	List	   *tors;
	int			c;

	Assert(list_length(targets) == 1 && ncol >= 2);
	lion_target_lists(t, whereclauses, wherekinds, ors, &tidx, &tcol,
					  &tclauses, &tkinds, &tors, NULL);

	/* column 0 against the WHERE, as its own grouped count walks it */
	run = lion_cost_count_rel(root, rel, t->driveidx[0], t->drivecol[0],
							  NULL, 0, tidx, tcol, tclauses, tkinds, tors,
							  groupest[0], groupest[0], 0,
							  LION_DISTINCT_NONE, 1.0, NULL, false, NULL,
							  0.0, 0.0);
	run += lion_cost_recheck(root, rel, tidx, tcol, tclauses, tkinds, tors,
							 lion_probe_rel_rows(root, rel), groupest[0],
							 rel->tuples);

	/* the keys the WHERE's rows lie at, scattered over the heap */
	keys = Max(nkeys * (1.0 - exp(-whererows / nkeys)), 1.0);
	keyfrac = Min(keys / nkeys, 1.0);

	/*
	 * Every column decoded at each of them.  Values a pass takes: column 0's
	 * a batch of groups (LION_GROUP_BATCH_MAX at most, fewer under a tight
	 * pin budget), the others' a work_mem of cursors shared among them.
	 */
	percap = Max((double) work_mem * 1024.0 /
				 ((double) sizeof(PGAlignedBlock) + 512.0) / (ncol - 1), 16.0);
	for (c = 0; c < ncol; c++)
	{
		double		vals = Min(Max(groupest[c], 1.0), rowsperkey);
		double		cap = (c == 0) ? (double) LION_GROUP_BATCH_MAX : percap;

		perkey += rowsperkey * LION_DECODE_MEMBER_COST;
		perkey += vals * LION_DECODE_CONTAINER_COST;
		if (c > 0)
		{
			IndexOptInfo *idx = t->driveidx[c];

			perkey += vals * LION_PROBE_COST * (1.0 - keyfrac);
			if (idx != NULL)
				run += (double) idx->pages / Max(idx->nkeycolumns, 1) *
					keyfrac * lion_index_page_cost(root, (double) idx->pages,
												   seq_page_cost);
		}
		combos *= Max(groupest[c], 1.0);
		passes *= Max(ceil(Max(groupest[c], 1.0) / cap), 1.0);
	}
	run += keys * perkey * passes;
	if (combos * sizeof(int64) * 4 <= (double) get_hash_memory_limit())
		run += whererows * (LION_DECODE_ROW_COST +
							(ncol - 1) * LION_DECODE_ROW_COL_COST +
							((combos * sizeof(int64) > LION_DECODE_CACHE_BYTES) ?
							 LION_DECODE_ROW_MISS_COST : 0.0));
	else
		run += whererows * (LION_DECODE_HASH_ROW_COST +
							(ncol - 1) * LION_DECODE_ROW_COL_COST);

	cpath->path.rows = outrows;
#if PG_VERSION_NUM >= 180000
	cpath->path.disabled_nodes = 0;
#endif
	/* a pass's rows go up once it has counted them all */
	cpath->path.startup_cost = run;
	cpath->path.total_cost = run + outrows * cpu_tuple_cost;
	(void) numgroups;
}

/*
 * How a multi-key clause will be answered (DESIGN.md §17), and how many
 * posting sets its query is made of: the keys its extraction yields, which is
 * what a count merges at every container key it asks the clause about.
 *
 * A literal is extracted again the way the executor extracts it, exactly -
 * only an exact extraction of a literal is pushed down
 * (lion_multikey_query_is_exact()).  A value the executor only has at run
 * time comes here as its plan-time estimate when there is one (the clause
 * lion_analyze_leaf() gave the cost model carries it), extracted as the
 * executor will extract the run-time value: exactly, as a superset to be
 * rechecked, or as every row.  With no estimate at all - a generic plan's
 * parameter - the query's shape is unknown, and it is taken to be the
 * expensive one, every row, as lioncostestimate() takes it for a bitmap scan.
 * Anything unexpected answers one set, the old price.
 */
static LionQueryMode
lion_multikey_cost_mode(IndexOptInfo *idx, AttrNumber col, Node *clause,
						double *nkeys)
{
	return lion_multikey_cost_mode_ex(idx, col, clause, nkeys, NULL);
}

/*
 * ... and in *isunion whether the keys are ORed and nothing else - the
 * union of their posting sets, a `&&`'s, which an AND meets as it meets an
 * IN list (lion_merge_cpu_cost_sets()) - where a `@>` or a tsquery ANDs them.
 */
static LionQueryMode
lion_multikey_cost_mode_ex(IndexOptInfo *idx, AttrNumber col, Node *clause,
						   double *nkeys, bool *isunion)
{
	OpExpr	   *op;
	Node	   *arg;
	Const	   *con;
	Oid			opfamily;
	Oid			lefttype;
	Oid			proc;
	int			strategy;
	FmgrInfo	flinfo;
	LionQuery	q;
	LionState	state;
	MemoryContext cxt;
	MemoryContext oldcxt;

	*nkeys = 1.0;
	if (isunion != NULL)
		*isunion = false;
	if (clause == NULL || !IsA(clause, OpExpr) || col < 1 ||
		col > idx->nkeycolumns || list_length(((OpExpr *) clause)->args) != 2)
		return LION_QMODE_KEYS;
	op = (OpExpr *) clause;

	/* the column is the left operand of a multi-key clause, the query the right */
	arg = lion_strip((Node *) lsecond(op->args));
	if (arg == NULL || !IsA(arg, Const))
		return LION_QMODE_ALL;
	con = (Const *) arg;
	if (con->constisnull)
		return LION_QMODE_NONE;

	opfamily = idx->opfamily[col - 1];
	lefttype = idx->opcintype[col - 1];
	strategy = get_op_opfamily_strategy(op->opno, opfamily);
	proc = get_opfamily_proc(opfamily, lefttype, lefttype,
							 LION_EXTRACTQUERY_PROC);
	if (strategy == 0 || !OidIsValid(proc))
		return LION_QMODE_KEYS;

	cxt = AllocSetContextCreate(CurrentMemoryContext,
								"roaring count query keys",
								ALLOCSET_SMALL_SIZES);
	oldcxt = MemoryContextSwitchTo(cxt);

	memset(&state, 0, sizeof(state));
	state.multikey = true;
	state.collation = con->constcollid;
	fmgr_info(proc, &flinfo);
	state.extractquery = flinfo;

	/* a query the exact extraction answers comes out the same either way */
	lion_extract_query_superset(&state, NULL,
								strategy == LION_STRAT_MATCH ?
								lion_tsquery_strip_prefixes(con->constvalue) :
								con->constvalue,
								(StrategyNumber) strategy, &q);
	if (q.mode == LION_QMODE_KEYS || q.mode == LION_QMODE_LOSSY)
		*nkeys = Max((double) q.nkeys, 1.0);
	if (isunion != NULL && q.mode == LION_QMODE_KEYS && q.tree != NULL &&
		q.tree->kind == LION_KN_OR)
	{
		int			i;

		*isunion = true;
		for (i = 0; i < q.tree->nargs; i++)
			if (q.tree->args[i]->kind != LION_KN_KEY)
				*isunion = false;
	}

	MemoryContextSwitchTo(oldcxt);
	MemoryContextDelete(cxt);

	return q.mode;
}

/*
 * Is a count's WHERE priced at its dearest because a multi-key query in it is
 * not known until run time - a generic plan's parameter, with no estimate to
 * go by, which lion_multikey_cost_mode() takes as every row?  Such a price is
 * the most the count can cost, the node's own sequential scan, and not an
 * estimate that may fall short of it: the count is offered without
 * pg_lion.pushdown_margin (DESIGN.md §39), which is there for the estimates.
 * An OR's leaf is a literal (lion_analyze_leaf()) and is never one.
 */
bool
lion_where_query_unknown(List *whereclauses, List *wherekinds,
						 List *whereinor)
{
	ListCell   *lc1;
	ListCell   *lc2;
	ListCell   *lc3;

	forthree(lc1, whereclauses, lc2, wherekinds, lc3, whereinor)
	{
		Node	   *clause = (Node *) lfirst(lc1);
		Node	   *arg;

		if (lfirst_int(lc2) != LION_CLAUSE_MULTI || lfirst_int(lc3) != 0)
			continue;
		if (clause == NULL || !IsA(clause, OpExpr) ||
			list_length(((OpExpr *) clause)->args) != 2)
			continue;
		arg = lion_strip((Node *) lsecond(((OpExpr *) clause)->args));
		if (arg != NULL && !IsA(arg, Const))
			return true;
	}
	return false;
}

/*
 * Will the superset of a LOSSY multi-key clause be decided from stored
 * positions rather than in the heap (lion_posfilter.c)?  When the index
 * column stores them (lion_index_stores_positions()) and the query names no
 * key a position cursor cannot follow - lion_tsquery_item_keys(), as the
 * executor asks it.  Only a literal is known now.
 */
static bool
lion_multikey_posexact(IndexOptInfo *idx, AttrNumber col, Node *clause)
{
	OpExpr	   *op;
	Node	   *arg;

	if (clause == NULL || !IsA(clause, OpExpr) || col < 1 ||
		col > idx->nkeycolumns || list_length(((OpExpr *) clause)->args) != 2)
		return false;
	op = (OpExpr *) clause;
	arg = lion_strip((Node *) lsecond(op->args));
	if (arg == NULL || !IsA(arg, Const) || ((Const *) arg)->constisnull)
		return false;

	return lion_query_posexact(idx->opfamily[col - 1], idx->opcintype[col - 1],
							   op->opno, ((Const *) arg)->constvalue,
							   ((Const *) arg)->constcollid) &&
		lion_index_stores_positions(idx, col);
}

double
lion_multikey_nkeys(IndexOptInfo *idx, AttrNumber col, Node *clause)
{
	double		nkeys;

	(void) lion_multikey_cost_mode(idx, col, clause, &nkeys);
	return nkeys;
}

/*
 * The heap recheck of the multi-key clauses whose query the node only has at
 * run time (DESIGN.md §17, "A query known only at run time"), over `matched`
 * rows of `rel` - the rows every clause selects - counted `counts` times over
 * (once per group, per test of a count(DISTINCT) walk, or per dimension row
 * of the FK-side join), and never more than `ceiling` candidates.
 *
 * How each clause will be answered is asked of its plan-time estimate
 * (lion_multikey_cost_mode()): exactly, which needs no recheck and costs
 * nothing here; as a superset, whose candidates are taken to be the rows the
 * clause selects - a floor, since the estimate says the superset is wider but
 * not by how much; or as every row, whose candidates are then every row the
 * OTHER clauses select - with no other clause, every row of the relation,
 * read by a sequential scan (lion_count_heap_filtered()).  A value with no
 * estimate at all is that last case.
 *
 * Each candidate is fetched, whatever the visibility map says - the map
 * vouches for visibility and not for the filter - on the pages each count
 * reads for itself (a filtered recheck keeps no visibility cache), and tested
 * at the clauses' own evaluation cost.  §10's recheck of the dirty pages is
 * still charged beside it by the caller; it is the smaller of the two.
 *
 * A superset decided from stored positions (lion_posfilter.c) reads no heap:
 * each candidate costs an operator call per key of the query, the cursors'
 * decoding, and nothing else is charged for it here.
 */
Cost
lion_cost_recheck(PlannerInfo *root, RelOptInfo *rel, List *whereidx,
				  List *wherecol, List *whereclauses, List *wherekinds,
				  List *ors, double matched, double counts, double ceiling)
{
	double		heap_pages = Max((double) rel->pages, 1.0);
	double		cand = Max(matched, 1.0);
	int		   *orgrp = lion_or_group_map(ors, list_length(whereclauses));
	Cost		perrow = cpu_tuple_cost;
	Cost		poscost = 0.0;	/* per candidate, the position filters' */
	bool		any = false;
	double		pages;
	int			ci = 0;
	ListCell   *lc1;
	ListCell   *lc2;
	ListCell   *lc3;
	ListCell   *lc4;

	forfour(lc1, whereidx, lc2, whereclauses, lc3, wherekinds, lc4, wherecol)
	{
		Node	   *clause = (Node *) lfirst(lc2);
		bool		inor = (orgrp[ci++] >= 0);
		LionQueryMode mode;
		double		nkeys;
		QualCost	qual_cost;

		if (lfirst_int(lc3) != LION_CLAUSE_MULTI)
			continue;
		mode = lion_multikey_cost_mode((IndexOptInfo *) lfirst(lc1),
									   (AttrNumber) lfirst_int(lc4), clause,
									   &nkeys);
		if (mode != LION_QMODE_LOSSY && mode != LION_QMODE_ALL)
			continue;

		/*
		 * An OR leaf's query is always a literal, and one the sets only bound
		 * is always decided from positions (lion_analyze_leaf()).
		 */
		if (inor || (mode == LION_QMODE_LOSSY &&
			lion_multikey_posexact((IndexOptInfo *) lfirst(lc1),
								   (AttrNumber) lfirst_int(lc4), clause)))
		{
			if (mode == LION_QMODE_LOSSY)
				poscost += nkeys * cpu_operator_cost;
			continue;
		}

		any = true;
		cost_qual_eval_node(&qual_cost, clause, root);
		perrow += qual_cost.per_tuple;
		if (mode == LION_QMODE_ALL)
			cand /= Max(clause_selectivity(root, lion_vcol_unvar(clause), 0,
										   JOIN_INNER, NULL),
						1e-10);
	}
	pfree(orgrp);
	if (!any)
		return Min(cand, Max(ceiling, 1.0)) * Max(counts, 1.0) * poscost;

	cand = Min(cand, Max(ceiling, 1.0));
	pages = Min(cand, heap_pages * Max(counts, 1.0));

	return pages * lion_heap_page_cost(root, rel, pages, heap_pages) +
		cand * (perrow + Max(counts, 1.0) * poscost);
}
