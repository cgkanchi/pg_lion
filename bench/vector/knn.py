#!/usr/bin/env python3
# lionvec against pgvector's HNSW on 975,000 OpenAI embeddings (1536 dimensions,
# cosine), with and without a filter (DESIGN.md §44.9).
#
# The table is items(id int, u float8, embedding vector(1536)), u uniform in
# [0, 1) so that `u < sel` keeps a share sel of the rows; Q.npy holds the
# queries (unit vectors) and S.npy their exact cosine similarity to every row,
# the ground truth.  The HNSW index is pgvector's on embedding::halfvec(1536)
# (m = 16, ef_construction = 64), the lionvec one on embedding itself, the
# filter's a lion index on u.
#
# pgvector's distance functions are costed at 100, as docs/REFERENCE.md asks
# of a lionvec installation (the planner otherwise sorts a filter's rows by
# distance where the scan is faster).
#
#   knn.py setup              build the lionvec and lion indexes
#   knn.py warm NQ            recall and latency, warm, every method and filter
#   knn.py cold NQ            latency of each query just after a restart
#   knn.py capped MB NQ       latency and disk reads of a stream of queries
#                             with PostgreSQL held to MB of memory
import json, os, re, subprocess, sys, time
import numpy as np
import psycopg
# warm needs the pg_prewarm extension in the database

DP = os.environ.get('KNN_DATA', '/home/claude/vtoy/data/')
DSN = os.environ.get('KNN_DSN', 'dbname=vtoy')
CLUSTER = os.environ.get('KNN_CLUSTER', '16 main')

lit = lambda q: '[' + ','.join('%.8f' % x for x in q) + ']'
HNSW = ("SELECT id FROM items {w} ORDER BY embedding::halfvec(1536) <=> %(q)s::halfvec(1536) "
        "LIMIT 10")
LION = "SELECT id FROM items {w} ORDER BY embedding <=> %(q)s::vector LIMIT 10"
STATS = re.compile(r'lionvec scan of "[^"]*": (\d+) passes, (\d+) candidates, (\d+) refined, '
                   r'(\d+) fetched, (\d+) returned, (\d+) skipped out of order, '
                   r'(\d+) bounds missed, (\d+) pages read')


DISTANCES = ('l2_distance(vector, vector)', 'vector_negative_inner_product(vector, vector)',
             'cosine_distance(vector, vector)')


def connect():
    conn = psycopg.connect(DSN, autocommit=True, prepare_threshold=None)
    conn.execute("LOAD 'pg_lion'")
    return conn


def cost_distances(conn):
    for f in DISTANCES:
        conn.execute(f'ALTER FUNCTION {f} COST 100')


def setup():
    conn = connect()
    conn.execute('CREATE EXTENSION IF NOT EXISTS pg_lion')
    conn.execute('CREATE EXTENSION IF NOT EXISTS pg_lion_vector')
    cost_distances(conn)
    for name, ddl in [('items_lv', 'CREATE INDEX items_lv ON items USING lionvec (embedding vector_cosine_ops)'),
                      ('items_ul', 'CREATE INDEX items_ul ON items USING lion (u)')]:
        if conn.execute('SELECT to_regclass(%s)', (name,)).fetchone()[0] is None:
            t = time.perf_counter()
            conn.execute(ddl)
            print(f'{name}: built in {time.perf_counter() - t:.0f} s', flush=True)
    conn.execute('VACUUM ANALYZE items')
    for (name, size) in conn.execute(
            "SELECT relname, pg_size_pretty(pg_relation_size(oid)) FROM pg_class "
            "WHERE relname IN ('items', 'items_hnsw', 'items_lv', 'items_ul') ORDER BY 1"):
        print(f'{name}: {size}')


def where(sel):
    return '' if sel >= 1.0 else f'WHERE u < {sel}'


def truth(S, u, sels, nq):
    # the exact ten nearest of each query under each filter, worked out before
    # anything is timed: S is read a block of rows at a time, not a column of
    # every row per query, which would push the database out of the page cache
    best = {sel: [[] for _ in range(nq)] for sel in sels}
    for lo in range(0, S.shape[0], 50000):
        blk = np.asarray(S[lo:lo + 50000, :nq])
        for sel in sels:
            rows = np.flatnonzero(u[lo:lo + 50000] < sel)
            for i in range(nq):
                if len(rows) == 0:
                    continue
                top = rows[np.argsort(-blk[rows, i])[:10]]
                best[sel][i].extend((float(blk[r, i]), lo + int(r)) for r in top)
    return {sel: [[r for _, r in sorted(b, reverse=True)[:10]] for b in best[sel]] for sel in sels}


def run(conn, name, sql, sel, settings, Q, gt, nq):
    stats = []
    handler = lambda d: stats.append(d.message_primary)
    conn.add_notice_handler(handler)
    cur = conn.cursor()
    cur.execute('RESET ALL')
    for s in settings:
        cur.execute(s)
    cur.execute('SET client_min_messages = debug1')
    rec, ts, nret, scans = [], [], [], []
    for i in range(3):                           # warm the plan and the session
        cur.execute(sql, {'q': lit(Q[150 + i])})
        cur.fetchall()
    for i in range(nq):
        stats.clear()
        t = time.perf_counter()
        cur.execute(sql, {'q': lit(Q[i])})
        ids = [r[0] for r in cur.fetchall()]
        ts.append((time.perf_counter() - t) * 1e3)
        rec.append(len(set(ids) & set(gt[i])) / 10)
        nret.append(len(ids))
        for m in stats:
            g = STATS.search(m)
            if g:
                scans.append([int(x) for x in g.groups()])
    cur.execute('SET client_min_messages = warning')
    cur.execute('EXPLAIN ' + sql, {'q': lit(Q[0])})
    plan = ' / '.join(r[0].strip() for r in cur.fetchall()
                      if re.search('Scan|Sort|Lion', r[0]))[:200]
    row = dict(name=name, sel=sel, recall=float(np.mean(rec)), exact_q=float(np.mean(np.array(rec) == 1)),
               returned=float(np.mean(nret)), median_ms=float(np.median(ts)),
               p95_ms=float(np.percentile(ts, 95)), plan=re.sub(r"'\[[^]]*\]'", "'...'", plan))
    if scans:
        a = np.array(scans)
        row.update(passes=float(a[:, 0].mean()), candidates=float(a[:, 1].mean()),
                   refined=float(a[:, 2].mean()), fetched=float(a[:, 3].mean()),
                   skipped=float(a[:, 5].mean()), missed=float(a[:, 6].mean()),
                   pages=float(a[:, 7].mean()))
    conn.remove_notice_handler(handler)
    print(json.dumps(row), flush=True)
    return row


def methods(sel):
    m = []
    for eps in (3, 4):
        m.append((f'lionvec eps={eps}', LION.format(w=where(sel)), [f'SET pg_lion.vector_epsilon = {eps}']))
    for ef in (40, 100):
        m.append((f'hnsw ef={ef}', HNSW.format(w=where(sel)), [f'SET hnsw.ef_search = {ef}']))
        if sel < 1.0:
            m.append((f'hnsw ef={ef} iterative', HNSW.format(w=where(sel)),
                      [f'SET hnsw.ef_search = {ef}', 'SET hnsw.iterative_scan = relaxed_order']))
    return m


def warm(nq):
    sels = (1.0, 0.1, 0.01, 0.001)
    Q = np.load(DP + 'Q.npy')
    gt = truth(np.load(DP + 'S.npy', mmap_mode='r'), np.load(DP + 'filter_u.npy'), sels, nq)
    conn = connect()
    cost_distances(conn)
    # warm means everything a query reads is in memory: the HNSW graph and
    # the heap in shared_buffers, and the lionvec index and the vectors (TOAST),
    # which do not fit beside them, in the page cache
    for sql in ["SELECT pg_prewarm('items_hnsw')", "SELECT pg_prewarm('items')",
                "SELECT pg_prewarm('items_ul')", "SELECT pg_prewarm('items_lv', 'read')",
                "SELECT pg_prewarm(reltoastrelid, 'read') FROM pg_class WHERE relname = 'items'",
                "SELECT pg_prewarm(indexrelid, 'read') FROM pg_index WHERE indrelid = "
                "(SELECT reltoastrelid FROM pg_class WHERE relname = 'items')"]:
        t = time.perf_counter()
        conn.execute(sql)
        print(f'{sql}: {time.perf_counter() - t:.0f} s', flush=True)
    out = []
    for sel in sels:
        for name, sql, settings in methods(sel):
            out.append(run(conn, name, sql, sel, settings, Q, gt[sel], nq))
    json.dump(out, open(os.environ.get('KNN_OUT', 'knn_warm.json'), 'w'), indent=1)


def cold(nq):
    # each query alone after a restart and, where the kernel lets us, with
    # the page cache dropped: what a query costs when nothing is in memory
    Q = np.load(DP + 'Q.npy')
    conn = connect()
    cost_distances(conn)
    conn.close()
    out = []
    for name, sql, settings in [m for m in methods(1.0) if m[0] in ('lionvec eps=4', 'hnsw ef=40')] + \
            [m for sel in (0.01, 0.001) for m in methods(sel)
             if m[0] in ('lionvec eps=4', 'hnsw ef=40 iterative')]:
        ts = []
        for i in range(nq):
            subprocess.run(['pg_ctlcluster'] + CLUSTER.split() + ['restart'], check=True)
            dropped = subprocess.run(['sh', '-c', 'sync; echo 3 > /proc/sys/vm/drop_caches'],
                                     capture_output=True).returncode == 0
            conn = connect()
            for s in settings:
                conn.execute(s)
            t = time.perf_counter()
            conn.execute(sql, {'q': lit(Q[i])}).fetchall()
            ts.append((time.perf_counter() - t) * 1e3)
            conn.close()
        row = dict(name=name, sql=sql[:60], settings=settings, cache_dropped=dropped, median_ms=float(np.median(ts)),
                   max_ms=float(np.max(ts)), all_ms=[round(x, 1) for x in ts])
        print(json.dumps(row), flush=True)
        out.append(row)
    json.dump(out, open(os.environ.get('KNN_OUT', 'knn_cold.json'), 'w'), indent=1)


def read_bytes(pid):
    for line in open(f'/proc/{pid}/io'):
        if line.startswith('read_bytes:'):
            return int(line.split()[1])


def capped(mb, nq):
    # a steady stream of queries where memory holds lionvec's CODE pages but
    # not the HNSW graph: PostgreSQL started inside a memory cgroup of mb MB
    # (its processes, shared_buffers of 128 MB, and the page cache its reads
    # fill), the page cache dropped first so that every page is charged to
    # it.  Each method after its own restart, run with rows of the table as
    # queries until the cgroup is full and 100 more, then timed on the test
    # queries; the disk reads are the backend's read_bytes.  KNN_CGROUP is a
    # memory cgroup directory to use (v1 or v2); run as root.
    cg = os.environ['KNN_CGROUP']
    limit = os.path.join(cg, 'memory.max')
    usage = os.path.join(cg, 'memory.current')
    if not os.path.exists(limit):
        limit = os.path.join(cg, 'memory.limit_in_bytes')
        usage = os.path.join(cg, 'memory.usage_in_bytes')
    Q = np.load(DP + 'Q.npy')
    ctl = ['pg_ctlcluster'] + CLUSTER.split()
    conn = connect()
    cost_distances(conn)
    pgdata = conn.execute('SHOW data_directory').fetchone()[0]
    W = [r[0] for r in conn.execute('SELECT embedding::text FROM items WHERE id % 191 = 7 ORDER BY id LIMIT 5000')]
    conn.close()
    out = []
    try:
        for name, sql, settings in [m for m in methods(1.0) if m[0] in ('lionvec eps=4', 'hnsw ef=40')] + \
                [m for m in methods(0.01) if m[0] in ('lionvec eps=4', 'hnsw ef=40 iterative')]:
            subprocess.run(ctl + ['stop'], check=True)
            subprocess.run(['sh', '-c', 'sync; echo 3 > /proc/sys/vm/drop_caches'], check=True)
            with open(limit, 'w') as f:
                f.write(str(mb << 20))
            subprocess.run(['sh', '-c', f'echo $$ > {cg}/cgroup.procs && exec ' + ' '.join(ctl) +
                            ' start -- -o "-c shared_buffers=128MB"'], check=True)
            pm = int(open(os.path.join(pgdata, 'postmaster.pid')).readline())
            paths = [line.rstrip('\n').split(':', 2)[2] for line in open(f'/proc/{pm}/cgroup')]
            if not any(p != '/' and cg.rstrip('/').endswith(p) for p in paths):
                raise RuntimeError(f'the postmaster is not in {cg}')
            conn = connect()
            for st in settings:
                conn.execute(st)
            pid = conn.execute('SELECT pg_backend_pid()').fetchone()[0]
            n = 0
            while n < len(W) - 100 and int(open(usage).read()) < 0.95 * (mb << 20):
                conn.execute(sql, {'q': W[n]}).fetchall()
                n += 1
            full = int(open(usage).read()) >= 0.95 * (mb << 20)
            for q in W[n:n + 100]:
                conn.execute(sql, {'q': q}).fetchall()
            ts, rd = [], []
            for i in range(nq):
                r = read_bytes(pid)
                t = time.perf_counter()
                conn.execute(sql, {'q': lit(Q[i])}).fetchall()
                ts.append((time.perf_counter() - t) * 1e3)
                rd.append((read_bytes(pid) - r) / 8192)
            conn.close()
            row = dict(name=name, sql=sql[:60], settings=settings, memory_mb=mb, warmup=n + 100, filled=full,
                       median_ms=float(np.median(ts)),
                       p95_ms=float(np.percentile(ts, 95)), median_pages_read=float(np.median(rd)),
                       mean_pages_read=float(np.mean(rd)))
            print(json.dumps(row), flush=True)
            out.append(row)
    finally:
        subprocess.run(ctl + ['stop'], check=False)
        with open(limit, 'w') as f:
            f.write('max' if limit.endswith('memory.max') else '-1')
        subprocess.run(ctl + ['start'], check=True)
    json.dump(out, open(os.environ.get('KNN_OUT', f'knn_capped_{mb}.json'), 'w'), indent=1)


if __name__ == '__main__':
    {'setup': lambda: setup(), 'warm': lambda: warm(int(sys.argv[2])),
     'cold': lambda: cold(int(sys.argv[2])),
     'capped': lambda: capped(int(sys.argv[2]), int(sys.argv[3]))}[sys.argv[1]]()
