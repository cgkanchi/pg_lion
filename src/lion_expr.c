/*-------------------------------------------------------------------------
 *
 * lion_expr.c
 *		Expression cursors: a boolean expression of posting sets, one
 *		container at a time - the merge's sources, and the stream and iterate
 *		interfaces the scans read them through.
 *
 * Part of the count engine: lion_count.h is its interface, and
 * lion_count_int.h declares what its files share.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "lion_count_int.h"

static const LionContainer *lion_ecursor_container(LionExprCursor *c);
static bool lion_or_probe_pays(const LionExprCursor *c,
							   const LionContainer *acc);
static void lion_or_probe(LionExprCursor *c, const LionContainer *acc,
						  LionContainer *dest);
static void lion_lazy_and(LionExprCursor *c, const LionContainer *acc,
						  LionContainer *dest);
static void lion_ecursor_build(LionExprCursor *c);
static void lion_and_build(LionExprCursor *c);

/* ---------------------------------------------------------------------
 * Expression cursors: one source of the merge
 * --------------------------------------------------------------------- */



/* ---- the leapfrog: the AND of cursors ---- */

/*
 * How many rows a node's sets hold, from what the located sets carry already
 * (`ntids`, the entries' own counts), so that no page is read to decide it:
 * a set's own, a union's sets together - an upper bound - and the least of an
 * intersection's children, which it cannot exceed.  It orders the sources of
 * a leapfrog (lion_leapfrog()); a wrong guess costs pages, never an answer.
 */
static double
lion_node_members(const LionKeyNode *node, const LionPostingSet *sets)
{
	double		m = 0;
	int			i;

	check_stack_depth();

	if (node == NULL)
		return 0;
	if (node->kind == LION_KN_KEY)
		return sets[node->keyno].found ? (double) sets[node->keyno].ntids : 0;
	for (i = 0; i < node->nargs; i++)
	{
		double		c = lion_node_members(node->args[i], sets);

		if (node->kind == LION_KN_OR)
			m += c;
		else if (i == 0 || c < m)
			m = c;
	}
	return m;
}

/*
 * order[0 .. n-1] = 0 .. n-1 sorted by est[], ascending.  An insertion sort,
 * because an AND has a handful of sources and this runs once per cursor; it
 * is STABLE, so of equal estimates the first comes first.
 */
static void
lion_leapfrog_order(const double *est, int n, int *order)
{
	int			i;
	int			j;

	for (i = 0; i < n; i++)
	{
		for (j = i; j > 0 && est[order[j - 1]] > est[i]; j--)
			order[j] = order[j - 1];
		order[j] = i;
	}
}

/*
 * THE LEAPFROG (DESIGN.md §22, §25): the AND of cursors, as the count's merge
 * makes it (lion_run_merge()) and as an AND node of the evaluator makes it
 * (lion_ecursor_build()) - which is what a lion index scan's AND of its quals'
 * sets is, in its stream and in its bitmap (DESIGN.md §29.2), and what the
 * count's multi-key AND trees are.  One function, so that the two cannot do
 * different work on the same sets.
 *
 * It winds cur[order[0 .. n-1]] forward to the next container key at which
 * every one of them has a container and their AND holds a member, and
 * returns that AND, its key in *key; NULL once one of them runs out, or the
 * key reaches `hi` (a chunk of a collection, LionCollect.ranged).
 *
 * order[0] is the DRIVER, the cursor with the fewest members, and the others
 * follow in ascending members.  The driver is the only one that ever steps
 * past a key; the others are left standing and are SOUGHT to the key the
 * driver has reached, which costs each one probe instead of a walk - stepping
 * all of them would cost each a container at key + 1 that the seek is about
 * to skip anyway.  They are taken in that order, each sought and folded into
 * the running intersection before the next one is touched at all, and the key
 * is ABANDONED the moment the intersection is empty, or a cursor sought lands
 * past the key: the ones after it in the order are neither sought nor read.
 * The fewer members a cursor has, the likelier it is to be the one that kills
 * the key, and the smaller the containers ANDed early are, which is what
 * keeps the dense ones - a bitset a key - out of all but the last ANDs.  Any
 * other order is still correct, and reads more.
 *
 * The AND is built in work[0] and work[1] alternately; *w comes back as the
 * one it is NOT in, for a caller that goes on with it (the merge's negated
 * sources).  The containers are left unoptimized (lion_container_and_raw()):
 * they are ANDed again, counted, or optimized once by an AND node whose
 * caller keeps what it hands out (DESIGN.md §15, "Unions and intersections
 * unoptimized").  With n = 1 it is the cursor's own container.  `stats`,
 * when not NULL, counts the seeks an abandoned key saved (EXPLAIN's Probes
 * Avoided): a cursor standing at the key or beyond would not have been
 * sought anyway, so only the ones still below it count.
 *
 * A UNION IS BUILT ONLY WHEN IT PAYS (DESIGN.md §29.11, "Unions probed").  A
 * cursor that is an IN list, a multi-key `&&` or an OR across columns stands
 * at a key with the children that have a container there, and - a lazy one,
 * which every OR under a leapfrog is - without their union built.  The
 * driver's union is built, since the intersection starts from it.  Any other
 * one's is built and ANDed with the running intersection only when that is
 * the cheaper way (lion_or_probe_pays()); otherwise the intersection's
 * members are looked up in the children's containers and the ones found
 * kept (lion_or_probe()), which is the same AND - acc ∩ (c1 ∪ ... ∪ ck) - at
 * the cost of the members the intersection has rather than of those the
 * union would have.  A cursor that is a nested tree - an AND node, an OR of
 * ANDs or of ORs - is lazy the same way, wound to a key and not evaluated
 * there, and is built or evaluated for acc's members alone the same way
 * (lion_lazy_and(), lion_tree_probe(); DESIGN.md §29.11, "Trees probed").
 *
 * THE DESIGN.md §9 PIN DISCIPLINE IS UNCHANGED, and this is the argument.
 * The rule is that the visibility-map question about a container's heap
 * blocks is asked before the pin on the page that container came from is
 * released.  A key that is abandoned asks NO such question - nothing of it
 * reaches the count - so there is no obligation to discharge for any of the
 * pages it touched, sought or not.  The cursors that are not sought keep
 * their PINS exactly where they stood: a pin too many never makes a count
 * wrong, it only makes VACUUM wait (see lion_ecursor_next()).  The pages the
 * sought ones let go of are let go by lion_ecursor_seek(), at keys nothing
 * was counted from.  And the key that is returned has every cursor standing
 * on it, each with the pin its container came with: the caller asks the map
 * before it steps the driver (lion_count_container()), and an AND node hands
 * the key up to a caller that does.  A union probed rather than built changes
 * none of it: its children were sought to the key exactly as they are for a
 * union built (lion_ecursor_seek()), the ones standing there keep the pins
 * their containers came with until the cursor moves past the key, and what
 * the probe keeps is a subset of the running intersection, whose members lie
 * in containers the cursors before it stand on with their pins.
 */
const LionContainer *
lion_leapfrog(LionExprCursor *cur, const int *order, int n,
			  LionContainer *const *work, int *w, uint64 hi,
			  LionCountStats *stats, uint32 *key)
{
	for (;;)
	{
		const LionContainer *acc = NULL;
		uint32		target;
		int			k;
		int			m;

		for (k = 0; k < n; k++)
		{
			if (!cur[order[k]].valid)
				return NULL;	/* one ran out: so has the intersection */
		}

		target = cur[order[0]].ckey;
		for (k = 1; k < n; k++)
		{
			if (cur[order[k]].ckey > target)
				target = cur[order[k]].ckey;
		}
		if ((uint64) target >= hi)
			return NULL;

		*w = 0;
		for (k = 0; k < n; k++)
		{
			LionExprCursor *c = &cur[order[k]];

			if (c->ckey < target)
			{
				lion_ecursor_seek(c, target);
				if (!c->valid)
					return NULL;
			}

			if (c->ckey > target)
			{
				/*
				 * No container at the target at all: the key is dead, as with
				 * an empty intersection, one step earlier.  The cursors after
				 * this one are left standing, and the round starts again at
				 * the key it found, the next one that can possibly survive -
				 * the driver sought there first.
				 */
				if (stats != NULL)
				{
					for (m = k + 1; m < n; m++)
					{
						if (cur[order[m]].ckey < target)
							stats->probes_avoided++;
					}
				}
				target = c->ckey;
				if ((uint64) target >= hi)
					return NULL;
				acc = NULL;
				*w = 0;
				k = -1;
				CHECK_FOR_INTERRUPTS();
				continue;
			}

			if (acc == NULL)
				acc = lion_ecursor_container(c);	/* a union is built here */
			else
			{
				/* built and ANDed, or acc's members looked up in it */
				lion_lazy_and(c, acc, work[*w]);
				acc = work[*w];
				*w ^= 1;
			}

			if (lion_container_cardinality(acc) == 0)
				break;
		}

		if (k >= n)
		{
			*key = target;
			return acc;
		}

		/*
		 * Abandoned: nothing of this key survives, so nothing asks the map
		 * about it and the driver's page may go.  Only the driver steps; the
		 * others still stand at or below the key it leaves and are sought
		 * from there on the next round.
		 */
		if (stats != NULL)
		{
			for (m = k + 1; m < n; m++)
			{
				if (cur[order[m]].ckey < target)
					stats->probes_avoided++;
			}
		}
		lion_ecursor_next(&cur[order[0]]);
		CHECK_FOR_INTERRUPTS();
	}
}

/* ---- planning a tree against the open budget ---- */


/* One leaf: its cursor, and what it copies into or pins. */
void
lion_leaf_cost(const LionPostingSet *ps, bool droppins, Size *mem, int *pins)
{
	*mem = sizeof(LionExprCursor);
	*pins = 0;
	if (ps->found && ps->mat != NULL && lion_mat_spills(ps->mat))
	{
		/* a spilled copy: the container it reads back */
		*mem += lion_alloc_size(MAXALIGN(LION_CONTAINER_MAX_SIZE));
		return;
	}
	if (!ps->found || ps->mat != NULL)
		return;					/* nothing to walk, or a private copy */

	/* the segment buffer, on the first sparse segment (small either way) */
	*mem += lion_alloc_size(LION_CONTAINER_HDRSZ +
							LION_SPARSE_THRESHOLD * sizeof(uint16));
	if (ps->is_inline)
	{
		/* the payload is the set's own; the cursor borrows its leaf pin */
		*mem += lion_alloc_size(lion_inline_stage_size(ps->paylen));
		return;
	}
	*mem += lion_alloc_size(sizeof(PGAlignedBlock));	/* the page image */
	if (!droppins)
		*pins = 1;
}



/*
 * A windowed union holds its images, their keys, the container it hands out
 * and two memory contexts' first blocks; the image count is what the budget
 * pays for, half of it, so that the child open at the time has the other
 * half.
 */
#define LION_WIDE_MIN_IMAGES	8

static int
lion_wide_images(const LionOpenBudget *budget)
{
	Size		n = (budget->mem / 2) / lion_alloc_size(LION_BITSET_BYTES);

	return (int) Min(Max(n, (Size) LION_WIDE_MIN_IMAGES), (Size) (INT_MAX / 2));
}

static inline Size
lion_wide_mem(int maximg)
{
	return sizeof(LionExprCursor) + 2 * ALLOCSET_DEFAULT_INITSIZE +
		lion_alloc_size(LION_CONTAINER_MAX_SIZE) +
		(Size) maximg * (lion_alloc_size(LION_BITSET_BYTES) +
						 sizeof(uint32) + sizeof(uint64 *));
}

/*
 * Plan one node (the comment on LionNodePlan).  With build, p->sub is
 * allocated and every child is planned into it, which is what a cursor is
 * then built from; without, the children are evaluated into a scratch plan
 * and only *p's own fields are filled, which is what the questions below ask
 * (is it pinned, what would it cost) without allocating a plan per leaf of a
 * fifty-thousand-value list.  One pass over the tree either way.
 *
 * droppins says the cursors of this node will not keep their pins whatever
 * the plan says (a bitmap walk, a child of a wide union or a trimmed AND):
 * they cost no pins then, and only memory can make an OR wide.
 */
void
lion_plan_node(LionNodePlan *p, const LionKeyNode *node,
			   const LionPostingSet *sets, const LionOpenBudget *budget,
			   bool droppins, bool build)
{
	LionNodePlan scratch;
	Size		summem;
	Size		maxmem = 0;
	int			sumpins = 0;
	int			firstpinned = -1;
	int			firstpins = 0;
	bool		allpinned = true;
	bool		anypinned = false;
	int			i;

	check_stack_depth();

	p->node = node;
	p->nsub = 0;
	p->sub = NULL;
	p->wide = false;
	p->maximg = 0;
	p->keep = LION_KEEP_ALL;
	p->pinned = true;
	p->mem = 0;
	p->pins = 0;

	if (node == NULL)
		return;					/* yields nothing: pinned vacuously */

	if (node->kind == LION_KN_KEY)
	{
		const LionPostingSet *ps = &sets[node->keyno];

		lion_leaf_cost(ps, droppins, &p->mem, &p->pins);
		/* the leaf rule of lion_source_pinned() */
		p->pinned = (!ps->found || (ps->mat == NULL && !ps->nopin));
		return;
	}

	Assert(node->nargs >= 1);
	if (build)
		p->sub = (LionNodePlan *) palloc0(sizeof(LionNodePlan) * node->nargs);
	p->nsub = node->nargs;

	summem = lion_node_overhead(node->kind, node->nargs);
	for (i = 0; i < node->nargs; i++)
	{
		LionNodePlan *cp = build ? &p->sub[i] : &scratch;

		lion_plan_node(cp, node->args[i], sets, budget, droppins, build);
		summem += cp->mem;
		maxmem = Max(maxmem, cp->mem);
		sumpins += cp->pins;
		if (cp->pinned)
		{
			anypinned = true;
			if (firstpinned < 0)
			{
				firstpinned = i;
				firstpins = cp->pins;
			}
		}
		else
			allpinned = false;
	}

	if (node->kind == LION_KN_OR)
	{
		if (budget != NULL && node->nargs > 1 &&
			(summem > budget->mem || sumpins > budget->pins))
		{
			/*
			 * Too wide to open at once: a windowed union.  Its children are
			 * opened one at a time, with no pin kept, so it holds its window
			 * and the largest child and nothing else - and no container it
			 * yields has a pin behind it, whatever its children would have
			 * had (lion_wide_fill()).
			 */
			p->wide = true;
			p->maximg = lion_wide_images(budget);
			p->mem = lion_wide_mem(p->maximg) + maxmem;
			p->pins = 0;
			p->pinned = false;
		}
		else
		{
			p->mem = summem;
			p->pins = sumpins;
			/* which children contributed is not known in advance: all */
			p->pinned = allpinned;
		}
		return;
	}

	Assert(node->kind == LION_KN_AND);
	p->mem = summem;
	/* every child stands at the key the result was built from: any one */
	p->pinned = anypinned;
	if (budget != NULL && !droppins && sumpins > budget->pins)
	{
		/*
		 * One pin is all the intersection needs, so only the first child that
		 * has one at every key keeps its pins; with none that does, no child
		 * does, because nothing they hold could carry the interlock anyway.
		 */
		p->keep = (firstpinned >= 0) ? firstpinned : LION_KEEP_NONE;
		p->pins = (firstpinned >= 0) ? firstpins : 0;
	}
	else
		p->pins = sumpins;
}

/* A node's plan, children and all, allocated in the current context. */
LionNodePlan *
lion_plan_build(const LionKeyNode *node, const LionPostingSet *sets,
				const LionOpenBudget *budget, bool droppins)
{
	LionNodePlan *p = (LionNodePlan *) palloc0(sizeof(LionNodePlan));

	lion_plan_node(p, node, sets, budget, droppins, true);
	return p;
}

/* ---- the windowed union: an OR too wide to open at once ---- */

/*
 * A WIDE OR NODE (DESIGN.md §15, "Bounded cursors").
 *
 * The k-way merge below opens every child for the whole walk.  A wide union
 * opens them ONE AT A TIME instead: for a window of container keys it reads
 * each child, in turn, from the window's first key up to its end, ORs what it
 * finds into one bitset image per container key, and closes the child before
 * the next is opened.  The images then come out in ascending key order,
 * exactly as the merge would have produced them, and the next window starts
 * at the smallest key any child had past this one.  What it holds is the
 * window and one child's cursors, whatever the number of children: the
 * window's images are at most maximg - half the budget - and its END is
 * not fixed in advance but moves down to whatever key the images run out at,
 * so a sparse union over a vast heap is still ONE window and a dense one is
 * as many as it has to be.
 *
 * The price is that a child is opened once per window instead of once, and
 * the pins.  Every child is walked with droppins, so a container the union
 * yields has no pin behind it at all: a wide union is never `pinned`, and
 * DESIGN.md §9 needs another positive source to carry the interlock for it,
 * or the count rechecks every candidate in the heap (cx.novm), exactly as it
 * does for a set located past the list pin budget.  The disjoint lists that
 * make most wide unions are not left to this: lion_count_sources_run()
 * counts them in pinned batches instead, and a union only goes wide where a
 * batch would not be exact - an OR across columns (§19), a multi-key OR
 * (§17), the bitmap walk of a multicolumn index.
 *
 * Correctness of the window, the part worth arguing:
 *
 *	- every child's containers below wend were ORed into the images, because
 *	  a child is only abandoned at its first key at or past wend;
 *	- wend only ever moves DOWN while the window is filled.  When the images
 *	  are all in use and a key arrives that has none, the largest key the
 *	  window holds is evicted and becomes wend - or, if the new key is larger
 *	  still, the new key does - so every image left is below the new wend,
 *	  and whatever an earlier child had between the new wend and the old one
 *	  was that evicted key alone (it was the largest image);
 *	- nextkey is the smallest key at or past wend that any child had: each
 *	  child reports the key it stopped at, and an eviction reports the
 *	  evicted key, which is below every key an earlier child stopped at.
 *
 * So the next window, started at nextkey, misses nothing, and no key is ever
 * handed out twice because windows never overlap.
 */

typedef struct LionWideOr
{
	MemoryContext cxt;			/* this struct, the images, the arrays */
	MemoryContext childcxt;		/* the one child cursor open at a time */
	LionPostingSet *sets;
	int			nsets;
	LionCountCtx *cx;
	int			maximg;
	int			nimg;			/* images of this window, keys ascending */
	int			nalloc;			/* images allocated; img[nimg..] are free */
	uint32	   *keys;
	uint64	  **img;
	int			pos;			/* the image the cursor stands on */
	uint64		wend;			/* the window ends before this key */
	uint64		nextkey;		/* where the next window starts, or END */
	LionContainer *out;			/* img[pos] as a container */
} LionWideOr;

/*
 * The image of `key`, made if it has none; NULL when the window is full and
 * the key is past everything it holds, in which case the key is the new end.
 */
static uint64 *
lion_wide_image(LionWideOr *w, uint32 key)
{
	int			lo = 0;
	int			hi = w->nimg;
	uint64	   *img;

	while (lo < hi)
	{
		int			mid = lo + (hi - lo) / 2;

		if (w->keys[mid] < key)
			lo = mid + 1;
		else
			hi = mid;
	}
	if (lo < w->nimg && w->keys[lo] == key)
		return w->img[lo];

	if (w->nimg >= w->maximg)
	{
		uint32		last = w->keys[w->nimg - 1];

		if (key > last)
		{
			/* the window ends here; this key starts the next one */
			w->wend = key;
			if (key < w->nextkey)
				w->nextkey = key;
			return NULL;
		}

		/* the largest key leaves the window and ends it; its image is free */
		w->wend = last;
		if (last < w->nextkey)
			w->nextkey = last;
		w->nimg--;
	}

	if (w->nimg < w->nalloc)
		img = w->img[w->nimg];	/* the first free image */
	else
	{
		img = (uint64 *) MemoryContextAlloc(w->cxt, LION_BITSET_BYTES);
		w->nalloc++;
	}
	memset(img, 0, LION_BITSET_BYTES);

	memmove(&w->keys[lo + 1], &w->keys[lo], sizeof(uint32) * (w->nimg - lo));
	memmove(&w->img[lo + 1], &w->img[lo], sizeof(uint64 *) * (w->nimg - lo));
	w->keys[lo] = key;
	w->img[lo] = img;
	w->nimg++;
	return img;
}

/*
 * Where a windowed union's keys end: past every key, or where the chunk a
 * collection is making ends (LionCollect.ranged) - no window reads its
 * children past that, and the union is over there.
 */
static inline uint64
lion_wide_last(const LionWideOr *w)
{
	if (w->cx->collect != NULL && w->cx->collect->ranged)
		return Min(w->cx->collect->hi, LION_WIDE_END);
	return LION_WIDE_END;
}

/*
 * Fill the window that starts at `start` (see the comment on LionWideOr).
 * Each child is built in childcxt with droppins, read up to the window's end,
 * closed, and its memory reset before the next one is built.
 */
static void
lion_wide_fill(LionExprCursor *c, uint64 start)
{
	LionWideOr *w = c->wide;
	const LionNodePlan *plan = c->plan;
	uint64		last = lion_wide_last(w);
	int			i;

	Assert(start < LION_WIDE_END);
	w->nimg = 0;
	w->pos = 0;
	w->wend = last;
	w->nextkey = LION_WIDE_END;

	for (i = 0; i < plan->nsub; i++)
	{
		LionExprCursor sub;
		MemoryContext oldcxt = MemoryContextSwitchTo(w->childcxt);

		lion_ecursor_init(&sub, &plan->sub[i], w->sets, w->nsets, w->cx, true);
		if (sub.valid && (uint64) sub.ckey < start)
			lion_ecursor_seek(&sub, (uint32) start);

		while (sub.valid)
		{
			uint64	   *img;

			if ((uint64) sub.ckey >= w->wend)
			{
				if ((uint64) sub.ckey < w->nextkey &&
					(uint64) sub.ckey < last)
					w->nextkey = sub.ckey;
				break;
			}
			img = lion_wide_image(w, sub.ckey);
			if (img == NULL)
				break;			/* the key is the new end, and next */
			lion_container_or_into_bitset(sub.cur, img);
			lion_ecursor_next(&sub);
			CHECK_FOR_INTERRUPTS();
		}

		lion_ecursor_close(&sub);
		MemoryContextSwitchTo(oldcxt);
		MemoryContextReset(w->childcxt);
		CHECK_FOR_INTERRUPTS();
	}
}

static void
lion_wide_init(LionExprCursor *c, LionPostingSet *sets, int nsets,
			   LionCountCtx *cx)
{
	MemoryContext cxt;
	LionWideOr *w;

	cxt = AllocSetContextCreate(CurrentMemoryContext,
								"lion index union window",
								ALLOCSET_DEFAULT_SIZES);
	w = (LionWideOr *) MemoryContextAllocZero(cxt, sizeof(LionWideOr));
	w->cxt = cxt;
	w->childcxt = AllocSetContextCreate(cxt, "lion index union child",
										ALLOCSET_DEFAULT_SIZES);
	w->sets = sets;
	w->nsets = nsets;
	w->cx = cx;
	w->maximg = Max(c->plan->maximg, 1);
	w->keys = (uint32 *) MemoryContextAlloc(cxt, sizeof(uint32) * w->maximg);
	w->img = (uint64 **) MemoryContextAllocZero(cxt,
												sizeof(uint64 *) * w->maximg);
	w->out = (LionContainer *) MemoryContextAlloc(cxt, LION_CONTAINER_MAX_SIZE);
	c->wide = w;

	/* the first window: a collection's chunk starts at its own first key */
	lion_wide_fill(c, (cx->collect != NULL && cx->collect->ranged) ?
				   cx->collect->lo : 0);
}

/* Stand on the first non-empty image at or after pos, filling as needed. */
static void
lion_wide_build(LionExprCursor *c)
{
	LionWideOr *w = c->wide;

	for (;;)
	{
		for (; w->pos < w->nimg; w->pos++)
		{
			lion_bits_to_container(w->img[w->pos], w->keys[w->pos], w->out);
			if (lion_container_cardinality(w->out) > 0)
			{
				c->ckey = w->keys[w->pos];
				c->cur = w->out;
				c->valid = true;
				return;
			}
		}
		if (w->nextkey >= LION_WIDE_END)
			return;				/* every child is exhausted */
		lion_wide_fill(c, w->nextkey);
	}
}

/* Skip every key below target; lion_ecursor_seek() rebuilds afterwards. */
static void
lion_wide_seek(LionExprCursor *c, uint32 target)
{
	LionWideOr *w = c->wide;

	if ((uint64) target < w->wend)
	{
		while (w->pos < w->nimg && w->keys[w->pos] < target)
			w->pos++;
	}
	else
	{
		/* past this window: the next one starts at the target, or later */
		w->pos = w->nimg;
		if (w->nextkey < LION_WIDE_END && w->nextkey < (uint64) target)
			w->nextkey = target;
	}
}

/* ---- the OR node's min-heap of children, keyed by container key ---- */

static inline void
lion_or_heap_push(LionExprCursor *c, int child)
{
	LionOrHeapEnt ent;
	int			i = c->nheap++;

	Assert(c->sub[child].valid);
	ent.ckey = c->sub[child].ckey;
	ent.child = child;
	ent.cur = c->sub[child].cur;

	while (i > 0)
	{
		int			parent = (i - 1) / 2;

		if (c->heap[parent].ckey <= ent.ckey)
			break;
		c->heap[i] = c->heap[parent];
		i = parent;
	}
	c->heap[i] = ent;
}

static inline LionOrHeapEnt
lion_or_heap_pop(LionExprCursor *c)
{
	LionOrHeapEnt top = c->heap[0];
	LionOrHeapEnt last;
	int			i = 0;

	Assert(c->nheap > 0);
	if (--c->nheap == 0)
		return top;

	last = c->heap[c->nheap];
	for (;;)
	{
		int			l = 2 * i + 1;
		int			r = l + 1;
		int			small = i;
		uint32		smallkey = last.ckey;

		if (l < c->nheap && c->heap[l].ckey < smallkey)
		{
			small = l;
			smallkey = c->heap[l].ckey;
		}
		if (r < c->nheap && c->heap[r].ckey < smallkey)
			small = r;
		if (small == i)
			break;
		c->heap[i] = c->heap[small];
		i = small;
	}
	c->heap[i] = last;
	return top;
}

/* ---- the union of more than two containers, in one pass ---- */

/*
 * The containers are ORed into the image by lion_container_or_into_bitset(),
 * the container library's own: this file used to carry a copy of it, which
 * lacked the library's masks and wrote up to 12 KiB past the image for a
 * damaged container (2026-09-27 review).
 *
 * Turn the accumulated image into a container in dest (capacity
 * LION_CONTAINER_MAX_SIZE), in the smallest representation, exactly as
 * lion_container_or() would have left it.
 */
void
lion_bits_to_container(const uint64 *w, uint32 ckey, LionContainer *dest)
{
	uint64		card = 0;
	int			k;

	for (k = 0; k < LION_BITSET_WORDS; k++)
		card += pg_popcount64(w[k]);

	lion_container_init(dest, ckey);
	if (card == 0)
		return;					/* an empty ARRAY; the caller drops it */

	/*
	 * Written straight into the payload rather than through
	 * lion_container_append_sorted() once per member: the image IS a BITSET
	 * payload, so the whole container key costs one memcpy whatever its
	 * cardinality.  lion_container_optimize() then picks the representation,
	 * and in assert builds lion_container_check() confirms that what was built
	 * by hand is a container the rest of the code may be handed.
	 */
	Assert(card <= LION_CONTAINER_RANGE);
	lion_container_to_bitset(dest);
	memcpy(LION_BITSET_DATA(dest), w, LION_BITSET_BYTES);
	dest->cardinality = (uint16) card;
	lion_container_optimize(dest);

#ifdef USE_ASSERT_CHECKING
	{
		const char *why = NULL;

		Assert(lion_container_check(dest, LION_CONTAINER_MAX_SIZE, &why));
	}
#endif
}

/*
 * The union of the containers standing at one key, for a count (c->raw;
 * DESIGN.md §15, "Unions and intersections unoptimized"), whose merge ANDs
 * it with the other sources and counts it and never keeps it.  Unoptimized:
 *
 *	- ARRAYs of LION_OR_FOLD_MEMBERS members or fewer together are folded
 *	  pairwise, each union a merge of two arrays and nothing else - the
 *	  fold's cost grows with the containers times their members, so only
 *	  while those are small;
 *	- anything else is ORed into one bitset image, written in place as the
 *	  payload of a BITSET container and counted once.  The image costs a
 *	  fixed 4 kB clear and count, which a union of a hundred-odd members
 *	  already repays: the pairwise fold of four dense ARRAYs merged, counted
 *	  its runs and converted to a RUN three times over at every key, only for
 *	  the AND after it to fill it back into a bitset; and three ARRAYs of 110
 *	  members, folded in two merges, took a sixth longer than their image on
 *	  the repro of §15's measurements.
 *
 * Which of the two the old rule took depended on the containers alone
 * (LION_OR_BITSET_MIN), which is still where a fold stops.
 */
#define LION_OR_FOLD_MEMBERS	128

static const LionContainer *
lion_or_hot_raw(LionExprCursor *c, uint32 ckey)
{
	const LionContainer *a;
	uint32		members = 0;
	bool		arrays = true;
	int			w = 0;
	int			i;

	for (i = 0; i < c->nhot; i++)
	{
		members += lion_container_cardinality(c->hot[i].cur);
		if (c->hot[i].cur->type != LION_CT_ARRAY)
			arrays = false;
	}

	if (arrays && members <= LION_OR_FOLD_MEMBERS &&
		c->nhot < LION_OR_BITSET_MIN)
	{
		a = c->hot[0].cur;
		for (i = 1; i < c->nhot; i++)
		{
			lion_container_or_raw(a, c->hot[i].cur, c->acc[w]);
			a = c->acc[w];
			w ^= 1;
		}
		return a;
	}

	lion_container_bitset_init(c->acc[0], ckey);
	for (i = 0; i < c->nhot; i++)
		lion_container_or_into_bitset(c->hot[i].cur,
									  LION_BITSET_DATA(c->acc[0]));
	(void) lion_container_bitset_recount(c->acc[0]);
	return c->acc[0];
}

/*
 * Is a node that drives a leapfrog still to be lazy there?  A union, yes: the
 * leapfrog asks for its container at the keys it keeps, and building it at
 * every key the union stands at would build it at keys the leapfrog gives up
 * as well.  An AND node, no (DESIGN.md §29.11, "Trees probed"): wound to a
 * key it seeks every child there before intersecting any, where its own
 * leapfrog stops at the first child that empties the key - and as the driver
 * it is intersected at every key it stands at anyway.
 */
bool
lion_leapfrog_lazy(const LionKeyNode *node)
{
	return node == NULL || node->kind != LION_KN_AND;
}

/*
 * Build the cursor of one planned node (lion_plan_node()).  droppins: carry
 * no interlock anywhere below - a child of a wide union, a child of a
 * trimmed AND other than the one that keeps its pins.  Its container is
 * what cx->raw says: unoptimized for a count, optimized for a stream, whose
 * caller copies it.
 */
void
lion_ecursor_init(LionExprCursor *c, const LionNodePlan *plan,
				 LionPostingSet *sets, int nsets, LionCountCtx *cx,
				 bool droppins)
{
	lion_ecursor_init_ex(c, plan, sets, nsets, cx, droppins, cx->raw, false);
}

/*
 * ... with raw: its containers may be left unoptimized - a count's, and those
 * of every child of an AND node, which the leapfrog ANDs and never hands on
 * as they are (the node optimizes what it hands up when it is not raw
 * itself); and lazy: under a leapfrog, whose OR is not built until it is
 * asked for (lion_ecursor_container(), DESIGN.md §29.11, "Unions probed").
 * An OR's children are what the OR is: its union of one child is that
 * child's container.
 */
void
lion_ecursor_init_ex(LionExprCursor *c, const LionNodePlan *plan,
					 LionPostingSet *sets, int nsets, LionCountCtx *cx,
					 bool droppins, bool raw, bool lazy)
{
	const LionKeyNode *node = plan->node;
	int			i;

	check_stack_depth();

	memset(c, 0, sizeof(LionExprCursor));
	c->node = node;
	c->plan = plan;
	c->raw = raw;
	c->cx = cx;
	if (node == NULL)
		return;					/* a source with no sets at all */

	c->kind = node->kind;

	/*
	 * Lazy under a leapfrog: a union, not built until it is asked for, and -
	 * where trees may be probed - an AND node, whose children are wound to a
	 * key they all have a container at and not intersected until it is asked
	 * for (DESIGN.md §29.11, "Unions probed" and "Trees probed").
	 */
	c->lazy = lazy && !plan->wide &&
		(node->kind == LION_KN_OR ||
		 (node->kind == LION_KN_AND && node->nargs > 1 &&
		  lion_enable_tree_probe));

	if (node->kind == LION_KN_KEY)
	{
		Assert(node->keyno >= 0 && node->keyno < nsets);
		lion_cursor_init(&c->leaf, &sets[node->keyno], cx, droppins);
	}
	else if (plan->wide)
	{
		Assert(node->kind == LION_KN_OR);
		lion_wide_init(c, sets, nsets, cx);
	}
	else
	{
		/*
		 * The children of a leapfrog: raw, and lazy where they are unions or
		 * trees - and so are the children of a lazy union, where trees may be
		 * probed: an OR of ANDs is evaluated for the members it is probed
		 * with, or built from its children's containers when it is asked for.
		 */
		bool		under = (node->kind == LION_KN_AND && node->nargs > 1);
		bool		sublazy = under ||
			(c->lazy && node->kind == LION_KN_OR && lion_enable_tree_probe);

		Assert(node->nargs >= 1 && plan->nsub == node->nargs);
		c->nsub = node->nargs;
		c->sub = (LionExprCursor *) palloc0(sizeof(LionExprCursor) * c->nsub);

		if (node->kind == LION_KN_AND)
		{
			/* the leapfrog's order: fewest members first (lion_leapfrog()) */
			double	   *est = (double *) palloc(sizeof(double) * c->nsub);

			for (i = 0; i < c->nsub; i++)
				est[i] = lion_node_members(node->args[i], sets);
			c->order = (int *) palloc(sizeof(int) * c->nsub);
			lion_leapfrog_order(est, c->nsub, c->order);
			pfree(est);
		}

		/*
		 * ... but the child that drives an AND node's leapfrog is always
		 * asked for its container, and an AND node there is built as it is
		 * sought, with the early exit of its own leapfrog, rather than wound
		 * to a key first (lion_leapfrog_lazy()).
		 */
		for (i = 0; i < c->nsub; i++)
			lion_ecursor_init_ex(&c->sub[i], &plan->sub[i], sets, nsets, cx,
								 droppins ||
								 (plan->keep != LION_KEEP_ALL && plan->keep != i),
								 under || raw,
								 sublazy &&
								 !(under && i == c->order[0] &&
								   !lion_leapfrog_lazy(node->args[i])));

		if (c->nsub > 1)
		{
			c->acc[0] = (LionContainer *) palloc(LION_CONTAINER_MAX_SIZE);
			c->acc[1] = (LionContainer *) palloc(LION_CONTAINER_MAX_SIZE);
		}

		if (node->kind == LION_KN_OR)
		{
			c->hot = (LionOrHeapEnt *) palloc(sizeof(LionOrHeapEnt) * c->nsub);
			c->heap = (LionOrHeapEnt *) palloc(sizeof(LionOrHeapEnt) * c->nsub);
			if (c->nsub > 2)
				c->bits = (uint64 *) palloc(LION_BITSET_BYTES);
			if (c->lazy)
				c->hotc = (const LionContainer **)
					palloc(sizeof(LionContainer *) * c->nsub);

			/*
			 * Every child that has a container goes on the heap; build()
			 * takes the ones standing at the smallest key back off it.
			 */
			for (i = 0; i < c->nsub; i++)
			{
				if (c->sub[i].valid)
					lion_or_heap_push(c, i);
			}
		}
	}

	lion_ecursor_build(c);
}

/*
 * The union of the children standing at the key, c->hot[] (at least two), in
 * c->cur - unoptimized for a raw cursor (lion_or_hot_raw()), else in the
 * smallest representation, as a stream's caller copies it.
 */
static void
lion_or_build(LionExprCursor *c)
{
	int			w = 0;
	int			i;

	Assert(c->kind == LION_KN_OR && c->nhot >= 1);
	c->pending = false;

	/*
	 * A child still pending - an AND node of a lazy OR's, wound to the key
	 * and not intersected (DESIGN.md §29.11, "Trees probed") - is built
	 * first; the heap's entry carried no container for it.
	 */
	for (i = 0; i < c->nhot; i++)
	{
		if (c->hot[i].cur == NULL)
			c->hot[i].cur = lion_ecursor_container(&c->sub[c->hot[i].child]);
	}
	if (c->nhot == 1)
	{
		c->cur = c->hot[0].cur;
		return;
	}
	c->cx->stats.unions_built++;

	if (c->raw)
		c->cur = lion_or_hot_raw(c, c->ckey);
	else if (c->nhot < LION_OR_BITSET_MIN)
	{
		const LionContainer *a = c->hot[0].cur;

		for (i = 1; i < c->nhot; i++)
		{
			lion_container_or(a, c->hot[i].cur, c->acc[w]);
			a = c->acc[w];
			w ^= 1;
		}
		c->cur = a;
	}
	else
	{
		/* One pass over the containers, one container built at the end. */
		memset(c->bits, 0, LION_BITSET_BYTES);
		for (i = 0; i < c->nhot; i++)
			lion_container_or_into_bitset(c->hot[i].cur, c->bits);
		lion_bits_to_container(c->bits, c->ckey, c->acc[0]);
		c->cur = c->acc[0];
	}
}

/*
 * The container the cursor stands on, the union of a lazy OR's children
 * built if it has not been (lion_leapfrog()).
 */

static const LionContainer *
lion_ecursor_container(LionExprCursor *c)
{
	Assert(c->valid);
	if (c->pending)
	{
		if (c->kind == LION_KN_OR)
			lion_or_build(c);
		else
			lion_and_build(c);
	}
	return c->cur;
}

/*
 * BUILD THE UNION, OR PROBE ITS MEMBERS (DESIGN.md §29.11, "Unions probed").
 *
 * The AND of a running intersection acc with the union of the containers a
 * lazy OR's children have at the key can be made two ways, with the same
 * members as the result: build the union (lion_or_build()) and AND acc with
 * it, or look acc's members up in each child's container in turn, a member
 * once found not again, and keep the ones found (lion_or_probe(),
 * lion_container_and_union_raw()).  The first costs every member of the
 * union - a bitset image cleared, filled and counted, or for a few small
 * ARRAYs a fold of merges - and then the AND; the second costs acc's members
 * a child - a bit test in a BITSET, a galloping search through an ARRAY, a
 * search of a RUN's runs or a merge with them - and acc's extraction when
 * acc is not an ARRAY.  So the probe wins where the intersection is small
 * against the union - a few rows of a selective AND against a list's
 * containers of hundreds or thousands - and the union where the intersection
 * is dense, or the list's containers are small.
 *
 * Which is cheaper is estimated here from what the containers' headers say,
 * in quarters of a nanosecond as a harness beside the container library
 * measured them (-O2, containers of a heap of 70 rows a block, members at
 * every offset a row can have): a bit test 2 ns; a gallop through an ARRAY
 * of s members 2 ns a step there and 3 in a count, whose containers are not
 * all in the first-level cache, 1 + log2(1 + s/a) steps a member of acc; a
 * search of r runs 1.5 ns a step, a merge with them 0.75 ns a run and 3 a
 * member; and for the union a member of an ARRAY 1 ns, a BITSET 170, a run 2,
 * the image's clear and count 150 (52 ns of which is the count on PostgreSQL
 * 18's AVX-512 popcount; 16's takes 670), a fold 1.25 ns a member a fold,
 * and acc's AND with it 1.5 ns a member.  The estimate is integer
 * arithmetic and its logarithms fixed point (lion_log2_16()): it is made at
 * every key a union is met at, and made in floating point it took 2 to 3%
 * of a count that builds its unions anyway.  Rounded to whole steps instead,
 * it took 120 members against two ARRAYs of 90 for one step each where they
 * take 1.8, probed them, and the count that built its union in 2.8 ms took
 * 4.2.  It takes every member of acc to be looked up in every child, where
 * one found is not looked up again: it errs towards the union.  An acc of
 * more than an ARRAY's worth of members is never probed.
 */
#define LION_UP_BIT			8	/* quarters of a nanosecond */
#define LION_UP_GALLOP		12
#define LION_UP_RUNSEARCH	6
#define LION_UP_RUNMERGE	3
#define LION_UP_RUNMERGE_A	12
#define LION_UP_EXTRACT		400 /* a BITSET's 512 words scanned */
#define LION_UP_MEMBER_OR	4
#define LION_UP_BITSET_OR	680
#define LION_UP_RUN_OR		8
#define LION_UP_IMAGE		600
#define LION_UP_FOLD		5
#define LION_UP_AND			6

/* GUC pg_lion.enable_union_probe (DESIGN.md §29.11, "Unions probed") */
bool		lion_enable_union_probe = true;

/* GUC pg_lion.enable_tree_probe (DESIGN.md §29.11, "Trees probed") */
bool		lion_enable_tree_probe = true;

/*
 * 16 log2(x) for x >= 1, within a sixteenth or so: the leading bit's position
 * and the four bits after it, as a straight line between two powers of two.
 */
static inline uint32
lion_log2_16(uint32 x)
{
	int			k = pg_leftmost_one_pos32(x);
	uint32		rest = x - ((uint32) 1 << k);

	return (uint32) k * 16 + ((k >= 4) ? rest >> (k - 4) : rest << (4 - k));
}

/* A RUN's runs, as many as its payload may hold at most. */
static inline uint32
lion_up_runs(const LionContainer *h)
{
	return Min((uint32) LION_RUN_NRUNS((LionContainer *) h),
			   (uint32) LION_RUN_MAX_NRUNS);
}

/*
 * What looking a members up in container h costs, in quarters of a
 * nanosecond: a bit test each in a BITSET; a search of a RUN's runs for a
 * few, else a merge with them; a gallop through an ARRAY of m members,
 * 1 + log2(1 + m/a) steps a member - 16 + 16 log2(16 + 16 m/a) - 64
 * sixteenths.
 */
static inline uint64
lion_up_lookup(const LionContainer *h, uint64 a)
{
	if (h->type == LION_CT_BITSET)
		return LION_UP_BIT * a;
	if (h->type == LION_CT_RUN)
	{
		uint32		r = lion_up_runs(h);

		return ((uint64) r >= 8 * a) ?
			LION_UP_RUNSEARCH * a * lion_log2_16(r + 1) / 16 :
			LION_UP_RUNMERGE * r + LION_UP_RUNMERGE_A * a;
	}
	return LION_UP_GALLOP * a *
		(lion_log2_16((uint32) (16 * lion_container_cardinality(h) / a) + 16) -
		 48) / 16;
}

static bool
lion_or_probe_pays(const LionExprCursor *c, const LionContainer *acc)
{
	uint64		a = lion_container_cardinality(acc);
	uint64		probe = 0;
	uint64		unite = 0;
	uint64		members = 0;
	bool		arrays = true;
	int			i;

	Assert(c->pending && c->nhot > 1);
	if (!lion_enable_union_probe || a > LION_ARRAY_MAX_CARD)
		return false;
	a = Max(a, 1);

	if (acc->type == LION_CT_BITSET)
		probe += LION_UP_EXTRACT + 4 * a;
	else if (acc->type == LION_CT_RUN)
		probe += 4 * a;

	for (i = 0; i < c->nhot; i++)
	{
		const LionContainer *h = c->hot[i].cur;
		uint64		m = lion_container_cardinality(h);

		members += m;
		probe += lion_up_lookup(h, a);
		if (h->type == LION_CT_BITSET)
		{
			unite += LION_UP_BITSET_OR;
			arrays = false;
		}
		else if (h->type == LION_CT_RUN)
		{
			unite += LION_UP_RUN_OR * (uint64) lion_up_runs(h);
			arrays = false;
		}
		else
			unite += LION_UP_MEMBER_OR * m;
	}

	/* lion_or_hot_raw()'s two ways, and acc's AND with what it builds */
	if (arrays && members <= LION_OR_FOLD_MEMBERS &&
		c->nhot < LION_OR_BITSET_MIN)
		unite = LION_UP_FOLD * members * (uint64) c->nhot +
			LION_UP_AND * (a + members);
	else
		unite += LION_UP_IMAGE +
			((acc->type == LION_CT_ARRAY) ? LION_UP_AND * a : LION_UP_IMAGE);

	return probe < unite;
}

/*
 * dest = acc AND the union of c->hot[], without building it: acc's members
 * looked up in the children's containers (lion_container_and_union_raw()).
 * The union stays unbuilt; nothing else asks for it at this key.
 */
static void
lion_or_probe(LionExprCursor *c, const LionContainer *acc, LionContainer *dest)
{
	int			i;

	Assert(c->pending && c->hotc != NULL);
	for (i = 0; i < c->nhot; i++)
		c->hotc[i] = c->hot[i].cur;
	(void) lion_container_and_union_raw(acc, c->hotc, (uint32) c->nhot, dest);
	c->cx->stats.unions_probed++;
}

/*
 * TREES PROBED (DESIGN.md §29.11).  A source of the count's merge, or a child
 * of an AND node, may be a tree of its own: a tsquery `(a | b | c) & (d |
 * e)` is an AND of two ORs, and an OR across columns of ANDed clauses an OR
 * of ANDs.  Such a tree was its own cursor, built whole at every key it was
 * sought to: its AND node leapfrogged its children and its driver OR's
 * union was always built - a bitset image of every member of every one of
 * its sets' containers at the key - however few rows the running
 * intersection that met it had left.  Where its sets are dense and the
 * intersection is a key's own rows of an FK-side join's count, a row or two
 * a container, all of that was spent to keep those few.
 *
 * So under a leapfrog an AND node is LAZY, as an OR is (lion_ecursor_init_ex()):
 * sought to a key it winds its children to the first key at or after it that
 * every one of them has a container at (lion_and_align()) and stops there,
 * PENDING, its intersection not made; and the children of a lazy OR are lazy
 * too.  The leapfrog that meets it then either builds it - the children's
 * containers intersected at the key, each the cheaper of the two ways again
 * (lion_and_build()) - and ANDs the running intersection with that, or
 * evaluates the tree for the intersection's members alone (lion_tree_probe()):
 * each leaf's container probed with the members still in question, an AND
 * node's children each given the members the one before it kept, an OR
 * node's the ones none before it found - the same members, acc ∩ tree, at the
 * cost of acc's members a leaf rather than of the tree's.  Which is cheaper is
 * estimated from the containers standing at the key (lion_tree_estimate(), in
 * lion_or_probe_pays()'s units): the lookups of every member of acc in every
 * leaf - which errs towards building - against the unions' images, their
 * members and the ANDs building would make.
 *
 * THE DESIGN.md §9 ARGUMENT IS THE LEAPFROG'S (lion_leapfrog()), and a lazy
 * AND adds nothing to it.  Winding the children to a key is what seeking them
 * there is, and nothing of a key they pass over reaches the visibility map; a
 * lazy node stands at its key with every child standing there too, each with
 * the pin its container came with - a trimmed AND with its kept child's -
 * until it moves past it; and what the probe keeps is a subset of the running
 * intersection, whose members lie in containers the cursors before it stand
 * on with their pins.  A node that is built is the eager node's container at
 * that key, made the same way.  What changes is only that a key the tree has
 * no row at is found out by the leapfrog that meets it - at the cost of its
 * probe - rather than by the tree running ahead to its next row, which the
 * leapfrog would then have sought every other source to.
 */
typedef struct LionTreeEst
{
	uint64		probe;			/* looking the members up in every leaf */
	uint64		build;			/* building its container at the key */
	uint64		members;		/* what that container would hold, at most */
	bool		bits;			/* ... and whether as a BITSET */
} LionTreeEst;

/* What ANDing two containers of m1 and m2 members costs, likewise. */
static inline uint64
lion_up_and(uint64 m1, bool b1, uint64 m2, bool b2)
{
	if (b1 && b2)
		return 2 * LION_UP_EXTRACT;
	if (b1)
		return LION_UP_BIT * m2;
	if (b2)
		return LION_UP_BIT * m1;
	return LION_UP_AND * (m1 + m2);
}

static void
lion_tree_estimate(const LionExprCursor *c, uint64 a, LionTreeEst *e)
{
	int			i;

	check_stack_depth();
	memset(e, 0, sizeof(LionTreeEst));
	if (!c->pending)
	{
		/* a leaf, or a node whose container is there: a container to probe */
		e->probe = lion_up_lookup(c->cur, a);
		e->members = lion_container_cardinality(c->cur);
		e->bits = (c->cur->type == LION_CT_BITSET);
		return;
	}

	if (c->kind == LION_KN_OR)
	{
		uint64		unite = 0;
		bool		arrays = true;
		bool		bits = false;

		for (i = 0; i < c->nhot; i++)
		{
			const LionContainer *h = c->hot[i].cur;
			LionTreeEst s;

			if (h != NULL)
			{
				s.probe = lion_up_lookup(h, a);
				s.build = 0;
				s.members = lion_container_cardinality(h);
				s.bits = (h->type == LION_CT_BITSET);
				if (h->type == LION_CT_RUN)
				{
					unite += LION_UP_RUN_OR * (uint64) lion_up_runs(h);
					arrays = false;
				}
				else
					unite += s.bits ? LION_UP_BITSET_OR :
						LION_UP_MEMBER_OR * s.members;
			}
			else
			{
				lion_tree_estimate(&c->sub[c->hot[i].child], a, &s);
				unite += s.bits ? LION_UP_BITSET_OR :
					LION_UP_MEMBER_OR * s.members;
			}
			if (s.bits)
				arrays = false;
			bits = s.bits;
			e->probe += s.probe;
			e->build += s.build;
			e->members += s.members;
		}

		/* lion_or_hot_raw()'s two ways; the union of one is its container */
		if (c->nhot == 1)
			e->bits = bits;
		else if (arrays && e->members <= LION_OR_FOLD_MEMBERS &&
				 c->nhot < LION_OR_BITSET_MIN)
			e->build += LION_UP_FOLD * e->members * (uint64) c->nhot;
		else
		{
			e->build += unite + LION_UP_IMAGE;
			e->bits = true;
		}
		e->members = Min(e->members, (uint64) LION_CONTAINER_RANGE);
		return;
	}

	/* an AND node: its children in the leapfrog's order, each ANDed in */
	for (i = 0; i < c->nsub; i++)
	{
		LionTreeEst s;

		lion_tree_estimate(&c->sub[c->order[i]], a, &s);
		e->probe += s.probe;
		e->build += s.build;
		if (i == 0)
		{
			e->members = s.members;
			e->bits = s.bits;
			continue;
		}
		e->build += lion_up_and(e->members, e->bits, s.members, s.bits);
		e->members = Min(e->members, s.members);
		e->bits = e->bits && s.bits;
	}
}

/*
 * Is looking acc's members up in the tree c stands for cheaper than building
 * its container and ANDing acc with it?  An acc of more members than an ARRAY
 * holds is never probed, as for a union.
 */
static bool
lion_tree_probe_pays(const LionExprCursor *c, const LionContainer *acc)
{
	uint64		a = lion_container_cardinality(acc);
	uint64		probe;
	uint64		build;
	LionTreeEst e;

	Assert(c->pending);
	if (!lion_enable_tree_probe || a > LION_ARRAY_MAX_CARD)
		return false;
	a = Max(a, 1);

	lion_tree_estimate(c, a, &e);
	probe = e.probe + 4 * a;	/* the members written out */
	if (acc->type == LION_CT_BITSET)
		probe += LION_UP_EXTRACT + 4 * a;
	else if (acc->type == LION_CT_RUN)
		probe += 4 * a;
	build = e.build + lion_up_and(a, acc->type == LION_CT_BITSET,
								  e.members, e.bits);
	return probe < build;
}

/*
 * Mark in found[] the positions pend[0 .. np - 1] into vals[] whose value
 * the tree c stands for holds at its key: a leaf's container, or a built
 * node's, probed; a lazy OR's children that have a container probed one
 * after the other with what none before found, then each pending child
 * evaluated for what is left; a lazy AND's children in the leapfrog's order,
 * each with what the ones before it kept.  vals[] is ascending, and so is
 * every list of positions made of it.
 */
static void
lion_tree_eval(LionExprCursor *c, const uint16 *vals, const uint16 *pend,
			   uint32 np, uint64 *found)
{
	uint16		rest[LION_ARRAY_MAX_CARD];
	uint64		f[LION_ARRAY_MAX_CARD / 64];
	uint32		nr = np;
	uint32		i;
	uint32		j;
	uint32		k;

	check_stack_depth();
	if (np == 0)
		return;
	Assert(np <= LION_ARRAY_MAX_CARD);
	memcpy(rest, pend, sizeof(uint16) * np);

	if (!c->pending)
	{
		(void) lion_container_probe_members(c->cur, vals, rest, nr, found);
		return;
	}

	if (c->kind == LION_KN_OR)
	{
		for (i = 0; i < (uint32) c->nhot && nr > 0; i++)
		{
			if (c->hot[i].cur != NULL)
				nr = lion_container_probe_members(c->hot[i].cur, vals, rest,
												  nr, found);
		}
		for (i = 0; i < (uint32) c->nhot && nr > 0; i++)
		{
			if (c->hot[i].cur != NULL)
				continue;
			memset(f, 0, sizeof(f));
			lion_tree_eval(&c->sub[c->hot[i].child], vals, rest, nr, f);
			for (j = k = 0; j < nr; j++)
			{
				uint32		p = rest[j];
				uint64		bit = UINT64CONST(1) << (p & 63);

				if (f[p >> 6] & bit)
					found[p >> 6] |= bit;
				else
					rest[k++] = (uint16) p;
			}
			nr = k;
		}
		return;
	}

	for (i = 0; i < (uint32) c->nsub && nr > 0; i++)
	{
		memset(f, 0, sizeof(f));
		lion_tree_eval(&c->sub[c->order[i]], vals, rest, nr, f);
		for (j = k = 0; j < nr; j++)
		{
			uint32		p = rest[j];

			if (f[p >> 6] & (UINT64CONST(1) << (p & 63)))
				rest[k++] = (uint16) p;
		}
		nr = k;
	}
	for (j = 0; j < nr; j++)
		found[rest[j] >> 6] |= UINT64CONST(1) << (rest[j] & 63);
}

/*
 * dest = acc AND the tree c stands for at its key, without building it
 * (lion_tree_eval()).  acc holds at most an ARRAY's members, which
 * lion_tree_probe_pays() checked by its cardinality; a damaged acc whose
 * payload holds more is taken as empty, as lion_container_and_union_raw()
 * takes it.
 */
static void
lion_tree_probe(LionExprCursor *c, const LionContainer *acc,
				LionContainer *dest)
{
	uint16		vals[LION_ARRAY_MAX_CARD];
	uint16		pend[LION_ARRAY_MAX_CARD];
	uint64		found[LION_ARRAY_MAX_CARD / 64];
	uint32		n;
	uint32		i;

	n = lion_container_extract_members(acc, vals, LION_ARRAY_MAX_CARD);
	if (n > LION_ARRAY_MAX_CARD)
		n = 0;
	for (i = 0; i < n; i++)
		pend[i] = (uint16) i;
	memset(found, 0, sizeof(found));
	lion_tree_eval(c, vals, pend, n, found);
	(void) lion_container_array_from_marks(dest, acc->ckey, vals, n, found);
	c->cx->stats.trees_probed++;
}

/*
 * dest = acc AND what cursor c stands on, as the leapfrog makes it past its
 * driver (lion_leapfrog(), lion_and_build()): a pending union or tree probed
 * with acc's members where that is the cheaper way - a union whose children
 * all have a container by lion_or_probe_pays(), anything else nested by
 * lion_tree_probe_pays() - else built and ANDed.
 */
static void
lion_lazy_and(LionExprCursor *c, const LionContainer *acc, LionContainer *dest)
{
	if (c->pending)
	{
		bool		tree = (c->kind == LION_KN_AND);
		int			i;

		for (i = 0; !tree && i < c->nhot; i++)
		{
			if (c->hot[i].cur == NULL)
				tree = true;
		}
		if (!tree)
		{
			if (lion_or_probe_pays(c, acc))
			{
				lion_or_probe(c, acc, dest);
				return;
			}
		}
		else if (lion_tree_probe_pays(c, acc))
		{
			lion_tree_probe(c, acc, dest);
			return;
		}
		else
			c->cx->stats.trees_built++;
	}
	lion_container_and_raw(acc, lion_ecursor_container(c), dest);
}

/*
 * A lazy AND node's children, wound forward to the first key at or after
 * where they stand that every one of them has a container at - the
 * leapfrog's seeks without its ANDs - and the node left PENDING there: its
 * intersection is made only when it is asked for (lion_and_build()), and a
 * leapfrog that meets it may look its own few members up in the children
 * instead (lion_tree_probe()).  Invalid once a child runs out.
 */
static void
lion_and_align(LionExprCursor *c)
{
	for (;;)
	{
		uint32		target = 0;
		int			k;

		for (k = 0; k < c->nsub; k++)
		{
			LionExprCursor *s = &c->sub[c->order[k]];

			if (!s->valid)
				return;
			if (s->ckey > target)
				target = s->ckey;
		}
		for (k = 0; k < c->nsub; k++)
		{
			LionExprCursor *s = &c->sub[c->order[k]];

			if (s->ckey < target)
			{
				lion_ecursor_seek(s, target);
				if (!s->valid)
					return;
			}
			if (s->ckey > target)
				break;			/* none at the target: on to where it is */
		}
		if (k == c->nsub)
		{
			c->ckey = target;
			c->valid = true;
			c->pending = true;
			return;
		}
		CHECK_FOR_INTERRUPTS();
	}
}

/*
 * The intersection of a pending AND node's children at the key they all
 * stand at, made as the leapfrog makes it there: the first in its order
 * built, each other one ANDed in the cheaper way (lion_lazy_and()), stopping
 * at the first that empties it.  It may come out empty, which a leapfrog
 * above takes as a key with no row; the node is optimized for a caller that
 * keeps what it hands out, as an eager one is.
 */
static void
lion_and_build(LionExprCursor *c)
{
	const LionContainer *acc = NULL;
	int			w = 0;
	int			k;

	Assert(c->kind == LION_KN_AND && c->pending && c->nsub > 1);
	c->pending = false;
	for (k = 0; k < c->nsub; k++)
	{
		LionExprCursor *s = &c->sub[c->order[k]];

		Assert(s->valid && s->ckey == c->ckey);
		if (acc == NULL)
			acc = lion_ecursor_container(s);
		else
		{
			lion_lazy_and(s, acc, c->acc[w]);
			acc = c->acc[w];
			w ^= 1;
		}
		if (lion_container_cardinality(acc) == 0)
			break;
	}
	if (!c->raw && (acc == c->acc[0] || acc == c->acc[1]))
		lion_container_optimize((LionContainer *) acc);
	c->cur = acc;
}

/*
 * Recompute the cursor's current container from its children.
 */
static void
lion_ecursor_build(LionExprCursor *c)
{
	const LionContainer *acc;
	int			w = 0;

	c->valid = false;
	c->pending = false;
	c->cur = NULL;

	if (c->node == NULL)
		return;

	if (c->wide != NULL)
	{
		lion_wide_build(c);
		return;
	}

	if (c->kind == LION_KN_KEY)
	{
		if (!c->leaf.valid)
			return;
		c->cur = c->leaf.cur;
		c->ckey = c->cur->ckey;
		c->valid = true;
		return;
	}

	if (c->kind == LION_KN_OR)
	{
		uint32		minckey;

		/*
		 * The k-way merge.  Everything that still has a container is on the
		 * heap, so its root IS the smallest container key any child has left;
		 * the children standing at it come off the heap into hot[] and stay
		 * there until lion_ecursor_next() moves past the key, which is what
		 * keeps their pins - and with them the §9 interlock - in place for as
		 * long as the result is being counted.
		 */
		Assert(c->nhot == 0);

		if (c->nheap == 0)
			return;				/* every child is exhausted */

		minckey = c->heap[0].ckey;
		do
		{
			c->hot[c->nhot++] = lion_or_heap_pop(c);
		} while (c->nheap > 0 && c->heap[0].ckey == minckey);

		c->ckey = minckey;
		c->valid = true;

		/*
		 * The union of one child is its container.  A lazy cursor's of more
		 * is built when lion_leapfrog() asks for it, which it does only for
		 * the driver and where building it is the cheaper way to AND it
		 * (DESIGN.md §29.11, "Unions probed").
		 */
		if (c->nhot == 1 && c->hot[0].cur != NULL)
			c->cur = c->hot[0].cur;
		else if (c->lazy)
			c->pending = true;	/* ... or its one child is pending itself */
		else
			lion_or_build(c);
		return;
	}

	Assert(c->kind == LION_KN_AND);

	/*
	 * A lazy AND node under a leapfrog only winds its children to a key they
	 * all have a container at, and leaves its intersection to whoever asks
	 * for it (DESIGN.md §29.11, "Trees probed").
	 */
	if (c->lazy)
	{
		lion_and_align(c);
		return;
	}

	/*
	 * The children, wound forward to the next key their AND holds a member
	 * at, as the count's merge winds its sources (lion_leapfrog(), DESIGN.md
	 * §22): the one with the fewest members drives, the others are sought to
	 * its keys in ascending members, and a key is abandoned as soon as the
	 * intersection is empty.  Nothing of the keys they pass over ever reaches
	 * the visibility map, so the §9 rule is untouched - this is the same
	 * window the code has always wound laggards forward in.
	 */
	acc = lion_leapfrog(c->sub, c->order, c->nsub, c->acc, &w, LION_WIDE_END,
						NULL, &c->ckey);
	if (acc == NULL)
		return;

	/*
	 * The AND was made of unoptimized containers, and the one handed up is
	 * optimized here when the caller copies it (a stream's, DESIGN.md
	 * §29.3) - once, rather than at every step of the AND as it was.  With
	 * two children or more it is in c->acc[].
	 */
	if (!c->raw && c->nsub > 1)
	{
		Assert(acc == c->acc[0] || acc == c->acc[1]);
		lion_container_optimize((LionContainer *) acc);
	}
	c->cur = acc;
	c->valid = true;
}

/*
 * Move past the current container key.  Only the children that stand at it
 * move; the ones that are ahead (an OR's) stay where they are.  This is the
 * only place a source lets go of a page pin that carried an answer.
 */
void
lion_ecursor_next(LionExprCursor *c)
{
	int			i;

	if (c->node == NULL || !c->valid)
		return;

	if (c->wide != NULL)
	{
		/* the next image; it holds no pin to let go of */
		c->wide->pos++;
		lion_ecursor_build(c);
		return;
	}

	switch (c->kind)
	{
		case LION_KN_KEY:
			lion_cursor_next(&c->leaf);
			break;

		case LION_KN_OR:

			/*
			 * Only the children that stood at this key move; the ones still
			 * on the heap are ahead of it and stay where they are.  A child
			 * that has a container again goes back on the heap, which is the
			 * one place an OR lets go of a page that carried an answer.
			 */
			for (i = 0; i < c->nhot; i++)
			{
				int			child = c->hot[i].child;

				lion_ecursor_next(&c->sub[child]);
				if (c->sub[child].valid)
					lion_or_heap_push(c, child);
			}
			c->nhot = 0;
			break;

		case LION_KN_AND:

			/*
			 * Every child stands at c->ckey, but only ONE of them has to step,
			 * the driver: lion_ecursor_build() then finds it ahead of the
			 * others and SEEKS them to it (DESIGN.md §22), which is one probe
			 * each instead of a walk.  Stepping all of them would cost every
			 * child a container at c->ckey + 1 that the seek is about to skip
			 * anyway.
			 */
			lion_ecursor_next(&c->sub[c->order[0]]);
			break;
	}

	lion_ecursor_build(c);
}

/*
 * Move past every container key below `target` (DESIGN.md §22).
 *
 * This is lion_ecursor_next() with a destination instead of a step, and it
 * obeys the same §9 rule: it is only ever called at a container key whose
 * containers carried no visibility-map obligation - a key the intersection
 * cannot use - so the pins it lets go had nothing counted from them.
 */
void
lion_ecursor_seek(LionExprCursor *c, uint32 target)
{
	int			i;

	if (c->node == NULL || !c->valid || c->ckey >= target)
		return;

	if (c->wide != NULL)
	{
		lion_wide_seek(c, target);
		lion_ecursor_build(c);
		return;
	}

	switch (c->kind)
	{
		case LION_KN_KEY:
			lion_cursor_seek(&c->leaf, target);
			break;

		case LION_KN_OR:

			/*
			 * The children standing at the current key move to the target, and
			 * so does every child the heap still holds below it; the ones
			 * already at or above it stay where they are.  Popping from a
			 * min-heap while its root is below the target touches exactly
			 * those and no more.
			 */
			for (i = 0; i < c->nhot; i++)
			{
				int			child = c->hot[i].child;

				lion_ecursor_seek(&c->sub[child], target);
				if (c->sub[child].valid)
					lion_or_heap_push(c, child);
			}
			c->nhot = 0;

			while (c->nheap > 0 && c->heap[0].ckey < target)
			{
				LionOrHeapEnt ent = lion_or_heap_pop(c);

				lion_ecursor_seek(&c->sub[ent.child], target);
				if (c->sub[ent.child].valid)
					lion_or_heap_push(c, ent.child);
			}
			break;

		case LION_KN_AND:

			/*
			 * Only the driver: lion_ecursor_build() seeks the others to
			 * wherever it lands, one at a time, and not at all past a child
			 * that rules the key out (lion_leapfrog()).
			 */
			lion_ecursor_seek(&c->sub[c->order[0]], target);
			break;
	}

	lion_ecursor_build(c);
}

void
lion_ecursor_close(LionExprCursor *c)
{
	int			i;

	if (c->node == NULL)
		return;

	if (c->wide != NULL)
	{
		/* no child is open between two calls, and none holds a pin */
		MemoryContextDelete(c->wide->cxt);
		c->wide = NULL;
	}
	else if (c->kind == LION_KN_KEY)
		lion_cursor_close(&c->leaf);
	else
	{
		for (i = 0; i < c->nsub; i++)
			lion_ecursor_close(&c->sub[i]);
	}

	c->nheap = 0;
	c->nhot = 0;
	c->valid = false;
	c->cur = NULL;
}

/* ---------------------------------------------------------------------
 * Source expressions
 * --------------------------------------------------------------------- */

/*
 * The tree a source combines its sets with: its own, or the implicit union of
 * all of them.  NULL when the source has no sets at all, which only a negated
 * source can have (a `col IS NOT NULL` on a column with no NULLs).
 */
LionKeyNode *
lion_source_tree(const LionCountSource *src)
{
	LionKeyNode *node;
	LionKeyNode **args;
	int			i;

	if (src->tree != NULL)
		return src->tree;
	if (src->nsets == 0)
		return NULL;

	args = (LionKeyNode **) palloc(sizeof(LionKeyNode *) * src->nsets);
	for (i = 0; i < src->nsets; i++)
	{
		args[i] = (LionKeyNode *) palloc0(sizeof(LionKeyNode));
		args[i]->kind = LION_KN_KEY;
		args[i]->keyno = i;
	}
	if (src->nsets == 1)
		return args[0];

	node = (LionKeyNode *) palloc0(sizeof(LionKeyNode));
	node->kind = LION_KN_OR;
	node->nargs = src->nsets;
	node->args = args;
	return node;
}

/*
 * Can this expression select anything at all?  A key with no entry in the
 * index selects nothing, and an AND of one such key selects nothing however
 * many other keys it has.  Answering that up front is what lets a count over
 * an impossible clause cost one bucket lookup per key and no merge.
 */
bool
lion_source_satisfiable(const LionKeyNode *node, const LionPostingSet *sets)
{
	int			i;

	if (node == NULL)
		return false;

	switch (node->kind)
	{
		case LION_KN_KEY:
			return sets[node->keyno].found;

		case LION_KN_AND:
			for (i = 0; i < node->nargs; i++)
			{
				if (!lion_source_satisfiable(node->args[i], sets))
					return false;
			}
			return true;

		case LION_KN_OR:
			for (i = 0; i < node->nargs; i++)
			{
				if (lion_source_satisfiable(node->args[i], sets))
					return true;
			}
			return false;
	}

	return false;
}

/*
 * Does every container this expression can yield come with a live buffer pin
 * on the page it was read from?  That is the DESIGN.md §9 interlock, and
 * lion_count_sources() has to keep at least one positive source that has it
 * (see the comment on lion_posting_set_materialize()).
 *
 *	- a leaf has it unless its set has been materialized or was located
 *	  without its pin (NOPIN, DESIGN.md §15); a leaf whose key has no entry
 *	  yields nothing, so it has it vacuously;
 *	- an AND has it if ANY child has it, because every child stands at the
 *	  container key the result was built from and so every child's pin is
 *	  still held when the result is counted;
 *	- an OR has it only if EVERY child has it, because which children
 *	  contributed to a given container key is not known in advance;
 *	- and an OR too wide for the open budget has it never: it is read as a
 *	  windowed union whose children keep no pin (DESIGN.md §15, "Bounded
 *	  cursors").
 *
 * That is lion_plan_node()'s `pinned`, and it is asked of it rather than
 * computed again here, so that what the count trusts and how the cursors are
 * built are one decision.  budget is the one the cursors will be built with;
 * NULL means the whole tree opened at once, as a batch of a disjoint list is.
 */
bool
lion_source_pinned(const LionKeyNode *node, const LionPostingSet *sets,
				   const LionOpenBudget *budget)
{
	LionNodePlan p;

	lion_plan_node(&p, node, sets, budget, false, false);
	return p.pinned;
}


/*
 * A pull interface over the evaluator: the containers of one expression over
 * located posting sets, in ascending container key, one per call (DESIGN.md
 * §29.3).  This is what a plain index scan streams its TIDs from, and what
 * lion_sets_iterate() below - the bitmap scan's push form - loops over.
 *
 * keeppins says whether the cursors keep the §9 pin on the page each current
 * container came from until the stream moves past it (the count's rule, and
 * what a scan under a non-MVCC snapshot needs, §29.5), or let go of every
 * posting leaf as soon as they have copied it (a plain scan under an MVCC
 * snapshot).  An INLINE set's own pin is the caller's business either way:
 * lion_posting_set_unpin() it first when no pin is wanted.
 */
struct LionSetStream
{
	LionCountCtx cx;			/* the cursors' statistics and pin mode */
	LionExprCursor cursor;
	bool		empty;			/* no sets at all: nothing to stream */
	bool		first;			/* the cursor stands on the first container,
								 * which has not been handed out yet */
};

/*
 * budget NULL: every node built as it always was (a plain index scan's
 * stream, DESIGN.md §29.5, whose pins a non-MVCC snapshot relies on and
 * whose long lists lion_scan.c batches itself); otherwise the tree is planned
 * against it (lion_plan_node()), which is what the bitmap walk below wants.
 */
static LionSetStream *
lion_stream_begin_budget(int nsets, LionPostingSet *sets, LionKeyNode *tree,
						 bool keeppins, const LionOpenBudget *budget)
{
	LionSetStream *st = (LionSetStream *) palloc0(sizeof(LionSetStream));
	LionCountSource src;
	LionKeyNode *node;

	memset(&src, 0, sizeof(src));
	src.nsets = nsets;
	src.sets = sets;
	src.tree = tree;

	st->cx.vmbuf = InvalidBuffer;
	st->cx.droppins = !keeppins;

	node = lion_source_tree(&src);
	if (node == NULL)
	{
		st->empty = true;
		return st;
	}

	lion_ecursor_init(&st->cursor,
					  lion_plan_build(node, sets, budget, !keeppins),
					  sets, nsets, &st->cx, false);
	st->first = true;
	return st;
}

LionSetStream *
lion_stream_begin(int nsets, LionPostingSet *sets, LionKeyNode *tree,
				  bool keeppins)
{
	return lion_stream_begin_budget(nsets, sets, tree, keeppins, NULL);
}

/*
 * Skip every container key below target.  Only before the first
 * lion_stream_next(): the stream then starts at the first container at or
 * above target, reached by the cursors' seeks (§22) rather than a walk.
 */
void
lion_stream_seek(LionSetStream *st, uint32 target)
{
	Assert(st->first);
	if (!st->empty && st->cursor.valid && st->cursor.ckey < target)
		lion_ecursor_seek(&st->cursor, target);
}

/*
 * The next container, or NULL at the end.  It is valid until the next call:
 * moving past it is what lets go of the pins it was read under.
 */
const LionContainer *
lion_stream_next(LionSetStream *st)
{
	if (st->empty)
		return NULL;

	if (st->first)
		st->first = false;
	else
	{
		lion_ecursor_next(&st->cursor);
		CHECK_FOR_INTERRUPTS();
	}

	return st->cursor.valid ? st->cursor.cur : NULL;
}

const LionContainer *
lion_stream_at(LionSetStream *st, uint32 target)
{
	if (st->empty)
		return NULL;

	/*
	 * Standing on a container already handed out is standing on it still:
	 * the seek moves the cursors past every key below target, and nothing
	 * else.
	 */
	if (st->cursor.valid && st->cursor.ckey < target)
	{
		lion_ecursor_seek(&st->cursor, target);
		CHECK_FOR_INTERRUPTS();
	}
	st->first = false;

	return st->cursor.valid ? st->cursor.cur : NULL;
}

void
lion_stream_far(LionSetStream *st)
{
	st->cx.farseeks = true;
}

void
lion_stream_end(LionSetStream *st)
{
	if (!st->empty)
		lion_ecursor_close(&st->cursor);
	st->empty = true;
}

/*
 * The stream of lion_sets_iterate(), below, for a caller that pulls it: a
 * bitmap scan's intersection of a range with other columns (DESIGN.md §28,
 * "Bitmap scans").
 */
LionSetStream *
lion_stream_begin_bounded(int nsets, LionPostingSet *sets, LionKeyNode *tree)
{
	LionOpenBudget budget;
	Relation	rel = NULL;
	int			i;

	for (i = 0; i < nsets && rel == NULL; i++)
	{
		if (sets[i].found)
			rel = sets[i].index;
	}
	lion_open_budget_init(&budget, rel);
	return lion_stream_begin_budget(nsets, sets, tree, false, &budget);
}

/*
 * Walk the containers of one expression over located posting sets, without
 * any visibility-map interlock: what a bitmap scan of a multi-key opclass
 * (DESIGN.md §17) or of several key columns (§24) needs.  Every TID goes to
 * the executor, which visits the heap for all of them, so no pin has anything
 * to protect here: the cursors let go of every posting leaf as soon as they
 * have copied it (they used to keep them, one per CHAIN set of an IN list, for
 * nothing).  And the tree is planned against work_mem (DESIGN.md §15,
 * "Bounded cursors"), so a union too wide to open at once - the multicolumn
 * scan of `k = ANY ($1) AND x = 1` over a list of 100000 values held 1.7 GB -
 * is read as a windowed union, in ascending container key like any other.
 */
int64
lion_sets_iterate(int nsets, LionPostingSet *sets, LionKeyNode *tree,
				 lion_container_callback cb, void *arg)
{
	LionSetStream *st;
	const LionContainer *c;
	int64		total = 0;

	st = lion_stream_begin_bounded(nsets, sets, tree);

	while ((c = lion_stream_next(st)) != NULL)
	{
		total += (int64) lion_container_cardinality(c);
		if (!cb(c, arg))
			break;
	}

	lion_stream_end(st);
	pfree(st);

	return total;
}

/*
 * The plain form: count the intersection of nsets posting sets.
 */
int64
lion_count_posting_sets(Relation heap, Snapshot snapshot, int nsets,
					   LionPostingSet *sets, LionCountStats *stats)
{
	LionCountSource *sources;
	int64		result;
	int			i;

	Assert(nsets >= 1);

	sources = (LionCountSource *) palloc0(sizeof(LionCountSource) * nsets);
	for (i = 0; i < nsets; i++)
	{
		sources[i].nsets = 1;
		sources[i].sets = &sets[i];
		sources[i].negated = false;
	}

	result = lion_count_sources(heap, snapshot, nsets, sources, stats);

	pfree(sources);
	return result;
}

int64
lion_count_keys(Relation heap, Snapshot snapshot, int nkeys, Relation *indexes,
			   Datum *keys, Oid *keytypes, LionCountStats *stats)
{
	LionPostingSet *sets;
	int64		result;
	int			i;

	Assert(nkeys >= 1);

	sets = (LionPostingSet *) palloc0(sizeof(LionPostingSet) * nkeys);

	for (i = 0; i < nkeys; i++)
		lion_posting_set_lookup_col(indexes[i], 1, keys[i],
								   keytypes ? keytypes[i] : InvalidOid,
								   &sets[i]);

	result = lion_count_posting_sets(heap, snapshot, nkeys, sets, stats);

	for (i = 0; i < nkeys; i++)
		lion_posting_set_release(&sets[i]);

	pfree(sets);
	return result;
}
