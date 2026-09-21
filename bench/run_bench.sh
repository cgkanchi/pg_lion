#!/bin/bash
# Build the extension against an optimized (no-cassert) PostgreSQL install from a COPY of the source
# tree, start the benchmark cluster on a private endpoint, create the lion indexes, run the query
# suite, stop.  Usage: bench/run_bench.sh <pg_install_prefix> <benchmark_data_dir> [pgbench seconds]
set -euo pipefail
PREFIX=${1:?prefix}; DATA=${2:?datadir}; T=${3:-5}
P=$(cd "$(dirname "$0")/.." && pwd)
. "$P/bench/lib.sh"
bench_build_extension "$PREFIX" "$P"
bench_start_cluster "$PREFIX" "$DATA"
mkdir -p "$P/bench/logs"
bench_psql -c "drop extension if exists pg_lion cascade" >/dev/null
bench_psql -f "$P/bench/gen_roaring.sql" | tee "$P/bench/logs/06_roaring_index_build.log"
SCRATCH=$(dirname "$PREFIX") T=$T "$P/bench/bench.sh" | tee "$P/bench/logs/06_bench_stdout.txt"
cp "$(dirname "$PREFIX")/bench_results.txt" "$P/bench/logs/06_queries_with_roaring_index.txt"
cp "$(dirname "$PREFIX")/bench_plans.log" "$P/bench/logs/06_query_plans_with_roaring_index.log"
echo "done"
