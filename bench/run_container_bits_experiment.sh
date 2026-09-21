#!/bin/bash
# Re-run the roaring phases with a clean -O2 build (15-bit containers), then rebuild the extension
# with 14-bit containers, rebuild the indexes, and measure sizes + the same phases; finally restore 15 bits.
set -uo pipefail
PREFIX=${1:?prefix}; DATA=${2:?datadir}; T=${3:-5}
P=$(cd "$(dirname "$0")/.." && pwd); S=$(dirname $PREFIX)
. "$P/bench/lib.sh"
build_ext() { # $1 = container bits
  local B=$(mktemp -d /tmp/claude-1000/lion_bits$1.XXXX)
  cp -r $P/src $P/Makefile $P/pg_lion.control $P/pg_lion--0.1.sql $P/pg_lion_citext.control $P/pg_lion_citext--0.1.sql $B/; rm -f $B/src/*.o $B/src/*.bc $B/*.so
  mkdir -p $B/test/sql $B/test/isolation
  sed -i "s/^#define LION_CONTAINER_BITS\t\t15/#define LION_CONTAINER_BITS\t\t$1/" $B/src/lion_tid.h
  grep -q "LION_CONTAINER_BITS		$1" $B/src/lion_tid.h || { echo "sed failed"; exit 1; }
  ( cd $B && make -s PG_CONFIG=$PREFIX/bin/pg_config && make -s PG_CONFIG=$PREFIX/bin/pg_config install ) || { echo "build failed"; exit 1; }
  echo "built extension with LION_CONTAINER_BITS=$1"
}
run_phases() { # $1 = tag
  bench_psql -f $P/bench/gen_roaring.sql > $P/bench/logs/$1_roaring_index_build.log 2>&1
  SCRATCH=$S T=$T ONLY="2b 2c" $P/bench/bench.sh > $P/bench/logs/$1_bench_stdout.txt 2>&1
  cp $S/bench_results.txt $P/bench/logs/$1_queries.txt; cp $S/bench_plans.log $P/bench/logs/$1_query_plans.log
  echo "phases done for $1"
}
bench_start_cluster "$PREFIX" "$DATA"
build_ext 15; bench_psql -c "drop extension if exists pg_lion cascade"; run_phases 06b_bits15
build_ext 14; bench_psql -c "drop extension if exists pg_lion cascade"; run_phases 07_bits14
build_ext 15; bench_psql -c "drop extension if exists pg_lion cascade"; bench_psql -f $P/bench/gen_roaring.sql > /dev/null 2>&1
echo EXPERIMENT_DONE
