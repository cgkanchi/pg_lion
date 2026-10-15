# Sourced by the other scripts here: where everything lives.  Each can be set
# in the environment first.
LOCAL=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO=$(cd "$LOCAL/../../.." && pwd)
BENCH_DIR=${BENCH_DIR:-$HOME/lionbench}         # shards, cache, cluster, logs: about 60 GB at 1M rows
PG_MAJOR=${PG_MAJOR:-18}
# The PostgreSQL to use: PG_CONFIG's (an install of your own, from source or
# apt), else PG_BIN, else apt.postgresql.org's layout, else the pg_config on
# PATH if it is $PG_MAJOR.  setup.sh installs PostgreSQL only when none is found.
if [ -n "${PG_CONFIG:-}" ]; then
	PG_BIN=$("$PG_CONFIG" --bindir)
elif [ -z "${PG_BIN:-}" ]; then
	PG_BIN=/usr/lib/postgresql/$PG_MAJOR/bin
	if [ ! -x "$PG_BIN/postgres" ] && command -v pg_config >/dev/null &&
	   pg_config --version | grep -q "PostgreSQL $PG_MAJOR\." &&
	   [ -x "$(pg_config --bindir)/postgres" ]; then
		PG_BIN=$(pg_config --bindir)
	fi
fi
NSHARDS=${NSHARDS:-26}                          # 26 = the whole 1M rows; 38,462 rows a shard
BENCH_PORT=${BENCH_PORT:-55440}
PGDATA_DIR=$BENCH_DIR/pgdata
SOCK=$BENCH_DIR/sock
VENV=$BENCH_DIR/venv
export PATH=$PG_BIN:$PATH PGHOST=$SOCK PGPORT=$BENCH_PORT PGUSER=postgres PGDATABASE=postgres
