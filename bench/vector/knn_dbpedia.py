#!/usr/bin/env python3
# lionvec against pgvector's HNSW on 115,386 DBpedia embeddings from OpenAI's
# text-embedding-3-large (1536 dimensions, cosine), with and without a filter
# (DESIGN.md §44.9, "DBpedia").  knn.py's method, on a public set anyone can
# fetch: shards 0-2 of the Hugging Face dataset
# Qdrant/dbpedia-entities-openai3-text-embedding-3-large-1536-1M.
#
# The queries are 200 of the rows, np.random.default_rng(0).choice(N, 200,
# replace=False) over the shards concatenated in sorted filename order (the
# numpy model's convention, vector-toy's stack.py), and they are HELD OUT of
# the table, so a query never finds itself.  The table is dbpedia(id int
# primary key, u float8, embedding vector(1536)): id is the row's place in the
# concatenated shards, u uniform in [0, 1) from np.random.default_rng(42)
# over every row (u[id]), so that `u < s` keeps a share s of the rows, with a
# lion index on it.  The vectors go in as the dataset has them, as float4: of
# unit length to within 1.4e-7 in these shards, and the cosine operator
# scales them anyway.
# The truth is the exact ten nearest by cosine of the float4 values the table
# holds, computed in float64 numpy, under each filter; no query's tenth and
# eleventh lie within 2.8e-5 of each other, so ties do not decide it.
#
# The HNSW index is pgvector's on embedding::halfvec(1536) (m = 16,
# ef_construction = 64), the lionvec one on embedding itself, with the
# `partitions` reloption given (0 lets the build choose).  pgvector's three
# vector distance functions are costed at 100, as docs/REFERENCE.md asks.
#
#   knn_dbpedia.py prepare          read the shards, pick the queries, compute
#                                   the truth; cached in KNN_CACHE
#   knn_dbpedia.py load             create the database, the table, its rows
#                                   and the lion index on u
#   knn_dbpedia.py hnsw             build the HNSW index (again, if it is there)
#   knn_dbpedia.py lionvec P        build the lionvec index with partitions = P
#                                   (again, if it is there)
#   knn_dbpedia.py vchord L         build VectorChord's vchordrq index with
#                                   lists = [L] (0: no lists), cosine, spherical
#                                   centroids; needs the vchord extension
#   knn_dbpedia.py diskann          build pgvectorscale's diskann index at its
#                                   defaults; needs the vectorscale extension
#   knn_dbpedia.py warm lionvec|hnsw|vchord|diskann NAME
#                                   recall and latency, warm, of each method of
#                                   that index and each filter, into
#                                   KNN_OUT/NAME.json with the index's build
#   knn_dbpedia.py table FILE...     the results as markdown tables, and the
#                                   pages a recall took, along the margins
#   knn_dbpedia.py cold             each query alone after a server restart
#                                   and a dropped page cache (run as root),
#                                   both indexes, into KNN_OUT/cold.json
#
# KNN_DSN (default "dbname=dbpedia", the rest from PGHOST and friends),
# KNN_ADMIN_DSN (default "dbname=postgres", to create the database), KNN_CACHE
# (default .local/dbpedia), KNN_OUT (default
# bench/results/2026-10-14-lionvec-dbpedia), KNN_NQ (default 200),
# KNN_SHARDS (default /mnt/project-files/train-0000[0-2]-of-00026.parquet),
# KNN_SRC (default this checkout: the one the installed pg_lion was built
# from, whose commit the results record),
# KNN_RESTART (default "./dev.sh restart"): the command cold runs to restart
# the server, and KNN_COLD_NQ (default 7) the queries each method runs cold.
# KNN_MARGINS (default 2,3,4,6,8,12,16) and KNN_PATIENCE (default 30): the
# pg_lion.vector_margin and pg_lion.vector_patience values warm runs.  The
# margins go past the default 3 because on these embeddings 3 returns 0.84 of
# the true ten, and 0.95 takes about 7.  The first runs took patiences 30 and
# 120 as well, from when the patience counted partitions' worth of rows, a
# quarter as many in four times the partitions; it counts sqrt(n) / 2 now.
# KNN_EF (default 20,40,70,100,200,400): the hnsw.ef_search values an hnsw
# run sweeps (the first runs took 40 and 100).  KNN_PROBES (default
# 10,20,40,80,160,320): the vchordrq.probes values a vchord run sweeps;
# KNN_SEARCH_LIST (default 10,20,30,50,100,200,400) and KNN_RESCORE (default
# 50): diskann.query_search_list_size and diskann.query_rescore for a diskann
# run.  KNN_COLD_VCHORD and KNN_COLD_DISKANN (default none): the probes and
# search list sizes cold runs.
#
# diskann answers an arbitrary WHERE by filtering what its scan streams out;
# its index-side filtering takes a smallint[] labels column named in the
# index, so the filter has to be known as a label when the index is built,
# and the runs here do not use it (a decision of 2026-10-15).
#
# lionvec, vchordrq and diskann index the same column, and the planner would
# choose among them by cost, so a run marks the other two invalid in pg_index
# for its duration (only_index(); the planner ignores an invalid index, as it
# does one a failed CREATE INDEX CONCURRENTLY left) and restores them after.
# That is a catalog write by the superuser, for the dev cluster this runs on.
import glob, hashlib, json, os, platform, re, subprocess, sys, time
import numpy as np
import pyarrow.parquet as pq
import psycopg
import psycopg.conninfo
# load needs the pg_lion, pg_lion_vector and pg_prewarm extensions installed

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
DSN = os.environ.get('KNN_DSN', 'dbname=dbpedia')
ADMIN_DSN = os.environ.get('KNN_ADMIN_DSN', 'dbname=postgres')
CACHE = os.environ.get('KNN_CACHE', os.path.join(REPO, '.local', 'dbpedia'))
SRC = os.environ.get('KNN_SRC', REPO)
OUT = os.environ.get('KNN_OUT', os.path.join(REPO, 'bench', 'results', '2026-10-14-lionvec-dbpedia'))
NQ = int(os.environ.get('KNN_NQ', '200'))
SHARDS = os.environ.get('KNN_SHARDS', '/mnt/project-files/train-0000[0-2]-of-00026.parquet')
COL = 'text-embedding-3-large-1536-embedding'
DIM = 1536
SELS = (1.0, 0.1, 0.03, 0.01)
MARGINS = [float(x) if '.' in x else int(x) for x in os.environ.get('KNN_MARGINS', '2,3,4,6,8,12,16').split(',')]
PATIENCE = [int(x) for x in os.environ.get('KNN_PATIENCE', '30').split(',')]
EFS = [int(x) for x in os.environ.get('KNN_EF', '20,40,70,100,200,400').split(',')]
PROBES = [int(x) for x in os.environ.get('KNN_PROBES', '10,20,40,80,160,320').split(',')]
SEARCH_LIST = [int(x) for x in os.environ.get('KNN_SEARCH_LIST', '10,20,30,50,100,200,400').split(',')]
RESCORE = [int(x) for x in os.environ.get('KNN_RESCORE', '50').split(',')]
INDEXES = {'lionvec': 'dbpedia_lv', 'hnsw': 'dbpedia_hnsw', 'vchord': 'dbpedia_vchord',
           'diskann': 'dbpedia_diskann'}
ON_EMBEDDING = ('dbpedia_lv', 'dbpedia_vchord', 'dbpedia_diskann')    # the three on the column itself

lit = lambda q: '[' + ','.join('%.9g' % x for x in q) + ']'
HNSW = ("SELECT id FROM dbpedia {w} ORDER BY embedding::halfvec(1536) <=> %(q)s::halfvec(1536) "
        "LIMIT 10")
LION = "SELECT id FROM dbpedia {w} ORDER BY embedding <=> %(q)s::vector LIMIT 10"
STATS = re.compile(r'lionvec scan of "[^"]*": (\d+) partitions probed, (\d+) passes, '
                   r'(\d+) candidates, (\d+) refined, (\d+) fetched, (\d+) returned, '
                   r'(\d+) skipped out of order, (\d+) bounds missed, (\d+) pages read'
                   r'(?:, (\d+) pauses)?(?:, (\d+) copies skipped)?')
EXACT = re.compile(r'lionvec scan of "[^"]*": (\d+) members fetched, (\d+) returned')
BUILT = re.compile(r'lionvec index "[^"]*": (\d+) partitions from a sample of (\d+) rows, '
                   r'(\d+) k-means iterations')
EXEC = re.compile(r'Execution Time: ([\d.]+) ms')
DISTANCES = ('l2_distance(vector, vector)', 'vector_negative_inner_product(vector, vector)',
             'cosine_distance(vector, vector)',
             'l2_distance(halfvec, halfvec)', 'halfvec_negative_inner_product(halfvec, halfvec)',
             'cosine_distance(halfvec, halfvec)')


def connect(dsn=DSN):
    conn = psycopg.connect(dsn, autocommit=True, prepare_threshold=None)
    conn.execute("LOAD 'pg_lion'")
    return conn


def cost_distances(conn):
    for f in DISTANCES:
        conn.execute(f'ALTER FUNCTION {f} COST 100')


def show(conn, *names):
    # the settings a run records, those the server has (io_method is 18's)
    out = {}
    for n in names:
        r = conn.execute("SELECT setting FROM pg_settings WHERE name = %s", (n,)).fetchone()
        if r:
            out[n] = r[0]
    return out


def only_index(conn, idx):
    # The other indexes on the embedding column, marked invalid so that the
    # planner can only take idx (see the head of the file); returns them for
    # restore_indexes().
    others = [r[0] for r in conn.execute(
        "SELECT c.relname FROM pg_index i JOIN pg_class c ON c.oid = i.indexrelid "
        "WHERE i.indrelid = 'dbpedia'::regclass AND i.indisvalid AND c.relname = ANY(%s)",
        ([o for o in ON_EMBEDDING if o != idx],))]
    for o in others:
        conn.execute("UPDATE pg_index SET indisvalid = false WHERE indexrelid = %s::regclass", (o,))
    if others:
        print(f'marked invalid for this run: {", ".join(others)}', flush=True)
    return others


def restore_indexes(conn, others):
    for o in others:
        conn.execute("UPDATE pg_index SET indisvalid = true WHERE indexrelid = %s::regclass", (o,))


def where(sel):
    return '' if sel >= 1.0 else f'WHERE u < {sel}'


def sql(kind, sel):
    # a kind's query under a filter (HNSW's is over halfvec)
    return (HNSW if kind == 'hnsw' else LION).format(w=where(sel))


def read_shards():
    files = sorted(glob.glob(SHARDS))
    if len(files) != 3:
        raise SystemExit(f'expected 3 shards matching {SHARDS}, found {files}')
    parts = []
    for f in files:
        col = pq.read_table(f, columns=[COL]).column(COL).combine_chunks()
        lens = np.diff(col.offsets.to_numpy())
        if not (lens == DIM).all():
            raise SystemExit(f'{f}: a vector of other than {DIM} dimensions')
        parts.append(col.flatten().to_numpy().astype(np.float32).reshape(-1, DIM))
        print(f'{f}: {parts[-1].shape[0]} rows', flush=True)
    return files, np.concatenate(parts)


def prepare():
    # the queries, held out, and the truth: the exact ten nearest by cosine of
    # the float4 values the table will hold, in float64, under each filter
    files, X = read_shards()
    N = X.shape[0]
    qidx = np.random.default_rng(0).choice(N, NQ, replace=False)
    u = np.random.default_rng(42).random(N)
    keep = np.ones(N, bool)
    keep[qidx] = False
    rows = np.flatnonzero(keep)
    Xd = X.astype(np.float64)
    Xd /= np.linalg.norm(Xd, axis=1, keepdims=True)
    Q = Xd[qidx]
    gt = {}
    S = np.empty((len(rows), NQ))
    for lo in range(0, len(rows), 20000):
        S[lo:lo + 20000] = Xd[rows[lo:lo + 20000]] @ Q.T
    for sel in SELS:
        m = u[rows] < sel
        ids, Ss = rows[m], S[m]
        top = np.empty((NQ, 10), np.int64)
        for i in range(NQ):
            # the greatest similarity first, ties by id
            o = np.lexsort((ids, -Ss[:, i]))[:10]
            top[i] = ids[o]
        gt[str(sel)] = top
        print(f'u < {sel}: {m.sum()} rows', flush=True)
    os.makedirs(CACHE, exist_ok=True)
    np.save(os.path.join(CACHE, 'X.npy'), X)
    np.save(os.path.join(CACHE, 'qidx.npy'), qidx)
    np.save(os.path.join(CACHE, 'u.npy'), u)
    np.savez(os.path.join(CACHE, 'truth.npz'), **gt)
    json.dump(dict(files=files, rows=int(N), queries=int(NQ), table_rows=int(len(rows)),
                   members={str(s): int((u[rows] < s).sum()) for s in SELS}),
              open(os.path.join(CACHE, 'info.json'), 'w'), indent=1)


def load():
    X = np.load(os.path.join(CACHE, 'X.npy'), mmap_mode='r')
    qidx = np.load(os.path.join(CACHE, 'qidx.npy'))
    u = np.load(os.path.join(CACHE, 'u.npy'))
    keep = np.ones(X.shape[0], bool)
    keep[qidx] = False
    rows = np.flatnonzero(keep)
    admin = psycopg.connect(ADMIN_DSN, autocommit=True)
    dbname = psycopg.conninfo.conninfo_to_dict(DSN).get('dbname', 'dbpedia')
    if admin.execute('SELECT 1 FROM pg_database WHERE datname = %s', (dbname,)).fetchone() is None:
        admin.execute(f'CREATE DATABASE {dbname}')
    admin.close()
    conn = psycopg.connect(DSN, autocommit=True, prepare_threshold=None)
    for e in ('vector', 'pg_lion', 'pg_lion_vector', 'pg_prewarm'):
        conn.execute(f'CREATE EXTENSION IF NOT EXISTS {e}')
    conn.execute("LOAD 'pg_lion'")
    cost_distances(conn)
    conn.execute('DROP TABLE IF EXISTS dbpedia')
    conn.execute('CREATE TABLE dbpedia (id int PRIMARY KEY, u float8, embedding vector(1536))')
    # binary COPY: per row 3 fields; vector's binary form is int16 dims, int16
    # unused, then the float4s, all big-endian
    rec = np.dtype([('nf', '>i2'), ('l1', '>i4'), ('id', '>i4'), ('l2', '>i4'), ('u', '>f8'),
                    ('l3', '>i4'), ('dim', '>i2'), ('unused', '>i2'), ('v', '>f4', (DIM,))])
    t = time.perf_counter()
    with conn.cursor() as cur:
        with cur.copy('COPY dbpedia (id, u, embedding) FROM STDIN (FORMAT binary)') as cp:
            cp.write(b'PGCOPY\n\xff\r\n\x00' + (0).to_bytes(4, 'big') + (0).to_bytes(4, 'big'))
            for lo in range(0, len(rows), 5000):
                r = rows[lo:lo + 5000]
                a = np.zeros(len(r), rec)
                a['nf'], a['l1'], a['id'], a['l2'], a['u'] = 3, 4, r, 8, u[r]
                a['l3'], a['dim'], a['v'] = 4 + 4 * DIM, DIM, X[r]
                cp.write(a.tobytes())
            cp.write((-1).to_bytes(2, 'big', signed=True))
    print(f'loaded {len(rows)} rows in {time.perf_counter() - t:.0f} s', flush=True)
    t = time.perf_counter()
    conn.execute('CREATE INDEX dbpedia_u ON dbpedia USING lion (u)')
    print(f'dbpedia_u: built in {time.perf_counter() - t:.1f} s', flush=True)
    conn.execute('VACUUM ANALYZE dbpedia')
    # a spot check: the vectors read back as written
    i = int(rows[12345])
    back = np.array(json.loads(conn.execute('SELECT embedding::text FROM dbpedia WHERE id = %s',
                                            (i,)).fetchone()[0]), np.float32)
    assert np.array_equal(back, X[i]), 'a vector did not read back as written'
    sizes()


def sizes(conn=None):
    conn = conn or connect()
    out = {}
    for name, size in conn.execute(
            "SELECT c.relname, pg_relation_size(c.oid) FROM pg_class c "
            "WHERE c.relname IN ('dbpedia', 'dbpedia_pkey', 'dbpedia_u', 'dbpedia_lv', 'dbpedia_hnsw', "
            "'dbpedia_vchord', 'dbpedia_diskann') "
            "UNION ALL SELECT 'dbpedia (TOAST)', pg_relation_size(reltoastrelid) FROM pg_class "
            "WHERE relname = 'dbpedia' ORDER BY 1"):
        out[name] = int(size)
        print(f'{name}: {size / 1048576:.1f} MB', flush=True)
    return out


def build(name, ddl):
    conn = connect()
    cost_distances(conn)
    notes = []
    conn.add_notice_handler(lambda d: notes.append(d.message_primary))
    conn.execute(f'DROP INDEX IF EXISTS {name}')
    conn.execute('SET client_min_messages = debug1')
    t = time.perf_counter()
    conn.execute(ddl)
    secs = time.perf_counter() - t
    conn.execute('SET client_min_messages = warning')
    info = dict(index=name, ddl=ddl, build_s=round(secs, 1),
                maintenance_work_mem=conn.execute('SHOW maintenance_work_mem').fetchone()[0],
                size_bytes=int(conn.execute('SELECT pg_relation_size(%s::regclass)',
                                            (name,)).fetchone()[0]),
                built_at=time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime()), **provenance())
    for m in notes:
        g = BUILT.search(m)
        if g:
            info.update(partitions=int(g.group(1)), sample=int(g.group(2)),
                        kmeans_iterations=int(g.group(3)))
    conn.execute('ANALYZE dbpedia')
    print(json.dumps(info), flush=True)
    os.makedirs(CACHE, exist_ok=True)
    json.dump(info, open(os.path.join(CACHE, f'{name}.build.json'), 'w'), indent=1)


def provenance():
    # what the results came from: the checkout's HEAD (KNN_SRC's, where the
    # installed library was built from another), the last commit to change
    # src/ and whether src/ differs from it, and the installed library
    git = lambda *a: subprocess.run(['git', '-C', SRC] + list(a), capture_output=True,
                                    text=True).stdout.strip()
    libdir = subprocess.run(['pg_config', '--pkglibdir'], capture_output=True, text=True).stdout.strip()
    try:
        lib = hashlib.sha256(open(os.path.join(libdir, 'pg_lion.so'), 'rb').read()).hexdigest()[:16]
    except OSError:
        lib = None
    return dict(checkout=SRC, head=git('rev-parse', '--short', 'HEAD'),
                branch=git('rev-parse', '--abbrev-ref', 'HEAD'),
                src_commit=git('log', '-1', '--format=%h', '--', 'src'),
                src_dirty=bool(git('status', '--porcelain', '--', 'src')), pg_lion_so_sha256=lib)


def run(conn, name, sql, sel, settings, Q, gt):
    stats = []
    handler = lambda d: stats.append(d.message_primary)
    conn.add_notice_handler(handler)
    cur = conn.cursor()
    cur.execute('RESET ALL')
    for s in settings:
        cur.execute(s)
    cur.execute('SET client_min_messages = debug1')
    nq = len(Q)
    for i in range(nq):                          # warm the plan, the session and
        cur.execute(sql, {'q': lit(Q[i])})       # shared_buffers with the pages
        cur.fetchall()                           # these queries read
    rec, ts, nret, scans, exact = [], [], [], [], []
    for i in range(nq):
        stats.clear()
        t = time.perf_counter()
        cur.execute(sql, {'q': lit(Q[i])})
        ids = [r[0] for r in cur.fetchall()]
        ts.append((time.perf_counter() - t) * 1e3)
        rec.append(len(set(ids) & set(gt[i].tolist())) / 10)
        nret.append(len(ids))
        for m in stats:
            g = STATS.search(m)
            if g:
                scans.append([int(x or 0) for x in g.groups()])
            g = EXACT.search(m)
            if g:
                exact.append(int(g.group(1)))
    # the server's own time, without the client's: EXPLAIN ANALYZE with no
    # per-node timing, and the plan's nodes, which a query could change
    cur.execute('SET client_min_messages = warning')
    xs, shapes, plan = [], set(), None
    for i in range(nq):
        cur.execute('EXPLAIN (ANALYZE, TIMING OFF, COSTS OFF) ' + sql, {'q': lit(Q[i])})
        lines = [r[0] for r in cur.fetchall()]
        xs.append(float(next(EXEC.search(l).group(1) for l in lines if EXEC.search(l))))
        nodes = [l.strip() for l in lines if re.search(r'Scan|Sort|Lion|Limit|Filter', l)
                 and not re.search(r'Rows Removed|Sort Method|Heap Blocks', l)]
        shapes.add(' / '.join(re.sub(r'\(actual.*|\'\[[^]]*\]\'.*', '', n).strip() for n in nodes))
        if plan is None:
            plan = ' / '.join(re.sub(r"'\[[^]]*\]'(::[a-z(0-9)]+)?", "'...'", n) for n in nodes)[:400]
    row = dict(name=name, sel=sel, recall=float(np.mean(rec)), exact_q=float(np.mean(np.array(rec) == 1)),
               returned=float(np.mean(nret)), median_ms=float(np.median(ts)),
               p95_ms=float(np.percentile(ts, 95)), exec_median_ms=float(np.median(xs)),
               exec_p95_ms=float(np.percentile(xs, 95)), plan=plan, plans=len(shapes))
    if scans:
        a = np.array(scans)
        row.update(scans=len(scans), probes=float(a[:, 0].mean()),
                   probes_median=float(np.median(a[:, 0])), probes_p95=float(np.percentile(a[:, 0], 95)),
                   passes=float(a[:, 1].mean()), candidates=float(a[:, 2].mean()),
                   refined=float(a[:, 3].mean()), fetched=float(a[:, 4].mean()),
                   skipped=float(a[:, 6].mean()), missed=float(a[:, 7].mean()),
                   pages=float(a[:, 8].mean()), pauses=float(a[:, 9].mean()))
    if exact:
        row.update(members_fetched=float(np.mean(exact)))
    conn.remove_notice_handler(handler)
    print(json.dumps(row), flush=True)
    return row


def methods(kind, sel):
    m = []
    # The planner's own plans, and the index's own paths too, since under a
    # filter the planner may sort the lion bitmap scan's rows by distance
    # instead: for lionvec LionOrdered (enable_sort and enable_indexscan
    # off), which under the 1% filter computes every member's distance, for
    # HNSW its iterative scan (enable_sort off).
    if kind == 'lionvec':
        for patience in PATIENCE:
            for margin in MARGINS:
                st = [f'SET pg_lion.vector_margin = {margin}', f'SET pg_lion.vector_patience = {patience}']
                if sel <= 0.01 and margin != 3:
                    continue        # every member's distance: no margin, no pause
                m.append((f'lionvec margin={margin} patience={patience}', sql(kind, sel), st))
                if sel < 1.0:
                    m.append((f'lionvec margin={margin} patience={patience} LionOrdered', sql(kind, sel),
                              st + ['SET enable_sort = off', 'SET enable_indexscan = off']))
    elif kind == 'hnsw':
        for ef in EFS:
            s = [f'SET hnsw.ef_search = {ef}']
            if sel < 1.0:
                s.append('SET hnsw.iterative_scan = relaxed_order')
                m.append((f'hnsw ef={ef} iterative', sql(kind, sel), s))
                m.append((f'hnsw ef={ef} iterative enable_sort=off', sql(kind, sel),
                          s + ['SET enable_sort = off']))
            else:
                m.append((f'hnsw ef={ef}', sql(kind, sel), s))
    elif kind == 'vchord':
        # VectorChord's vchordrq: probes lists a query scans; under a filter
        # the planner's plan, the index's scan with vchordrq.prefilter, which
        # checks each candidate's filter at its heap tuple before the rerank,
        # and without it, the filter applied to what the scan returns
        for probes in PROBES:
            s = [f"SET vchordrq.probes = '{probes}'"]
            m.append((f'vchord probes={probes}', sql(kind, sel), s))
            if sel < 1.0:
                m.append((f'vchord probes={probes} prefilter', sql(kind, sel),
                          s + ['SET vchordrq.prefilter = on', 'SET enable_sort = off']))
                m.append((f'vchord probes={probes} enable_sort=off', sql(kind, sel),
                          s + ['SET enable_sort = off']))
    else:
        # pgvectorscale's diskann: the search list size and the rescored
        # count; under a filter its streaming scan with the filter applied to
        # what it returns (its index-side filtering takes a labels column)
        for rescore in RESCORE:
            for size in SEARCH_LIST:
                s = [f'SET diskann.query_search_list_size = {size}', f'SET diskann.query_rescore = {rescore}']
                name = f'diskann list={size} rescore={rescore}'
                m.append((name, sql(kind, sel), s))
                if sel < 1.0:
                    m.append((name + ' enable_sort=off', sql(kind, sel), s + ['SET enable_sort = off']))
    return m


def warm(kind, name):
    X = np.load(os.path.join(CACHE, 'X.npy'), mmap_mode='r')
    qidx = np.load(os.path.join(CACHE, 'qidx.npy'))
    Q = np.asarray(X[qidx[:NQ]])
    truth = np.load(os.path.join(CACHE, 'truth.npz'))
    idx = INDEXES[kind]
    conn = connect()
    cost_distances(conn)
    # warm: every relation in shared_buffers (2 GB holds them all here),
    # each method's queries run once untimed first
    for q in ["SELECT pg_prewarm('dbpedia')", "SELECT pg_prewarm('dbpedia_pkey')",
              "SELECT pg_prewarm('dbpedia_u')", f"SELECT pg_prewarm('{idx}')",
              "SELECT pg_prewarm(reltoastrelid) FROM pg_class WHERE relname = 'dbpedia'",
              "SELECT pg_prewarm(indexrelid) FROM pg_index WHERE indrelid = "
              "(SELECT reltoastrelid FROM pg_class WHERE relname = 'dbpedia')"]:
        t = time.perf_counter()
        n = conn.execute(q).fetchone()[0]
        print(f'{q}: {n} pages, {time.perf_counter() - t:.1f} s', flush=True)
    try:
        built = json.load(open(os.path.join(CACHE, f'{idx}.build.json')))
    except OSError:
        built = None
    meta = dict(name=name, index=idx, build=built, run=provenance(),
                run_at=time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime()), queries=len(Q),
                settings=show(conn, 'shared_buffers', 'work_mem', 'maintenance_work_mem', 'jit',
                              'max_parallel_workers_per_gather', 'max_parallel_maintenance_workers',
                              'random_page_cost', 'effective_io_concurrency', 'effective_cache_size',
                              'io_method', 'server_version'),
                pgvector=conn.execute("SELECT extversion FROM pg_extension WHERE extname = 'vector'").fetchone()[0],
                extensions=dict(conn.execute("SELECT extname, extversion FROM pg_extension WHERE extname IN "
                                             "('vector', 'pg_lion_vector', 'vchord', 'vectorscale')").fetchall()),
                sizes=sizes(conn), machine=dict(cpus=os.cpu_count(), cpu=cpu_model(),
                                                kernel=platform.release()))
    out = []
    others = only_index(conn, idx)
    try:
        for sel in SELS:
            for mname, query, settings in methods(kind, sel):
                out.append(run(conn, mname, query, sel, settings, Q, truth[str(sel)]))
    finally:
        restore_indexes(conn, others)
    os.makedirs(OUT, exist_ok=True)
    json.dump(dict(meta=meta, results=out), open(os.path.join(OUT, f'{name}.json'), 'w'), indent=1)


def cold():
    # What a query costs when nothing is in memory: PostgreSQL restarted and
    # the page cache dropped before each one (the restart alone does not
    # empty the kernel's cache), two seconds for the flush to settle, then
    # the query once under EXPLAIN (ANALYZE, BUFFERS) for the server's time
    # and the pages it read from the kernel.  A few methods a filter, at the
    # recalls the warm runs measured; the median of KNN_COLD_NQ queries, since
    # a cloud disk's latency varies from read to read.
    nq = int(os.environ.get('KNN_COLD_NQ', '7'))
    restart = os.environ.get('KNN_RESTART', './dev.sh restart')
    X = np.load(os.path.join(CACHE, 'X.npy'), mmap_mode='r')
    qidx = np.load(os.path.join(CACHE, 'qidx.npy'))
    Q = np.asarray(X[qidx[:nq]])
    lo = ['SET enable_sort = off', 'SET enable_indexscan = off']
    hn = ['SET hnsw.ef_search = 40', 'SET hnsw.iterative_scan = relaxed_order']
    probes = [int(x) for x in os.environ.get('KNN_COLD_VCHORD', '').split(',') if x]
    lists = [int(x) for x in os.environ.get('KNN_COLD_DISKANN', '').split(',') if x]
    configs = []
    for margin in (3, 7, 12):
        configs.append(('lionvec', 1.0, f'lionvec margin={margin}', sql('lionvec', 1.0),
                        [f'SET pg_lion.vector_margin = {margin}']))
    for ef in (40, 100):
        configs.append(('hnsw', 1.0, f'hnsw ef={ef}', sql('hnsw', 1.0), [f'SET hnsw.ef_search = {ef}']))
    for p in probes:
        configs.append(('vchord', 1.0, f'vchord probes={p}', sql('vchord', 1.0), [f"SET vchordrq.probes = '{p}'"]))
    for l in lists:
        configs.append(('diskann', 1.0, f'diskann list={l}', sql('diskann', 1.0),
                        [f'SET diskann.query_search_list_size = {l}']))
    for sel in (0.03, 0.01):
        configs.append(('lionvec', sel, 'lionvec planner plan', sql('lionvec', sel),
                        ['SET pg_lion.vector_margin = 3']))
        for margin in (3, 7) if sel > 0.01 else (3,):
            configs.append(('lionvec', sel, f'lionvec margin={margin} LionOrdered', sql('lionvec', sel),
                            [f'SET pg_lion.vector_margin = {margin}'] + lo))
        configs.append(('hnsw', sel, 'hnsw ef=40 iterative', sql('hnsw', sel), hn))
        configs.append(('hnsw', sel, 'hnsw ef=40 iterative enable_sort=off', sql('hnsw', sel),
                        hn + ['SET enable_sort = off']))
        for p in probes:
            configs.append(('vchord', sel, f'vchord probes={p} prefilter', sql('vchord', sel),
                            [f"SET vchordrq.probes = '{p}'", 'SET vchordrq.prefilter = on', 'SET enable_sort = off']))
        for l in lists:
            configs.append(('diskann', sel, f'diskann list={l} enable_sort=off', sql('diskann', sel),
                            [f'SET diskann.query_search_list_size = {l}', 'SET enable_sort = off']))
    read = re.compile(r'Buffers: shared[^\n]*?read=(\d+)')
    out = []
    for kind, sel, name, query, settings in configs:
        ms, pages, plan = [], [], None
        conn = connect()
        others = only_index(conn, INDEXES[kind])     # persists across the restarts
        conn.close()
        for i in range(nq):
            subprocess.run(restart, shell=True, check=True, capture_output=True)
            dropped = subprocess.run(['sh', '-c', 'sync; echo 3 > /proc/sys/vm/drop_caches'],
                                     capture_output=True).returncode == 0
            time.sleep(2)
            for _ in range(50):
                try:
                    conn = connect()
                    break
                except psycopg.OperationalError:
                    time.sleep(0.2)
            cost_distances(conn)
            for st in settings:
                conn.execute(st)
            lines = [r[0] for r in conn.execute('EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, COSTS OFF) ' + query,
                                                {'q': lit(Q[i])}).fetchall()]
            ms.append(float(next(EXEC.search(l).group(1) for l in lines if EXEC.search(l))))
            g = read.search(next((l for l in lines if 'Buffers:' in l), ''))
            pages.append(int(g.group(1)) if g else 0)
            if plan is None:
                plan = ' / '.join(l.strip()[:60] for l in lines
                                  if re.search(r'Scan|Sort|Lion|Limit', l) and 'Buffers' not in l)[:200]
            conn.close()
        conn = connect()
        restore_indexes(conn, others)
        conn.close()
        row = dict(name=name, sel=sel, cache_dropped=dropped, median_ms=float(np.median(ms)),
                   max_ms=float(max(ms)), all_ms=[round(x, 1) for x in ms],
                   pages_read_median=float(np.median(pages)), all_pages=pages, plan=plan)
        print(json.dumps(row), flush=True)
        out.append(row)
    os.makedirs(OUT, exist_ok=True)
    json.dump(dict(meta=dict(run=provenance(), queries=nq, restart=restart,
                             run_at=time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime())), results=out),
              open(os.path.join(OUT, 'cold.json'), 'w'), indent=1)


def table(files):
    # the results as markdown: every row, then for each lionvec file and
    # pause, the CODE pages, partitions and time a scan needed for a recall,
    # interpolated along the margins (unfiltered, and through each filter's
    # LionOrdered or plain scan)
    print('| file | filter | method | recall | median ms (p95) | server ms | probed | pages | plan |')
    print('| --- | --- | --- | --- | --- | --- | --- | --- | --- |')
    data = {}
    for f in files:
        d = json.load(open(f))
        name = d['meta']['name']
        data[name] = d
        for r in d['results']:
            plan = ('LionOrdered' if 'LionOrdered' in r['plan'] else 'Sort' if 'Sort' in r['plan']
                    else 'Index Scan + Filter' if 'Filter' in r['plan'] else 'Index Scan')
            probed = (f"{r['probes']:.0f}" if 'probes' in r else
                      f"exact ({r['members_fetched']:.0f})" if 'members_fetched' in r else '')
            pages = f"{r['pages']:.0f}" if 'pages' in r else ''
            print(f"| {name} | {r['sel']:g} | {r['name']} | {r['recall']:.3f} | {r['median_ms']:.1f} "
                  f"({r['p95_ms']:.1f}) | {r['exec_median_ms']:.1f} | {probed} | {pages} | {plan} |")
    print()
    print('| file | filter | path | patience | recall | pages | probed | server ms |')
    print('| --- | --- | --- | --- | --- | --- | --- | --- |')
    for name, d in data.items():
        for sel in SELS:
            for path in ('', ' LionOrdered'):
                for patience in sorted({int(re.search(r'patience=(\d+)', r['name']).group(1))
                                        for r in d['results'] if 'patience=' in r['name']}):
                    pts = sorted((r['recall'], r['pages'], r['probes'], r['exec_median_ms'])
                                 for r in d['results'] if r['sel'] == sel and 'pages' in r
                                 and re.fullmatch(rf'lionvec margin=[\d.]+ patience={patience}{path}', r['name']))
                    if len(pts) < 2:
                        continue
                    for target in (0.95, 0.98):
                        hit = next(((a, b) for a, b in zip(pts, pts[1:]) if a[0] <= target <= b[0]), None)
                        if hit is None:
                            print(f'| {name} | {sel:g} | {path.strip() or "planner"} | {patience} | {target} '
                                  f'| not reached (best {pts[-1][0]:.3f}) | | |')
                            continue
                        (r0, *v0), (r1, *v1) = hit
                        w = (target - r0) / (r1 - r0) if r1 > r0 else 0.0
                        v = [a + w * (b - a) for a, b in zip(v0, v1)]
                        print(f'| {name} | {sel:g} | {path.strip() or "planner"} | {patience} | {target} '
                              f'| {v[0]:.0f} | {v[1]:.0f} | {v[2]:.1f} |')
    # the other kinds: the server time a recall took, interpolated along the
    # setting a method sweeps (ef, probes, the search list size), the rows of
    # one method differing in that number alone
    print()
    print('| file | filter | method | recall | server ms | setting |')
    print('| --- | --- | --- | --- | --- | --- |')
    for name, d in data.items():
        fams = {}
        for r in d['results']:
            g = re.search(r'=(\d+)', r['name'])
            if r['name'].startswith('lionvec') or not g:
                continue
            fam = r['name'][:g.start(1)] + '*' + r['name'][g.end(1):]
            plan = 'Sort' if 'Sort' in r['plan'] else 'scan'
            fams.setdefault((r['sel'], fam), []).append((int(g.group(1)), r['recall'], r['exec_median_ms'], plan))
        for (sel, fam), pts in sorted(fams.items(), key=lambda kv: (-kv[0][0], kv[0][1])):
            pts.sort()
            if len(pts) < 2:
                continue
            for target in (0.95, 0.98):
                if pts[0][1] >= target:
                    # the smallest setting swept is already there: its own row
                    print(f'| {name} | {sel:g} | {fam} | {target} | {pts[0][2]:.1f} | {pts[0][0]} ({pts[0][1]:.3f}) |')
                    continue
                hit = next(((a, b) for a, b in zip(pts, pts[1:]) if a[1] <= target <= b[1]), None)
                if hit is None:
                    print(f'| {name} | {sel:g} | {fam} | {target} | not reached (best {max(p[1] for p in pts):.3f}) | |')
                    continue
                (p0, r0, t0, pl0), (p1, r1, t1, pl1) = hit
                if pl0 != pl1:
                    # the planner's plan changes between the two settings: no
                    # line to interpolate along
                    print(f'| {name} | {sel:g} | {fam} | {target} | plan changes between {p0} ({r0:.3f}, {t0:.1f} ms, '
                          f'{pl0}) and {p1} ({r1:.3f}, {t1:.1f} ms, {pl1}) | |')
                    continue
                w = (target - r0) / (r1 - r0) if r1 > r0 else 0.0
                print(f'| {name} | {sel:g} | {fam} | {target} | {t0 + w * (t1 - t0):.1f} | {p0 + w * (p1 - p0):.0f} |')


def cpu_model():
    try:
        return next(l.split(':', 1)[1].strip() for l in open('/proc/cpuinfo') if l.startswith('model name'))
    except (OSError, StopIteration):
        return platform.processor()


if __name__ == '__main__':
    a = sys.argv[1:]
    if a[:1] == ['prepare']:
        prepare()
    elif a[:1] == ['load']:
        load()
    elif a[:1] == ['hnsw']:
        build('dbpedia_hnsw', 'CREATE INDEX dbpedia_hnsw ON dbpedia USING hnsw '
              '((embedding::halfvec(1536)) halfvec_cosine_ops) WITH (m = 16, ef_construction = 64)')
    elif a[:1] == ['lionvec'] and len(a) == 2:
        build('dbpedia_lv', 'CREATE INDEX dbpedia_lv ON dbpedia USING lionvec '
              f'(embedding vector_cosine_ops) WITH (partitions = {int(a[1])})')
    elif a[:1] == ['vchord'] and len(a) == 2:
        # VectorChord's recommendation for cosine: no residual quantization,
        # spherical centroids; lists = [] clusters nothing (one list)
        lists = '[]' if int(a[1]) == 0 else f'[{int(a[1])}]'
        build('dbpedia_vchord', 'CREATE INDEX dbpedia_vchord ON dbpedia USING vchordrq '
              '(embedding vector_cosine_ops) WITH (options = $$\nresidual_quantization = false\n'
              f'[build.internal]\nlists = {lists}\nspherical_centroids = true\n$$)')
    elif a == ['diskann']:
        build('dbpedia_diskann', 'CREATE INDEX dbpedia_diskann ON dbpedia USING diskann '
              '(embedding vector_cosine_ops)')
    elif a[:1] == ['warm'] and len(a) == 3 and a[1] in INDEXES:
        warm(a[1], a[2])
    elif a[:1] == ['table'] and len(a) > 1:
        table(a[1:])
    elif a == ['cold']:
        cold()
    elif a[:1] == ['sizes']:
        sizes()
    else:
        raise SystemExit('usage: knn_dbpedia.py prepare | load | hnsw | lionvec P | vchord L | diskann | '
                         'warm lionvec|hnsw|vchord|diskann NAME | table FILE... | sizes | cold')
