#!/bin/bash
# The benchmark's own PostgreSQL cluster, in $BENCH_DIR/pgdata (not dev.sh's
# .local/data, so a dev cluster is left alone).
#   pg.sh init      initdb and the tuned settings (sized to this machine's RAM)
#   pg.sh start | stop | restart | psql [args]
set -euo pipefail
. "$(dirname "$0")/env.sh"
LOG=$BENCH_DIR/pg.log

case "${1:-}" in
  init)
	if [ -e "$PGDATA_DIR/PG_VERSION" ]; then echo "$PGDATA_DIR exists; pg.sh init leaves it alone"; exit 0; fi
	mkdir -p -m 0700 "$SOCK"
	initdb -D "$PGDATA_DIR" -U postgres --no-locale -E UTF8 >/dev/null
	# Memory: shared_buffers a quarter of RAM up to 8 GB; maintenance_work_mem
	# 35% up to 8 GB (HNSW's build keeps its whole graph there to be fast:
	# about 3.5 GB at 1M halfvec rows).  The rest are the project's standing
	# benchmark settings (bench/results/2026-10-15-dbpedia-pg18/README.md).
	mem_mb=$(awk '/MemTotal/ {print int($2 / 1024)}' /proc/meminfo)
	sb=$(( mem_mb / 4 )); [ $sb -gt 8192 ] && sb=8192
	mwm=$(( mem_mb * 35 / 100 )); [ $mwm -gt 8192 ] && mwm=8192
	{
	  echo "# lionbench (bench/vector/local/pg.sh init, ${mem_mb} MB of RAM)"
	  echo "port = $BENCH_PORT"
	  echo "unix_socket_directories = '$SOCK'"
	  echo "listen_addresses = ''"
	  echo "shared_buffers = ${sb}MB"
	  echo "maintenance_work_mem = ${mwm}MB"
	  echo "work_mem = 64MB"
	  echo "effective_cache_size = 10GB"
	  echo "random_page_cost = 1.1"
	  echo "effective_io_concurrency = 200"
	  echo "maintenance_io_concurrency = 200"
	  echo "max_parallel_workers_per_gather = 0"
	  echo "max_parallel_maintenance_workers = 0"
	  echo "jit = off"
	  echo "fsync = off"
	  echo "synchronous_commit = off"
	  echo "full_page_writes = off"
	  echo "wal_level = replica"
	  echo "max_wal_senders = 0"
	  echo "max_wal_size = 4GB"
	  echo "autovacuum = off"
	  echo "log_min_messages = warning"
	  if [ "$("$PG_BIN/pg_config" --version | grep -o '[0-9]*' | head -1)" -ge 18 ]; then
	    echo "io_method = worker"
	  fi
	  if [ -e "$("$PG_BIN/pg_config" --pkglibdir)/vchord.so" ]; then
	    echo "shared_preload_libraries = 'vchord'"
	  fi
	} >> "$PGDATA_DIR/postgresql.conf"
	pg_ctl -D "$PGDATA_DIR" -l "$LOG" start >/dev/null
	echo "cluster in $PGDATA_DIR on $SOCK:$BENCH_PORT; shared_buffers ${sb}MB, maintenance_work_mem ${mwm}MB"
	;;
  start)   mkdir -p -m 0700 "$SOCK"; pg_ctl -D "$PGDATA_DIR" -l "$LOG" start ;;
  stop)    pg_ctl -D "$PGDATA_DIR" stop -m fast ;;
  restart) pg_ctl -D "$PGDATA_DIR" -l "$LOG" restart -m fast ;;
  status)  pg_ctl -D "$PGDATA_DIR" status ;;
  psql)    shift; exec psql -X "$@" ;;
  *) echo "usage: $0 {init|start|stop|restart|status|psql [args]}"; exit 1 ;;
esac
