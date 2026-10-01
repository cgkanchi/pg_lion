/*-------------------------------------------------------------------------
 *
 * lion_costs.h
 *		The planner settings that hold lion's cost constants (DESIGN.md §31,
 *		"The settings").
 *
 *		Each is the MULTIPLIER of one price of lion's cost model, in the unit
 *		the price has always been written in - cpu_operator_cost,
 *		cpu_tuple_cost, seq_page_cost or random_page_cost - so that lion's
 *		prices keep moving with core's cost settings, and each defaults to
 *		the value it was fitted or derived at.  They exist to calibrate the
 *		model against a workload, as one tunes random_page_cost, without a
 *		rebuild.  lion_costs.c has the table of them; the macros that
 *		multiply them out (LION_CONTAINER_COST and the others) stay beside the
 *		code that uses them, with the measurement each default comes from.
 *
 *-------------------------------------------------------------------------
 */
#ifndef LION_COSTS_H
#define LION_COSTS_H

/* A plain lion index scan's heap side (lion_amcost.c, DESIGN.md §29.11) */
extern PGDLLIMPORT double lion_plain_fetch_row_cost;
extern PGDLLIMPORT double lion_bitmap_row_cost;
extern PGDLLIMPORT double lion_walk_pass_cost;

/*
 * The merge, the lookups and the heap recheck, wherever posting sets are
 * counted or ANDed (lion_plan_cost.c, DESIGN.md §10 "The units")
 */
extern PGDLLIMPORT double lion_container_cost;
extern PGDLLIMPORT double lion_member_cost;
extern PGDLLIMPORT double lion_probe_cost;
extern PGDLLIMPORT double lion_memory_probe_cost;
extern PGDLLIMPORT double lion_and_member_cost;
extern PGDLLIMPORT double lion_union_key_cost;
extern PGDLLIMPORT double lion_union_member_cost;
extern PGDLLIMPORT double lion_descent_cost;
extern PGDLLIMPORT double lion_union_set_cost;
extern PGDLLIMPORT double lion_recheck_tid_cost;
extern PGDLLIMPORT double lion_recheck_group_tid_cost;
extern PGDLLIMPORT double lion_resident_page_cost;

/* The count's walks: GROUP BY, count(DISTINCT), ranges (§20, §26, §28, §32) */
extern PGDLLIMPORT double lion_entry_count_cost;
extern PGDLLIMPORT double lion_list_group_cost;
extern PGDLLIMPORT double lion_distinct_test_cost;
extern PGDLLIMPORT double lion_range_entry_cost;
extern PGDLLIMPORT double lion_range_union_entry_cost;
extern PGDLLIMPORT double lion_probe_step_cost;
extern PGDLLIMPORT double lion_range_fold_cost;

/* The FK-side join (§27) */
extern PGDLLIMPORT double lion_fkjoin_count_cost;
extern PGDLLIMPORT double lion_fkjoin_row_cost;
extern PGDLLIMPORT double lion_fkjoin_probe_cost;
extern PGDLLIMPORT double lion_fkjoin_probe_page_cost;
extern PGDLLIMPORT double lion_fkjoin_set_cost;
extern PGDLLIMPORT double lion_fkjoin_lookup_cost;
extern PGDLLIMPORT double lion_fkjoin_collect_container_cost;
extern PGDLLIMPORT double lion_fkjoin_copy_count_cost;
extern PGDLLIMPORT double lion_fkjoin_copy_probe_cost;
extern PGDLLIMPORT double lion_fkjoin_copy_member_cost;
extern PGDLLIMPORT double lion_fkjoin_copy_container_cost;
extern PGDLLIMPORT double lion_fkjoin_batch_row_cost;
extern PGDLLIMPORT double lion_fkjoin_sort_compare_cost;
extern PGDLLIMPORT double lion_fkjoin_sort_key_cost;
extern PGDLLIMPORT double lion_fkjoin_sort_seq_page_cost;
extern PGDLLIMPORT double lion_fkjoin_sort_random_page_cost;

/*
 * The units a lion path is priced in against the plan it competes with
 * (DESIGN.md §39): the cost units a millisecond each kind of core plan runs
 * at, as a multiple of the 500 that lion's CPU constants are fitted at (§10,
 * "The reference").
 */
extern PGDLLIMPORT double lion_hashagg_rate;
extern PGDLLIMPORT double lion_agg_rate;
extern PGDLLIMPORT double lion_hashjoin_rate;
extern PGDLLIMPORT double lion_mergejoin_rate;
extern PGDLLIMPORT double lion_nestloop_rate;
extern PGDLLIMPORT double lion_bitmap_rate;

/*
 * The share of a lion path's own price it is offered at (DESIGN.md §39, "The
 * margin"): its price is divided by it, so that it must come to this share
 * of the best core plan's to be chosen.  1 is no margin.
 */
extern PGDLLIMPORT double lion_pushdown_margin;

/*
 * The scale on the page costs while a lion path's own price is summed in a
 * competitor's units (DESIGN.md §39, lion_plan_units.c): one over the
 * competitor's rate, so that the price times the rate is the CPU terms in
 * the competitor's units and the pages in core's.  1 at every other time -
 * lioncostestimate() among them, which prices the AM's own scans as core
 * prices its own.  Every page cost the custom paths' prices are made of reads
 * it: these two, and lion_heap_page_cost().
 */
extern double lion_page_scale;

#define LION_SEQ_PAGE_COST		(seq_page_cost * lion_page_scale)
#define LION_RANDOM_PAGE_COST	(random_page_cost * lion_page_scale)

extern void lion_costs_init(void);

#endif							/* LION_COSTS_H */
