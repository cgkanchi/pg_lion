#!/bin/bash
# Dev cluster helpers.  Usage: ./dev.sh {start|stop|restart|psql [args]|reset|env}
P=$(cd "$(dirname "$0")" && pwd)
# LION_SOCK / LION_PORT let a worktree run its own cluster next to the main one
# (CI sets both).  The default socket directory is per user - in
# $XDG_RUNTIME_DIR, which is private to the user, where there is one - rather
# than a fixed path in a shared /tmp, and short, because a Unix-domain socket
# path is limited to ~107 bytes, which a directory inside a deep checkout could
# exceed.
export PGHOST=${LION_SOCK:-${XDG_RUNTIME_DIR:-/tmp}/pg_lion-$(id -un)} PGPORT=${LION_PORT:-54329} PGUSER=postgres PGDATABASE=postgres
export PATH=$P/.local/pg/bin:$PATH
D=$P/.local/data
case "$1" in
  start)   mkdir -p "$PGHOST"; pg_ctl -D "$D" -l "$P/.local/pg.log" start ;;
  stop)    pg_ctl -D "$D" stop -m fast ;;
  restart) mkdir -p "$PGHOST"; pg_ctl -D "$D" -l "$P/.local/pg.log" restart -m fast ;;
  psql)    shift; exec psql -X "$@" ;;
  env)     printf "export PGHOST=%q PGPORT=%q PGUSER=%q PGDATABASE=%q PATH=%q\n" "$PGHOST" "$PGPORT" "$PGUSER" "$PGDATABASE" "$PATH" ;;
  # wal_level = replica, not minimal.  Under minimal a relation whose file was
  # created in the running transaction is not WAL-logged: every CREATE INDEX
  # and REINDEX, and every insert or split into an index the same transaction
  # created.  The build's bulk writer then only registers its pages for sync
  # (core logs or syncs the whole file at commit), and the extension's own
  # records are skipped, so the regression suite never ran the build's
  # WAL-logging half, nor those write paths' records, outside test/recovery.
  # replica logs everything.  Measured on an assert build: about 13% more
  # time over basic, insert, large, vacuum, pushdown and verify, most of it in
  # insert and large; fsync stays off either way.  minimal also forced
  # max_wal_senders = 0; replica does not, and it stays 0 because nothing
  # replicates from this cluster.
  reset)   pg_ctl -D "$D" stop -m fast >/dev/null 2>&1; rm -rf "$D"; initdb -D "$D" -U postgres --no-locale -E UTF8 >/dev/null
           cat >> "$D/postgresql.conf" <<CONF
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
wal_level = replica
max_wal_senders = 0
autovacuum = off
jit = off
log_min_messages = warning
CONF
           mkdir -p "$PGHOST"; pg_ctl -D "$D" -l "$P/.local/pg.log" start >/dev/null && echo "cluster reset and started on $PGHOST:$PGPORT" ;;
  *) echo "usage: $0 {start|stop|restart|psql|reset|env}"; exit 1 ;;
esac
