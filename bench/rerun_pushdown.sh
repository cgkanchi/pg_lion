#!/bin/bash
# Rebuild the extension cleanly against the optimized install and re-run the roaring phases only
# (indexes already exist in the benchmark cluster). Usage: bench/rerun_pushdown.sh <prefix> <datadir> <tag> [secs]
set -uo pipefail
PREFIX=${1:?}; DATA=${2:?}; TAG=${3:?}; T=${4:-5}
P=$(cd "$(dirname "$0")/.." && pwd); S=$(dirname $PREFIX)
export PGHOST=/tmp/claude-1000/pgsk PGPORT=54329 PGUSER=postgres PGDATABASE=postgres
B=$(mktemp -d /tmp/claude-1000/rbi_rerun.XXXX)
cp -r $P/src $P/Makefile $P/roaring_index.control $P/roaring_index--0.1.sql $P/roaring_index_citext.control $P/roaring_index_citext--0.1.sql $B/; rm -f $B/src/*.o $B/src/*.bc $B/*.so; mkdir -p $B/test/sql $B/test/isolation
( cd $B && make -s PG_CONFIG=$PREFIX/bin/pg_config && make -s PG_CONFIG=$PREFIX/bin/pg_config install ) || { echo "build failed"; exit 1; }
$PREFIX/bin/pg_ctl -D $DATA -l $S/bench_pg.log start >/dev/null 2>&1 || true; sleep 2
$PREFIX/bin/psql -X -q -c "select count(*) from pg_class where relname like 'fact_%_roaring'" | head -3
SCRATCH=$S T=$T ONLY="2b 2c" $P/bench/bench.sh > $P/bench/logs/${TAG}_bench_stdout.txt 2>&1
cp $S/bench_results.txt $P/bench/logs/${TAG}_queries.txt; cp $S/bench_plans.log $P/bench/logs/${TAG}_query_plans.log
$PREFIX/bin/pg_ctl -D $DATA stop -m fast >/dev/null 2>&1
echo RERUN_DONE
