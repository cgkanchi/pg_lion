# Sourced by the other scripts here: where everything lives.  Each can be set
# in the environment first.
LOCAL=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO=$(cd "$LOCAL/../../.." && pwd)
BENCH_DIR=${BENCH_DIR:-$HOME/lionbench}         # shards, cache, cluster, logs: about 60 GB at 1M rows
PG_MAJOR=${PG_MAJOR:-18}
PG_BIN=${PG_BIN:-/usr/lib/postgresql/$PG_MAJOR/bin}
NSHARDS=${NSHARDS:-26}                          # 26 = the whole 1M rows; 38,462 rows a shard
BENCH_PORT=${BENCH_PORT:-55440}
PGDATA_DIR=$BENCH_DIR/pgdata
SOCK=$BENCH_DIR/sock
VENV=$BENCH_DIR/venv
export PATH=$PG_BIN:$PATH PGHOST=$SOCK PGPORT=$BENCH_PORT PGUSER=postgres PGDATABASE=postgres
