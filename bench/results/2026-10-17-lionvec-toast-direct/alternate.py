# Inline warm, main and the branch in alternating sessions (8), to tell a
# library's cost from a session's: each session restarts the server with
# its library, prewarms, and runs each kind's 10 queries once, then 5 times
# each (the median kept), a connection a kind.  knn_readahead.py's
# environment (KNN_DSN, KNN_CACHE, KNN_RA_SO_DIR, KNN_RA_INSTALL,
# KNN_RESTART); ALT_KINDS (u3,f3,x1,x3).
#
#   alternate.py OUT.json
import os, sys, json, numpy as np
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'vector'))
import knn_readahead as R
K = R.K
kinds = os.environ.get('ALT_KINDS', 'u3,f3,x1,x3').split(',')
libs = ['main', 'branch', 'branch', 'main', 'main', 'branch', 'branch', 'main']
X = np.load(os.path.join(K.CACHE, 'X.npy'), mmap_mode='r')
qidx = np.load(os.path.join(K.CACHE, 'qidx.npy'))
Q = np.asarray(X[qidx[:10]])
conn, _ = R.restart('main', False)
others = K.only_index(conn, K.INDEXES['lionvec'])
conn.close()
rows = []
try:
    for si, lib in enumerate(libs):
        conn, _ = R.restart(lib, False)
        K.prewarm(conn, K.INDEXES['lionvec'])
        conn.close()
        for kind in kinds:
            c = K.connect()
            cfg = dict(label=lib, lib=lib, mode='stream', direct=None)
            query = R.prepare_session(c, cfg, kind)
            for qi in range(10):
                R.answer(c, query, Q[qi])
            for qi in range(10):
                ms = [R.measure(c, query, Q[qi])['ms'] for _ in range(5)]
                rows.append(dict(session=si, lib=lib, kind=kind, qi=qi, ms=float(np.median(ms))))
            c.close()
        print(si, lib, {k: round(float(np.median([r['ms'] for r in rows if r['session'] == si and r['kind'] == k])), 2) for k in kinds}, flush=True)
finally:
    conn, _ = R.restart('branch', False)
    K.restore_indexes(conn, others)
    conn.close()
out = dict(libs=libs, kinds=kinds, so=dict(main=R.sha(os.path.join(R.SO_DIR, 'main.so')),
           branch=R.sha(os.path.join(R.SO_DIR, 'branch.so'))), rows=rows)
json.dump(out, open(sys.argv[1], 'w'), indent=1)
for k in kinds:
    m = {(r['lib'], r['qi']): [] for r in rows if r['kind'] == k}
    for r in rows:
        if r['kind'] == k:
            m[(r['lib'], r['qi'])].append(r['ms'])
    rat = [np.median(m[('branch', q)]) / np.median(m[('main', q)]) for q in range(10)]
    print(k, 'main', round(float(np.median([np.median(m[('main', q)]) for q in range(10)])), 2),
          'branch', round(float(np.median([np.median(m[('branch', q)]) for q in range(10)])), 2),
          'ratio', round(float(np.median(rat)), 3), round(float(np.percentile(rat, 25)), 3), round(float(np.percentile(rat, 75)), 3))
