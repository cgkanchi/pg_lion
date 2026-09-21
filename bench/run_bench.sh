#!/bin/bash
# Build the extension against an optimized (no-cassert) PostgreSQL install from a COPY of the source
# tree, start the benchmark cluster, create the roaring indexes, run the query suite, stop.
# Usage: bench/run_bench.sh <pg_install_prefix> <benchmark_data_dir> [pgbench seconds]
set -euo pipefail
PREFIX=${1:?prefix}; DATA=${2:?datadir}; T=${3:-5}
P=$(cd "$(dirname "$0")/.." && pwd)
BUILD=$(mktemp -d /tmp/claude-1000/rbi_bench_build.XXXX)
cp -r $P/src $P/Makefile $P/roaring_index.control $P/roaring_index--0.1.sql $P/roaring_index_citext.control $P/roaring_index_citext--0.1.sql $BUILD/
rm -f $BUILD/src/*.o $BUILD/src/*.bc $BUILD/*.so   # never reuse objects built against another server
mkdir -p $BUILD/test/sql $BUILD/test/isolation
( cd $BUILD && make -s PG_CONFIG=$PREFIX/bin/pg_config && make -s PG_CONFIG=$PREFIX/bin/pg_config install ) || { echo "extension build failed"; exit 1; }
echo "extension installed into $PREFIX"
export PGHOST=/tmp/claude-1000/pgsk PGPORT=54329 PGUSER=postgres PGDATABASE=postgres
$PREFIX/bin/pg_ctl -D $DATA -l $DATA/../bench_pg.log start >/dev/null 2>&1 || true
sleep 2
$PREFIX/bin/psql -X -q -c "drop extension if exists roaring_index cascade" >/dev/null
$PREFIX/bin/psql -X -q -f $P/bench/gen_roaring.sql | tee $P/bench/logs/06_roaring_index_build.log
SCRATCH=$(dirname $PREFIX) T=$T $P/bench/bench.sh | tee $P/bench/logs/06_bench_stdout.txt
cp $(dirname $PREFIX)/bench_results.txt $P/bench/logs/06_queries_with_roaring_index.txt 2>/dev/null || true
cp $(dirname $PREFIX)/bench_plans.log $P/bench/logs/06_query_plans_with_roaring_index.log 2>/dev/null || true
$PREFIX/bin/pg_ctl -D $DATA stop -m fast >/dev/null 2>&1 || true
echo "done; build dir $BUILD"
