# MS MARCO v2.1 (TREC-RAG 2024) passages embedded with Cohere Embed English
# v3 - CohereLabs/msmarco-v2.1-embed-english-v3, 113,520,750 passages of
# 1024 dimensions stored as float16 - for knn_dbpedia.py's builds, warm and
# cold runs and tables at tens of millions of rows. This script does the two
# steps that are the data set's own, prepare and load, and leaves the cache
# as knn_dbpedia.py expects it: run that with KNN_TABLE=msmarco,
# KNN_DSN=dbname=msmarco and the same KNN_CACHE for everything after.
#
# The table is the first KNN_ROWS passages, in the order of passages_npy's
# files (the order the data set's top1k_offsets count in), as
# msmarco (id int primary key, u float8, embedding halfvec(1024)): halfvec
# holds the float16 values exactly. u is uniform on [0, 1) from a fixed seed,
# the filter column, as in knn_dbpedia.py.
#
# The queries are the data set's own (TREC Deep Learning 2021-2023), not held
# out rows: KNN_NQ of its 1,677, drawn with a fixed seed, rounded to float16
# as the query's cast to halfvec(1024) rounds them. X.npy in the cache holds
# those queries alone and qidx.npy is 0..NQ-1, which is all the warm and cold
# runs read of it.
#
# The truth is the exact ten nearest by cosine under each filter, as
# knn_dbpedia.py's (ties by id): a float32 pass over the rows keeps each
# query's 100 best under each filter, which are then scored again in float64.
# Unfiltered, it is checked against the data set's top1k_offsets cut to the
# rows the table holds.
#
#   knn_msmarco.py prepare      the queries, u, the truth, into KNN_CACHE
#   knn_msmarco.py load         the table, binary COPY by KNN_LOAD_WORKERS
#                               processes, and the lion index on u
#
# KNN_MSMARCO_NPY is the folder of passages_npy's files.
import glob, json, os, subprocess, sys, time
import multiprocessing as mp
import numpy as np
import pyarrow.parquet as pq
import psycopg
import psycopg.conninfo

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import knn_dbpedia as k

NPY = os.environ.get('KNN_MSMARCO_NPY', '/mnt/d/lionbench-msmarco/npy')
QUERIES = os.environ.get('KNN_MSMARCO_QUERIES', '/mnt/d/lionbench-msmarco/queries/queries.parquet')
ROWS = int(os.environ.get('KNN_ROWS', '50000000'))
WORKERS = int(os.environ.get('KNN_LOAD_WORKERS', '8'))
DIM = 1024
POOL = 100                  # each query's candidates a filter, from the float32 pass
CHUNK = 250000


def parts():
    # (file, first id, rows) for the first ROWS rows, the files in name order
    out, at = [], 0
    for f in sorted(glob.glob(os.path.join(NPY, 'msmarco_v2.1_doc_segmented_*.npy'))):
        n = np.load(f, mmap_mode='r').shape[0]
        take = min(n, ROWS - at)
        out.append((f, at, take))
        at += take
        if at == ROWS:
            return out
    raise SystemExit(f'{NPY} holds {at} rows, fewer than KNN_ROWS = {ROWS}')


def chunks(ps, lo=0, hi=None):
    # (first id, float16 rows) of ids lo..hi, CHUNK at a time
    hi = ROWS if hi is None else hi
    for f, first, n in ps:
        a, b = max(lo, first), min(hi, first + n)
        if a >= b:
            continue
        m = np.load(f, mmap_mode='r')
        if m.dtype != np.float16 or m.shape[1] != DIM:
            raise SystemExit(f'{f}: {m.dtype} {m.shape}, not float16 by {DIM}')
        for s in range(a, b, CHUNK):
            e = min(b, s + CHUNK)
            yield s, np.asarray(m[s - first:e - first])


def prepare():
    os.makedirs(k.CACHE, exist_ok=True)
    t = pq.read_table(QUERIES, columns=['_id', 'emb', 'top1k_offsets'])
    pick = np.sort(np.random.default_rng(0).choice(t.num_rows, k.NQ, replace=False))
    emb = t.column('emb').combine_chunks()
    allq = emb.flatten().to_numpy().reshape(t.num_rows, -1)
    if allq.shape[1] != DIM:
        raise SystemExit(f'queries of {allq.shape[1]} dimensions')
    Q = allq[pick].astype(np.float16)
    np.save(os.path.join(k.CACHE, 'X.npy'), Q)
    np.save(os.path.join(k.CACHE, 'qidx.npy'), np.arange(k.NQ))
    u = np.random.default_rng(42).random(ROWS)
    np.save(os.path.join(k.CACHE, 'u.npy'), u)
    ps = parts()

    Qn = Q.astype(np.float32)
    Qn /= np.linalg.norm(Qn, axis=1, keepdims=True)
    pool = {s: (np.full((POOL, k.NQ), -np.inf, np.float32), np.zeros((POOL, k.NQ), np.int64)) for s in k.SELS}
    t0 = time.perf_counter()
    for lo, A in chunks(ps):
        A = A.astype(np.float32)
        nrm = np.linalg.norm(A, axis=1)
        S = (A @ Qn.T) / np.where(nrm > 0, nrm, np.inf)[:, None]
        ids = np.arange(lo, lo + len(A))
        for sel in k.SELS:
            m = u[lo:lo + len(A)] < sel
            ps_s, pi = pool[sel]
            cs = np.concatenate([ps_s, S[m]])
            ci = np.concatenate([pi, np.broadcast_to(ids[m][:, None], (int(m.sum()), k.NQ))])
            top = np.argpartition(-cs, POOL - 1, axis=0)[:POOL]
            pool[sel] = (np.take_along_axis(cs, top, 0), np.take_along_axis(ci, top, 0))
        if (lo // CHUNK) % 20 == 0:
            print(f'{lo + len(A):,} rows scored, {time.perf_counter() - t0:.0f} s', flush=True)

    # the candidates scored again in float64 from the float16 values
    need = np.unique(np.concatenate([pool[s][1].ravel() for s in k.SELS]))
    vecs = {}
    for f, first, n in ps:
        m = np.load(f, mmap_mode='r')
        mine = need[(need >= first) & (need < first + n)]
        for i in mine:
            vecs[int(i)] = np.asarray(m[i - first], np.float64)
    Qd = Q.astype(np.float64)
    Qd /= np.linalg.norm(Qd, axis=1, keepdims=True)
    gt = {}
    for sel in k.SELS:
        top = np.empty((k.NQ, 10), np.int64)
        for q in range(k.NQ):
            cand = np.unique(pool[sel][1][:, q])
            V = np.stack([vecs[int(i)] for i in cand])
            s = (V @ Qd[q]) / np.linalg.norm(V, axis=1)
            top[q] = k.top10(cand, s)
        gt[str(sel)] = top
        print(f'u < {sel}: {int((u < sel).sum())} rows', flush=True)
    np.savez(os.path.join(k.CACHE, 'truth.npz'), **gt)

    # unfiltered, against the data set's own top 1000 over all 113M rows:
    # those under ROWS, in their order, are the table's nearest
    offs = t.column('top1k_offsets').to_pylist()
    agree, short = [], 0
    for q, j in enumerate(pick):
        mine = [o for o in offs[j] if o < ROWS][:10]
        if len(mine) < 10:
            short += 1
            continue
        agree.append(len(set(mine) & set(gt['1.0'][q].tolist())) / 10)
    print(f'truth against top1k_offsets: {np.mean(agree):.4f} of the ten agree over {len(agree)} queries '
          f'({short} with fewer than ten under {ROWS})', flush=True)
    json.dump(dict(source='CohereLabs/msmarco-v2.1-embed-english-v3', files=[p[0] for p in ps], rows=ROWS,
                   queries=int(k.NQ), query_rows=[int(i) for i in pick],
                   query_ids=[t.column('_id')[int(i)].as_py() for i in pick], table_rows=ROWS,
                   members={str(s): int((u < s).sum()) for s in k.SELS},
                   truth_check=dict(agree=float(np.mean(agree)), queries=len(agree), short=short)),
              open(os.path.join(k.CACHE, 'info.json'), 'w'), indent=1)


# binary COPY: per row 3 fields; halfvec's binary form is int16 dims, int16
# unused, then the float16s, all big-endian
REC = np.dtype([('nf', '>i2'), ('l1', '>i4'), ('id', '>i4'), ('l2', '>i4'), ('u', '>f8'),
                ('l3', '>i4'), ('dim', '>i2'), ('unused', '>i2'), ('v', '>f2', (DIM,))])


def records_rec(ids, u, A):
    # the COPY rows of ids, u and the float16 rows A, as bytes
    a = np.zeros(len(A), REC)
    a['nf'], a['l1'], a['id'], a['l2'], a['u'] = 3, 4, ids, 8, u
    a['l3'], a['dim'], a['v'] = 4 + 2 * DIM, DIM, A
    return a.tobytes()


def copy_range(args):
    # a COPY of ids lo..hi through psql's stdin: psycopg's COPY writer moved
    # 19-25 MB/s here, psql 109 (500,000 rows, one process)
    lo, hi = args
    u = np.load(os.path.join(k.CACHE, 'u.npy'), mmap_mode='r')
    dbname = psycopg.conninfo.conninfo_to_dict(k.DSN).get('dbname', 'msmarco')
    p = subprocess.Popen(['psql', '-XAtq', '-v', 'ON_ERROR_STOP=1', '-d', dbname, '-c',
                          f'COPY {k.TABLE} (id, u, embedding) FROM STDIN (FORMAT binary)'],
                         stdin=subprocess.PIPE)
    p.stdin.write(b'PGCOPY\n\xff\r\n\x00' + (0).to_bytes(4, 'big') + (0).to_bytes(4, 'big'))
    for s, A in chunks(parts(), lo, hi):
        p.stdin.write(records_rec(np.arange(s, s + len(A)), u[s:s + len(A)], A))
    p.stdin.write((-1).to_bytes(2, 'big', signed=True))
    p.stdin.close()
    if p.wait() != 0:
        raise RuntimeError(f'COPY of ids {lo}..{hi} failed')
    return hi - lo


def load():
    admin = psycopg.connect(k.ADMIN_DSN, autocommit=True)
    dbname = psycopg.conninfo.conninfo_to_dict(k.DSN).get('dbname', 'msmarco')
    if admin.execute('SELECT 1 FROM pg_database WHERE datname = %s', (dbname,)).fetchone() is None:
        admin.execute(f'CREATE DATABASE {dbname}')
    admin.close()
    conn = psycopg.connect(k.DSN, autocommit=True, prepare_threshold=None)
    for e in ('vector', 'pg_lion', 'pg_lion_vector', 'pg_prewarm'):
        conn.execute(f'CREATE EXTENSION IF NOT EXISTS {e}')
    k.cost_distances(conn)
    T = k.TABLE
    conn.execute(f'DROP TABLE IF EXISTS {T}')
    conn.execute(f'CREATE TABLE {T} (id int, u float8, embedding halfvec({DIM})){k.STORAGE}')
    # contiguous id ranges, a COPY each, WORKERS at a time
    step = -(-ROWS // (WORKERS * 8))
    ranges = [(lo, min(ROWS, lo + step)) for lo in range(0, ROWS, step)]
    t = time.perf_counter()
    done = 0
    with mp.get_context('spawn').Pool(WORKERS) as p:
        for n in p.imap_unordered(copy_range, ranges):
            done += n
            print(f'{done:,} rows, {time.perf_counter() - t:.0f} s', flush=True)
    print(f'loaded {done} rows in {time.perf_counter() - t:.0f} s', flush=True)
    t = time.perf_counter()
    conn.execute(f'ALTER TABLE {T} ADD PRIMARY KEY (id)')
    print(f'{T}_pkey: built in {time.perf_counter() - t:.1f} s', flush=True)
    t = time.perf_counter()
    conn.execute(f'CREATE INDEX {T}_u ON {T} USING lion (u) WITH (summaries = auto)')
    print(f'{T}_u: built in {time.perf_counter() - t:.1f} s', flush=True)
    t = time.perf_counter()
    conn.execute(f'VACUUM ANALYZE {T}')
    print(f'VACUUM ANALYZE in {time.perf_counter() - t:.0f} s', flush=True)
    # a spot check: vectors read back as written
    ps = parts()
    for i in (0, 12345678 % ROWS, ROWS - 1):
        f, first, n = next(p for p in ps if p[1] <= i < p[1] + p[2])
        want = np.asarray(np.load(f, mmap_mode='r')[i - first], np.float32)
        back = np.array(json.loads(conn.execute(f'SELECT embedding::text FROM {T} WHERE id = %s',
                                                (i,)).fetchone()[0]), np.float32)
        assert np.array_equal(back, want), f'row {i} did not read back as written'
    k.sizes(conn)


if __name__ == '__main__':
    a = sys.argv[1:]
    if a == ['prepare']:
        prepare()
    elif a == ['load']:
        load()
    else:
        raise SystemExit('usage: knn_msmarco.py prepare | load')
