#!/bin/bash
# Dev cluster helpers.  Usage: ./dev.sh {start|stop|restart|psql [args]|reset|env}
P=$(cd "$(dirname "$0")" && pwd)
# LION_SOCK / LION_PORT let a worktree run its own cluster next to the main one
export PGHOST=${LION_SOCK:-/tmp/claude-1000/pgsk} PGPORT=${LION_PORT:-54329} PGUSER=postgres PGDATABASE=postgres
export PATH=$P/.local/pg/bin:$PATH
D=$P/.local/data
case "$1" in
  start)   mkdir -p $PGHOST; pg_ctl -D $D -l $P/.local/pg.log start ;;
  stop)    pg_ctl -D $D stop -m fast ;;
  restart) pg_ctl -D $D -l $P/.local/pg.log restart -m fast ;;
  psql)    shift; exec psql -X "$@" ;;
  env)     printf "export PGHOST=%q PGPORT=%q PGUSER=%q PGDATABASE=%q PATH=%q\n" "$PGHOST" "$PGPORT" "$PGUSER" "$PGDATABASE" "$PATH" ;;
  reset)   pg_ctl -D $D stop -m fast >/dev/null 2>&1; rm -rf $D; initdb -D $D -U postgres --no-locale -E UTF8 >/dev/null
           cat >> $D/postgresql.conf <<CONF
port = $PGPORT
unix_socket_directories = '$PGHOST'
listen_addresses = ''
shared_buffers = 2GB
work_mem = 64MB
maintenance_work_mem = 1GB
max_parallel_workers_per_gather = 0
max_parallel_maintenance_workers = 0
fsync = off
synchronous_commit = off
wal_level = minimal
max_wal_senders = 0
autovacuum = off
jit = off
log_min_messages = warning
CONF
           mkdir -p $PGHOST; pg_ctl -D $D -l $P/.local/pg.log start >/dev/null && echo "cluster reset and started on $PGHOST:$PGPORT" ;;
  *) echo "usage: $0 {start|stop|restart|psql|reset|env}"; exit 1 ;;
esac
