#!/bin/bash
#
# bench/fkjoin_perkey.sh -- where an FK-side join's time goes, per key and per
# key container (DESIGN.md §27, "Where a key's time goes" and "The copy,
# looked up by key").
#
# The shape: a fact whose fk rows are scattered over the heap, a few dozen a
# key and about one to a container, so that each key's count reads as many
# containers as its key has rows; a fact filter kept by a lion index on the
# same table, whose collected copy has a container at nearly every container
# key of the heap; and a dimension selecting one key in ten through a partial
# covering btree.  The query is the forward semi join
#
#   SELECT count(*) FROM fk_fact f
#   WHERE f.flag AND EXISTS (SELECT 1 FROM fk_dim d
#                            WHERE d.id = f.fk AND d.target)
#
# which core turns into an inner join over the unique key, and which the node
# answers as a summed count (LION_JOINFLAG_SUM).  Reported per worker count,
# as medians of --runs: the query's time (server side, planning included),
# and EXPLAIN ANALYZE's Join Count, Join Lookup, Join Child and Fact Filter
# Collect times, summed over the participants as the node prints them, with
# the counters that say what they were spent on.
#
# Usage: bench/fkjoin_perkey.sh [--db NAME] [--fact N] [--dim N]
#                               [--filter N] [--runs N] [--workers "0 3"]
#                               [--force-node] [--skip-load]
#
#   --fact     fact rows (default 20000000: some 1 GB of heap)
#   --dim      dimension keys (default 667000, 30 fact rows a key)
#   --filter   how many rows in 1024 the fact filter keeps (default 3)
#   --force-node  disable core's join methods, so that the node runs even
#              where the planner prefers a hash join
#
# The numbers are only ever comparable WITHIN one machine and one build.
#
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
DB=lion_fkbench
FACT=20000000
DIM=667000
FILTER=3
RUNS=7
WORKERS="0 3"
FORCE=no
LOAD=yes

while [ $# -gt 0 ]; do
	case "$1" in
		--db) DB=$2; shift 2 ;;
		--fact) FACT=$2; shift 2 ;;
		--dim) DIM=$2; shift 2 ;;
		--filter) FILTER=$2; shift 2 ;;
		--runs) RUNS=$2; shift 2 ;;
		--workers) WORKERS=$2; shift 2 ;;
		--force-node) FORCE=yes; shift ;;
		--skip-load) LOAD=no; shift ;;
		-h|--help) sed -n '2,36p' "$0"; exit 0 ;;
		*) echo "unknown option $1" >&2; exit 1 ;;
	esac
done

# a private server: PGHOST and friends set by the caller; else the dev cluster
if [ -z "${PGPORT:-}" ]; then
	eval "$("$ROOT/dev.sh" env)"
fi

psql -X -q -d postgres -tAc \
	"SELECT 1 FROM pg_database WHERE datname = '$DB'" | grep -q 1 ||
	createdb "$DB"
run() { psql -X -q -v ON_ERROR_STOP=1 -d "$DB" "$@"; }

if [ "$LOAD" = yes ]; then
	run <<-SQL
		SET synchronous_commit = on;
		CREATE EXTENSION IF NOT EXISTS pg_lion;
		DROP TABLE IF EXISTS fk_fact, fk_dim;
		CREATE TABLE fk_dim AS
		SELECT md5(g::text)::uuid AS id, (g % 10 = 0) AS target
		FROM generate_series(1, $DIM) g;
		ALTER TABLE fk_dim ADD PRIMARY KEY (id);
		CREATE INDEX fk_dim_target_cov ON fk_dim (target) INCLUDE (id) WHERE target;
		CREATE TABLE fk_fact AS
		SELECT g AS id,
		       md5((1 + (hashint8(g) & 2147483647) % $DIM)::text)::uuid AS fk,
		       (hashint8(g * 7) & 1023) < $FILTER AS flag
		FROM generate_series(1, $FACT) g;
		CREATE INDEX fk_fact_lion ON fk_fact USING lion (flag, fk);
		VACUUM (FREEZE, ANALYZE) fk_fact;
		VACUUM (FREEZE, ANALYZE) fk_dim;
	SQL
fi

Q="SELECT count(*) FROM fk_fact f WHERE f.flag AND EXISTS (SELECT 1 FROM fk_dim d WHERE d.id = f.fk AND d.target)"
SETUP="SET max_parallel_workers_per_gather = \$w;"
if [ "$FORCE" = yes ]; then
	SETUP="$SETUP SET enable_hashjoin = off; SET enable_mergejoin = off; SET enable_nestloop = off;"
fi

echo "# db=$DB fact=$FACT dim=$DIM filter=$FILTER/1024 runs=$RUNS force_node=$FORCE"
echo "# commit=$(cd "$ROOT" && git rev-parse --short HEAD)$(cd "$ROOT" && git diff --quiet || echo '+dirty')"
for w in $WORKERS; do
	run <<-SQL
		${SETUP//\$w/$w}
		CREATE TEMP TABLE r (ms float8, node jsonb);
		DO \$\$
		DECLARE
			t0 timestamptz;
			e jsonb;
			i int;
		BEGIN
			EXECUTE '$Q';		-- warm
			FOR i IN 1 .. $RUNS LOOP
				t0 := clock_timestamp();
				EXECUTE '$Q';
				INSERT INTO r(ms) VALUES (1000 * extract(epoch FROM clock_timestamp() - t0));
				EXECUTE 'EXPLAIN (ANALYZE, FORMAT JSON) $Q' INTO e;
				INSERT INTO r(node) VALUES (jsonb_path_query_first(e,
					'\$.** ? (@."Custom Plan Provider" == "LionCount")'));
			END LOOP;
		END \$\$;
		SELECT $w AS workers,
		       round(percentile_cont(0.5) WITHIN GROUP (ORDER BY ms)::numeric, 1) AS ms,
		       round(percentile_cont(0.5) WITHIN GROUP (ORDER BY (node->>'Join Count Time')::float8)::numeric, 1) AS count_ms,
		       round(percentile_cont(0.5) WITHIN GROUP (ORDER BY (node->>'Join Lookup Time')::float8)::numeric, 1) AS lookup_ms,
		       round(percentile_cont(0.5) WITHIN GROUP (ORDER BY (node->>'Join Child Time')::float8)::numeric, 1) AS child_ms,
		       round(percentile_cont(0.5) WITHIN GROUP (ORDER BY (node->>'Fact Filter Collect Time')::float8)::numeric, 1) AS collect_ms,
		       max((node->>'Join Keys Looked Up')::int8) AS keys,
		       max((node->>'Join Key Containers Read')::int8) AS key_containers,
		       max((node->>'Fact Filter Copy Seeks')::int8) AS copy_seeks,
		       max((node->>'Fact Filter Rows Collected')::int8) AS collected
		FROM r;
		DROP TABLE r;
	SQL
done
