#!/usr/bin/env python3
# A warm lionvec scan over random unit vectors, timed before and after the
# integer 8-bit kernel, with recall against the exact ten nearest.
#
#   scanbench.py setup N D            make the table, the index, the queries
#   scanbench.py run LABEL ROUNDS     time the queries, LABEL names the build
#
# The index is built once (by whichever build is installed at setup) and
# shared by every run: the format does not change, so only the kernel does.
import os, re, struct, sys, time, json
import numpy as np
import psycopg

DSN = os.environ.get('SB_DSN', 'host=/var/tmp/lionpg_sock port=54329 user=postgres dbname=postgres')
OUT = os.environ.get('SB_OUT', os.path.dirname(os.path.abspath(__file__)))
NQ = 50
STATS = re.compile(r'lionvec scan of "[^"]*": (\d+) partitions probed, (\d+) passes, '
                   r'(\d+) candidates, (\d+) refined, (\d+) fetched, (\d+) returned')
DISTANCES = ('l2_distance(vector, vector)', 'vector_negative_inner_product(vector, vector)',
             'cosine_distance(vector, vector)')
lit = lambda q: '[' + ','.join('%.8f' % x for x in q) + ']'


def connect():
    conn = psycopg.connect(DSN, autocommit=True, prepare_threshold=None)
    conn.execute("LOAD 'pg_lion'")
    return conn


def unit_rows(rng, n, d):
    x = rng.standard_normal((n, d), dtype=np.float32)
    x /= np.linalg.norm(x, axis=1, keepdims=True)
    return x


def copy_binary(conn, table, ids, X):
    n, d = X.shape
    be = X.astype('>f4')
    with conn.cursor() as cur, cur.copy(f'COPY {table} (id, embedding) FROM STDIN (FORMAT BINARY)') as cp:
        cp.write(b'PGCOPY\n\xff\r\n\x00' + struct.pack('>ii', 0, 0))
        head = struct.pack('>hih', 2, 4, 0)  # 2 fields; id length 4; (id written next)
        vhead = struct.pack('>ihh', 4 + 4 * d, d, 0)
        for i in range(n):
            cp.write(struct.pack('>hi', 2, 4) + struct.pack('>i', int(ids[i])) + vhead + be[i].tobytes())
        cp.write(struct.pack('>h', -1))


def setup(n, d):
    rng = np.random.default_rng(12345)
    X = unit_rows(rng, n, d)
    Q = unit_rows(rng, NQ, d)
    # the exact ten nearest by cosine (unit rows: the largest inner products)
    gt = []
    for j in range(NQ):
        s = X @ Q[j]
        top = np.argpartition(-s, 10)[:10]
        gt.append([int(i) for i in top[np.argsort(-s[top])]])
    np.save(os.path.join(OUT, 'Q.npy'), Q)
    json.dump(gt, open(os.path.join(OUT, 'gt.json'), 'w'))

    conn = connect()
    conn.execute('CREATE EXTENSION IF NOT EXISTS vector')
    conn.execute('CREATE EXTENSION IF NOT EXISTS pg_lion')
    conn.execute('CREATE EXTENSION IF NOT EXISTS pg_prewarm')
    for f in DISTANCES:
        conn.execute(f'ALTER FUNCTION {f} COST 100')
    conn.execute('DROP TABLE IF EXISTS items')
    conn.execute(f'CREATE TABLE items (id int, embedding vector({d}))')
    t = time.perf_counter()
    for lo in range(0, n, 10000):
        copy_binary(conn, 'items', np.arange(lo, min(n, lo + 10000)), X[lo:lo + 10000])
    print(f'loaded {n} rows in {time.perf_counter() - t:.0f} s', flush=True)
    t = time.perf_counter()
    conn.execute('CREATE INDEX items_lv ON items USING lionvec (embedding vector_cosine_ops)')
    print(f'index built in {time.perf_counter() - t:.0f} s', flush=True)
    conn.execute('VACUUM ANALYZE items')
    for (name, size) in conn.execute(
            "SELECT relname, pg_size_pretty(pg_relation_size(oid)) FROM pg_class "
            "WHERE relname IN ('items', 'items_lv') ORDER BY 1"):
        print(f'{name}: {size}')


def canary():
    # the machine's speed at the time, in ms: a fixed matmul, the median of 5
    rng = np.random.default_rng(1)
    A = rng.standard_normal((600, 600), dtype=np.float32)
    ts = []
    for _ in range(5):
        t = time.perf_counter()
        for _ in range(3):
            A = A @ A / 600.0
        ts.append((time.perf_counter() - t) * 1e3)
    return float(np.median(ts))


def run(label, rounds):
    Q = np.load(os.path.join(OUT, 'Q.npy'))
    gt = json.load(open(os.path.join(OUT, 'gt.json')))
    conn = connect()
    for rel in ('items', 'items_lv'):
        conn.execute(f"SELECT pg_prewarm('{rel}')")
    # the TOAST relation too
    for (toast,) in conn.execute("SELECT reltoastrelid::regclass FROM pg_class WHERE relname = 'items' AND reltoastrelid <> 0"):
        conn.execute(f"SELECT pg_prewarm('{toast}')")
    stats = []
    conn.add_notice_handler(lambda d: stats.append(d.message_primary))
    cur = conn.cursor()
    cur.execute('SET client_min_messages = debug1')
    sql = 'SELECT id FROM items ORDER BY embedding <=> %(q)s::vector LIMIT 10'
    plan = cur.execute('EXPLAIN ' + sql, {'q': lit(Q[0])}).fetchall()
    assert any('items_lv' in r[0] for r in plan), plan
    # warm: every query once
    for j in range(NQ):
        cur.execute(sql, {'q': lit(Q[j])}).fetchall()
    times = [[] for _ in range(NQ)]
    recall = [None] * NQ
    work = [None] * NQ
    for r in range(rounds):
        for j in range(NQ):
            stats.clear()
            t = time.perf_counter()
            ids = [x[0] for x in cur.execute(sql, {'q': lit(Q[j])}).fetchall()]
            times[j].append((time.perf_counter() - t) * 1e3)
            rec = len(set(ids) & set(gt[j])) / 10
            m = [STATS.match(s) for s in stats]
            m = [x for x in m if x]
            w = tuple(int(v) for v in m[-1].groups()) if m else None
            if recall[j] is None:
                recall[j], work[j] = rec, w
            else:
                assert recall[j] == rec and work[j] == w, (j, recall[j], rec, work[j], w)
    med = [float(np.median(t)) for t in times]
    res = {'label': label, 'rounds': rounds, 'canary_ms': canary(), 'median_ms': med, 'recall': recall, 'work': work,
           'median_of_medians_ms': float(np.median(med)), 'mean_of_medians_ms': float(np.mean(med)),
           'mean_recall': float(np.mean(recall)),
           'mean_probed': float(np.mean([w[0] for w in work])),
           'mean_refined': float(np.mean([w[3] for w in work]))}
    print(f"{label}: median of the queries' medians {res['median_of_medians_ms']:.2f} ms, mean {res['mean_of_medians_ms']:.2f}; "
          f"recall {res['mean_recall']:.4f}; probed {res['mean_probed']:.1f}, refined {res['mean_refined']:.0f}; canary {res['canary_ms']:.1f} ms", flush=True)
    with open(os.path.join(OUT, f'run-{label}-{int(time.time())}.json'), 'w') as f:
        json.dump(res, f)


if __name__ == '__main__':
    if sys.argv[1] == 'setup':
        setup(int(sys.argv[2]), int(sys.argv[3]))
    elif sys.argv[1] == 'run':
        run(sys.argv[2], int(sys.argv[3]))
