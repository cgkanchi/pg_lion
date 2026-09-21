#!/bin/bash
# Rebuild the extension cleanly against the optimized install and re-run the roaring phases only
# (indexes already exist in the benchmark cluster). Usage: bench/rerun_pushdown.sh <prefix> <datadir> <tag> [secs]
set -euo pipefail
PREFIX=${1:?}; DATA=${2:?}; TAG=${3:?}; T=${4:-5}
P=$(cd "$(dirname "$0")/.." && pwd)
. "$P/bench/lib.sh"
bench_build_extension "$PREFIX" "$P"
bench_start_cluster "$PREFIX" "$DATA"
mkdir -p "$P/bench/logs"
bench_psql -tA -c "select count(*) || ' roaring indexes' from pg_class where relname like 'fact_%_roaring'"
SCRATCH=$(dirname "$PREFIX") T=$T ONLY="2b 2c" "$P/bench/bench.sh" > "$P/bench/logs/${TAG}_bench_stdout.txt" 2>&1
cp "$(dirname "$PREFIX")/bench_results.txt" "$P/bench/logs/${TAG}_queries.txt"
cp "$(dirname "$PREFIX")/bench_plans.log" "$P/bench/logs/${TAG}_query_plans.log"
echo RERUN_DONE
