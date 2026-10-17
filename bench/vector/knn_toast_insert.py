#!/usr/bin/env python3
# What keeping a TOAST locator costs an insert (DESIGN.md §44.13, "Costs"):
# a bulk INSERT ... SELECT of KNN_INS_N rows of knn_dbpedia.py's table, loaded
# with its vectors in TOAST (KNN_INLINE=0, database dbpedia_toast), into a
# table of KNN_INS_BASE of its rows under a lionvec index built by the library
# being measured - one that keeps no locator (an earlier pg_lion, format 4)
# against one that keeps one (format 5) - with, where given,
# pg_lion.vector_toast_direct set (which an insert does not read).
#
# Each repetition loads the table and builds its index anew (serially, at
# maintenance_work_mem 1 GB), VACUUM ANALYZEs and checkpoints, then times the
# INSERT and the WAL it wrote, and records the build's time, the meta page's
# version and flags, and the index's size.  The configurations take turns,
# reversed every other repetition; a change of library restarts the server,
# after which the source table and its TOAST relation are read into shared
# buffers.
#
#   knn_toast_insert.py OUT.json
#
# KNN_INS_CONFIGS (default main:main,on:branch:on,off:branch:off):
# label:library[:direct], each library KNN_RA_SO_DIR/LIBRARY.so (default
# .local/so) copied over KNN_RA_INSTALL (default pg_config's
# pkglibdir/pg_lion.so) before KNN_RESTART (default "./dev.sh restart");
# KNN_INS_REPS (5), KNN_INS_BASE (20000), KNN_INS_N (5000); KNN_DSN (default
# dbname=dbpedia_toast).  The server needs pg_prewarm, and pageinspect in the
# database for the meta page.
import json, os, statistics, subprocess, sys, time
import psycopg

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
SO_DIR = os.environ.get('KNN_RA_SO_DIR', os.path.join(REPO, '.local', 'so'))
INSTALL = os.environ.get('KNN_RA_INSTALL') or os.path.join(
    subprocess.run(['pg_config', '--pkglibdir'], capture_output=True, text=True,
                   check=True).stdout.strip(), 'pg_lion.so')
RESTART = os.environ.get('KNN_RESTART', './dev.sh restart')
DSN = os.environ.get('KNN_DSN', 'dbname=dbpedia_toast')
REPS = int(os.environ.get('KNN_INS_REPS', '5'))
BASE = int(os.environ.get('KNN_INS_BASE', '20000'))
N = int(os.environ.get('KNN_INS_N', '5000'))
CONFIGS = [c.split(':') for c in
           os.environ.get('KNN_INS_CONFIGS', 'main:main,on:branch:on,off:branch:off').split(',')]
_lib = [None]


def connect():
    for _ in range(100):
        try:
            c = psycopg.connect(DSN, autocommit=True)
            c.execute("LOAD 'pg_lion'")
            return c
        except psycopg.OperationalError:
            time.sleep(0.2)
    raise SystemExit('the server did not come back')


def use(lib):
    # the library in place and the server restarted, the source table and
    # its TOAST relation and that one's index read into shared buffers
    if _lib[0] == lib:
        return
    subprocess.run(['cp', '-p', os.path.join(SO_DIR, lib + '.so'), INSTALL + '.new'], check=True)
    os.replace(INSTALL + '.new', INSTALL)
    subprocess.run(RESTART, shell=True, check=True, capture_output=True)
    _lib[0] = lib
    c = connect()
    toast = "(SELECT reltoastrelid FROM pg_class WHERE relname = 'dbpedia')"
    c.execute("SELECT pg_prewarm('dbpedia')")
    c.execute(f"SELECT pg_prewarm({toast})")
    c.execute(f"SELECT pg_prewarm(indexrelid) FROM pg_index WHERE indrelid = {toast}")
    c.close()


def rep(cfg):
    label, lib = cfg[0], cfg[1]
    use(lib)
    c = connect()
    c.execute('DROP TABLE IF EXISTS ins')
    c.execute('CREATE TABLE ins (id int, u float8, embedding vector(1536)) WITH (autovacuum_enabled = off)')
    c.execute(f'INSERT INTO ins SELECT id, u, embedding FROM dbpedia WHERE id <= {BASE}')
    c.execute("SET maintenance_work_mem = '1GB'")
    c.execute('SET max_parallel_maintenance_workers = 0')
    t = time.perf_counter()
    c.execute('CREATE INDEX ins_lv ON ins USING lionvec (embedding vector_cosine_ops)')
    build_s = time.perf_counter() - t
    # the meta page's version (uint16 at 28) and flags (at 34)
    meta = c.execute("SELECT get_byte(p, 28) | (get_byte(p, 29) << 8), get_byte(p, 34) "
                     "FROM (SELECT get_raw_page('ins_lv', 0) p) s").fetchone()
    c.execute('VACUUM ANALYZE ins')
    c.execute('CHECKPOINT')
    if len(cfg) > 2:
        c.execute(f'SET pg_lion.vector_toast_direct = {cfg[2]}')
    lsn0 = c.execute('SELECT pg_current_wal_insert_lsn()').fetchone()[0]
    t = time.perf_counter()
    c.execute(f'INSERT INTO ins SELECT id, u, embedding FROM dbpedia WHERE id > {BASE} AND id <= {BASE + N}')
    ms = (time.perf_counter() - t) * 1000
    wal = c.execute('SELECT pg_wal_lsn_diff(pg_current_wal_insert_lsn(), %s)', (lsn0,)).fetchone()[0]
    size = c.execute("SELECT pg_relation_size('ins_lv')").fetchone()[0]
    c.close()
    r = dict(config=label, lib=lib, direct=cfg[2] if len(cfg) > 2 else None, ms=round(ms, 1),
             wal_bytes=int(wal), build_s=round(build_s, 2), version=meta[0], flags=meta[1],
             index_bytes=int(size))
    print(json.dumps(r), flush=True)
    return r


def main(out):
    rows = []
    for i in range(REPS):
        order = CONFIGS if i % 2 == 0 else CONFIGS[::-1]
        for cfg in order:
            rows.append(dict(rep(cfg), rep=i))
    summary = {}
    for cfg in CONFIGS:
        mine = [r for r in rows if r['config'] == cfg[0]]
        summary[cfg[0]] = dict(ms_median=statistics.median(r['ms'] for r in mine),
                               ms_all=[r['ms'] for r in mine],
                               wal_median=statistics.median(r['wal_bytes'] for r in mine),
                               build_s_median=statistics.median(r['build_s'] for r in mine))
    print(json.dumps(summary, indent=1))
    json.dump(dict(base=BASE, n=N, reps=REPS, configs=CONFIGS, summary=summary, rows=rows,
                   run_at=time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime())),
              open(out, 'w'), indent=1)


if __name__ == '__main__':
    main(sys.argv[1])
