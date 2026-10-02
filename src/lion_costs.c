/*-------------------------------------------------------------------------
 *
 * lion_costs.c
 *		Lion's cost constants as planner settings (DESIGN.md §31, "The
 *		settings").
 *
 *		Every per-operation price of lion's cost model is the multiple of a
 *		core cost setting that a macro beside its code spells out -
 *		LION_CONTAINER_COST is (lion_container_cost * cpu_operator_cost) - and
 *		the multiplier is a setting, pg_lion.container_cost, so that the model
 *		can be calibrated on a workload by changing settings rather than by
 *		recompiling, as core's own is with random_page_cost.  The unit stays
 *		the core setting the price was always written in, so lion's prices
 *		still scale with core's CPU and page costs, and the default is the
 *		value the price was fitted or derived at: the comment above each
 *		macro is why it is that value.
 *
 *		The rates of DESIGN.md §39 are here too: the cost units a millisecond
 *		of each kind of core plan, as a multiple of the reference the
 *		multipliers are fitted at, which a lion path is priced in against
 *		that kind of plan (lion_plan_units.c).
 *
 *		This file holds nothing but the tables of them and their
 *		registration, called from _PG_init.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <float.h>

#include "utils/guc.h"
#include "utils/memutils.h"

#include "lion_costs.h"

/*
 * The multipliers.  Each is set to its default when lion_costs_init()
 * registers it, before the planner can read it.
 */
double		lion_plain_fetch_row_cost;
double		lion_bitmap_row_cost;
double		lion_walk_pass_cost;
double		lion_container_cost;
double		lion_member_cost;
double		lion_probe_cost;
double		lion_memory_probe_cost;
double		lion_and_member_cost;
double		lion_union_key_cost;
double		lion_union_member_cost;
double		lion_descent_cost;
double		lion_union_set_cost;
double		lion_recheck_tid_cost;
double		lion_recheck_group_tid_cost;
double		lion_resident_page_cost;
double		lion_entry_count_cost;
double		lion_list_group_cost;
double		lion_distinct_test_cost;
double		lion_range_entry_cost;
double		lion_range_union_entry_cost;
double		lion_probe_step_cost;
double		lion_range_fold_cost;
double		lion_fkjoin_count_cost;
double		lion_fkjoin_row_cost;
double		lion_fkjoin_probe_cost;
double		lion_fkjoin_probe_page_cost;
double		lion_fkjoin_set_cost;
double		lion_fkjoin_lookup_cost;
double		lion_fkjoin_collect_container_cost;
double		lion_fkjoin_copy_count_cost;
double		lion_fkjoin_copy_probe_cost;
double		lion_fkjoin_copy_member_cost;
double		lion_fkjoin_copy_container_cost;
double		lion_fkjoin_batch_row_cost;
double		lion_fkjoin_sort_compare_cost;
double		lion_fkjoin_sort_key_cost;
double		lion_fkjoin_sort_seq_page_cost;
double		lion_fkjoin_sort_random_page_cost;
double		lion_hashagg_rate;
double		lion_agg_rate;
double		lion_hashjoin_rate;
double		lion_mergejoin_rate;
double		lion_nestloop_rate;
double		lion_bitmap_rate;
double		lion_pushdown_margin;

typedef struct LionCostSetting
{
	const char *name;			/* the setting */
	double	   *variable;		/* the multiplier it holds */
	double		boot;			/* its default */
	const char *unit;			/* the core cost setting it is a multiple of */
	const char *desc;			/* what it prices */
} LionCostSetting;

#define LION_OP		"cpu_operator_cost"
#define LION_TUPLE	"cpu_tuple_cost"
#define LION_SEQ	"seq_page_cost"
#define LION_RANDOM	"random_page_cost"

/*
 * The settings.  pg_lion.<name>_cost is the multiplier of the macro
 * LION_<NAME>_COST - pg_lion.container_cost of LION_CONTAINER_COST - and the
 * comment above that macro is where its default comes from; the two page
 * costs of the FK-side join's sort are the two terms of
 * LION_FKJOIN_SORT_PAGE_COST, and union_set_cost prices LION_FKJOIN_SET_COST
 * too, which is the same rebuild of a union (lion_plan_cost.c).
 */
static const LionCostSetting lion_cost_settings[] = {
	/* lion_amcost.c: a plain index scan's heap side (DESIGN.md §29.11) */
	{"pg_lion.plain_fetch_row_cost", &lion_plain_fetch_row_cost, 1.0, LION_TUPLE,
	 "Sets the planner's estimate of the cost of a lion plain index scan's fetch of a row past the first on its heap page, beyond a bitmap heap scan's"},
	{"pg_lion.bitmap_row_cost", &lion_bitmap_row_cost, 0.1, LION_OP,
	 "Sets the planner's estimate of the cost of a row's bitmap entry, which a lion plain index scan is charged as a bitmap heap scan is"},
	{"pg_lion.walk_pass_cost", &lion_walk_pass_cost, 5.0, LION_TUPLE,
	 "Sets the planner's estimate of the cost of each entry past the first that a lion plain index scan walks the heap for, beyond a bitmap scan's"},

	/* lion_plan_cost.c: the merge, the lookups and the heap recheck (§10) */
	{"pg_lion.container_cost", &lion_container_cost, 8.0, LION_OP,
	 "Sets the planner's estimate of the cost of reading and counting a container of a lion posting set"},
	{"pg_lion.member_cost", &lion_member_cost, 0.15, LION_OP,
	 "Sets the planner's estimate of the cost of each member of a lion container read, up to 1024 a container"},
	{"pg_lion.probe_cost", &lion_probe_cost, 40.0, LION_OP,
	 "Sets the planner's estimate of the cost of seeking a lion posting tree to a container key"},
	{"pg_lion.memory_probe_cost", &lion_memory_probe_cost, 30.0, LION_OP,
	 "Sets the planner's estimate of the cost of seeking a lion posting set copied into memory to a container key"},
	{"pg_lion.and_member_cost", &lion_and_member_cost, 0.8, LION_OP,
	 "Sets the planner's estimate of the cost of ANDing each member of a lion intersection with what a seek found, or of looking it up in a container of a union"},
	{"pg_lion.union_key_cost", &lion_union_key_cost, 30.0, LION_OP,
	 "Sets the planner's estimate of the cost of building the union of a lion IN list's or multi-key query's containers at a container key"},
	{"pg_lion.union_member_cost", &lion_union_member_cost, 0.25, LION_OP,
	 "Sets the planner's estimate of the cost of each member of a union of lion containers built at a container key"},
	{"pg_lion.descent_cost", &lion_descent_cost, 120.0, LION_OP,
	 "Sets the planner's estimate of the cost of each level of a lion entry directory descended"},
	{"pg_lion.union_set_cost", &lion_union_set_cost, 100.0, LION_TUPLE,
	 "Sets the planner's estimate of the cost of each set of a lion union source that a count builds again"},
	{"pg_lion.recheck_tid_cost", &lion_recheck_tid_cost, 1.5, LION_TUPLE,
	 "Sets the planner's estimate of the cost of each candidate row of a lion count's heap recheck"},
	{"pg_lion.recheck_group_tid_cost", &lion_recheck_group_tid_cost, 6.0, LION_TUPLE,
	 "Sets the planner's estimate of the cost of each candidate row of a grouped lion count's heap recheck"},
	{"pg_lion.resident_page_cost", &lion_resident_page_cost, 120.0, LION_OP,
	 "Sets the planner's estimate of the cost of each page of a resident lion index that a lion count reads"},

	/* lion_plan_cost.c: the count's walks (§20, §26, §28, §32) */
	{"pg_lion.entry_count_cost", &lion_entry_count_cost, 50.0, LION_TUPLE,
	 "Sets the planner's estimate of the cost of each count a lion GROUP BY makes, per entry or pair of entries"},
	{"pg_lion.list_group_cost", &lion_list_group_cost, 18.0, LION_TUPLE,
	 "Sets the planner's estimate of the cost of each count of a lion GROUP BY whose groups an IN list drives"},
	{"pg_lion.distinct_test_cost", &lion_distinct_test_cost, 50.0, LION_TUPLE,
	 "Sets the planner's estimate of the cost of each test of a lion count(DISTINCT) walk"},
	{"pg_lion.range_entry_cost", &lion_range_entry_cost, 40.0, LION_TUPLE,
	 "Sets the planner's estimate of the cost of each entry of a lion range walk counted on its own"},
	{"pg_lion.range_union_entry_cost", &lion_range_union_entry_cost, 12.0, LION_TUPLE,
	 "Sets the planner's estimate of the cost of each small entry of a lion summed range counted with the rest of its leaf"},
	{"pg_lion.probe_step_cost", &lion_probe_step_cost, 2.0, LION_OP,
	 "Sets the planner's estimate of the cost of each container of a set a lion summed range probes"},
	{"pg_lion.range_fold_cost", &lion_range_fold_cost, 420.0, LION_OP,
	 "Sets the planner's estimate of the cost of each fold or merge of a lion range collected as a source into a container of its union that is not a bitset"},

	/* lion_plan_fkjoin_cost.c: the FK-side join (§27) */
	{"pg_lion.fkjoin_count_cost", &lion_fkjoin_count_cost, 14.0, LION_TUPLE,
	 "Sets the planner's estimate of the cost of each count of a lion FK-side join, per dimension row"},
	{"pg_lion.fkjoin_row_cost", &lion_fkjoin_row_cost, 20.0, LION_TUPLE,
	 "Sets the planner's estimate of the cost of each row a lion FK-side join hands up"},
	{"pg_lion.fkjoin_probe_cost", &lion_fkjoin_probe_cost, 10.0, LION_OP,
	 "Sets the planner's estimate of the cost of each probe of a lion FK-side join's count into a fact filter"},
	{"pg_lion.fkjoin_probe_page_cost", &lion_fkjoin_probe_page_cost, 54.0, LION_OP,
	 "Sets the planner's estimate of the cost of each posting page a probe of a lion FK-side join's count into a fact filter reads"},
	{"pg_lion.fkjoin_set_cost", &lion_fkjoin_set_cost, 11.0, LION_TUPLE,
	 "Sets the planner's estimate of the cost of each set of a fact filter that is a union, per count of a lion FK-side join that probes it"},
	{"pg_lion.fkjoin_lookup_cost", &lion_fkjoin_lookup_cost, 75.0, LION_OP,
	 "Sets the planner's estimate of the cost of each key a lion FK-side join looks up on the directory leaf it stands on"},
	{"pg_lion.fkjoin_collect_container_cost", &lion_fkjoin_collect_container_cost, 2.0, LION_OP,
	 "Sets the planner's estimate of the cost of each container of the driving fact filter that a lion FK-side join reads to collect the filters"},
	{"pg_lion.fkjoin_copy_count_cost", &lion_fkjoin_copy_count_cost, 33.0, LION_TUPLE,
	 "Sets the planner's estimate of the cost of each count of a lion FK-side join against its copy of the fact filters"},
	{"pg_lion.fkjoin_copy_probe_cost", &lion_fkjoin_copy_probe_cost, 2.0, LION_OP,
	 "Sets the planner's estimate of the cost of looking a lion FK-side join's copy of the fact filters up and ANDing it, per fk container"},
	{"pg_lion.fkjoin_copy_member_cost", &lion_fkjoin_copy_member_cost, 3.0, LION_OP,
	 "Sets the planner's estimate of the cost of each member of an fk container ANDed with a lion FK-side join's copy of the fact filters"},
	{"pg_lion.fkjoin_copy_container_cost", &lion_fkjoin_copy_container_cost, 20.0, LION_OP,
	 "Sets the planner's estimate of the cost of each container of a lion FK-side join's copy of the fact filters made"},
	{"pg_lion.fkjoin_batch_row_cost", &lion_fkjoin_batch_row_cost, 75.0, LION_OP,
	 "Sets the planner's estimate of the cost of each dimension row's place in a batch a lion FK-side join looks up in key order"},
	{"pg_lion.fkjoin_sort_compare_cost", &lion_fkjoin_sort_compare_cost, 0.25, LION_OP,
	 "Sets the planner's estimate of the cost of each comparison of the sort that makes a lion FK-side join's keys distinct"},
	{"pg_lion.fkjoin_sort_key_cost", &lion_fkjoin_sort_key_cost, 6.0, LION_OP,
	 "Sets the planner's estimate of the cost of each key put into and taken out of the sort that makes a lion FK-side join's keys distinct"},
	{"pg_lion.fkjoin_sort_seq_page_cost", &lion_fkjoin_sort_seq_page_cost, 0.75, LION_SEQ,
	 "Sets the planner's estimate of the cost of each page a lion FK-side join's key sort writes or reads past work_mem, the part read in sequence"},
	{"pg_lion.fkjoin_sort_random_page_cost", &lion_fkjoin_sort_random_page_cost, 0.25, LION_RANDOM,
	 "Sets the planner's estimate of the cost of each page a lion FK-side join's key sort writes or reads past work_mem, the part read at random"},
};

/*
 * THE COMPETITORS' UNITS (DESIGN.md §39).  Lion's CPU constants are fitted at
 * 500 cost units a millisecond, the middle of core's sequential and
 * index-only scans (§10, "The reference"); core's other plans run at rates
 * of their own, and a lion path is priced in the units of the plan it
 * competes with: its own price times that plan's rate over 500
 * (lion_plan_units.c).  pg_lion.<kind>_rate is that ratio
 * for one kind of core plan, its default what §10's table measured; a kind
 * the table has too little of to say, or whose rate is the reference, is
 * priced at 1, as before.  Sequential, index-only and plain index scans are
 * the reference itself and have no setting.
 */
typedef struct LionRateSetting
{
	const char *name;			/* the setting */
	double	   *variable;		/* the rate it holds */
	double		boot;			/* its default */
	const char *desc;			/* the plans it is the rate of */
} LionRateSetting;

static const LionRateSetting lion_rate_settings[] = {
	/* §10: 208 and 157 units a millisecond at 200 and 20,000 groups */
	{"pg_lion.hashagg_rate", &lion_hashagg_rate, 0.42,
	 "a hashed aggregate"},
	/* §10: 401 to 695 over sequential scans, 415 to 536 over index-only ones */
	{"pg_lion.agg_rate", &lion_agg_rate, 1.0,
	 "a plain or sorted aggregate over a scan"},
	/* §10: 280 and 283 with 1,000 to 4,400 rows hashed, 185 at 140,000 */
	{"pg_lion.hashjoin_rate", &lion_hashjoin_rate, 0.5,
	 "a hash join"},
	/* no measurement */
	{"pg_lion.mergejoin_rate", &lion_mergejoin_rate, 1.0,
	 "a merge join"},
	/* §10: 1,057 into a btree; §31: about 2,000 over warm indexes */
	{"pg_lion.nestloop_rate", &lion_nestloop_rate, 2.0,
	 "a nested loop into a parameterized index or bitmap scan"},
	/* §10: 544 to 4,419 page-bound; §22: about 70 TID-bound - no one rate */
	{"pg_lion.bitmap_rate", &lion_bitmap_rate, 1.0,
	 "a bitmap heap scan"},
};

/*
 * Register the settings, from _PG_init.  They are user settings, real-valued
 * from 0 to DBL_MAX as core's cost settings are, and shown by EXPLAIN
 * (SETTINGS) when changed.  pg_settings lists them among the customized
 * options, as it does every extension's: no API puts a custom setting in one
 * of core's groups, and core's own records are not an extension's to edit.
 * The rates are ratios, from a thousandth to a thousand: a rate of 0 would
 * make lion's CPU free.
 */
void
lion_costs_init(void)
{
	MemoryContext oldcxt = MemoryContextSwitchTo(TopMemoryContext);
	int			i;

	for (i = 0; i < (int) lengthof(lion_cost_settings); i++)
	{
		const LionCostSetting *s = &lion_cost_settings[i];

		/* the setting keeps the description: it is made once, and kept */
		DefineCustomRealVariable(s->name,
								 psprintf("%s, in multiples of %s.",
										  s->desc, s->unit),
								 "A multiplier of one price of lion's cost model, for calibrating the model; the default is the value the price was measured or derived at.",
								 s->variable,
								 s->boot,
								 0.0, DBL_MAX,
								 PGC_USERSET,
								 GUC_EXPLAIN,
								 NULL, NULL, NULL);
	}

	for (i = 0; i < (int) lengthof(lion_rate_settings); i++)
	{
		const LionRateSetting *s = &lion_rate_settings[i];

		DefineCustomRealVariable(s->name,
								 psprintf("Sets the planner's estimate of the cost units a millisecond of %s, as a multiple of the rate lion's CPU costs are fitted at.",
										  s->desc),
								 "A lion custom path's own price is this multiple of its fitted price against the cheapest core plan of this kind (DESIGN.md §39); 1 prices it as fitted.",
								 s->variable,
								 s->boot,
								 0.001, 1000.0,
								 PGC_USERSET,
								 GUC_EXPLAIN,
								 NULL, NULL, NULL);
	}

	/*
	 * THE MARGIN (DESIGN.md §39): a lion custom path's own price, converted
	 * into its competitor's units, is divided by it - so that below 1 a lion
	 * path is chosen only where its price is at most this share of the best
	 * core path's, and a near tie goes to core.  1, no margin, by default:
	 * §39's matrix found lion the faster plan in most of its near ties, and
	 * every plan a margin moved moved to a slower one.
	 */
	DefineCustomRealVariable("pg_lion.pushdown_margin",
							 "Sets the share of the cheapest competing plan's cost a lion custom path's own price must come to for it to be chosen.",
							 "The price of a LionCount, LionSemiJoin, LionAntiJoin or LionOrdered path is divided by it (DESIGN.md §39); 1 is no margin.",
							 &lion_pushdown_margin,
							 1.0,
							 0.01, 1.0,
							 PGC_USERSET,
							 GUC_EXPLAIN,
							 NULL, NULL, NULL);

	MemoryContextSwitchTo(oldcxt);
}
