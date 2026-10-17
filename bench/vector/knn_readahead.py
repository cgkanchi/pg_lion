#!/usr/bin/env python3
# How lionvec reads ahead (DESIGN.md §44.4, "Prefetch" and "Read streams";
# §44.9), on knn_dbpedia.py's DBpedia table, queries and truth: the same
# queries under several builds of pg_lion and settings of
# pg_lion.vector_prefetch, in three states of the caches:
#
#   device   the server restarted and the kernel's page cache dropped before
#            each query, two seconds for the flush to settle: every page from
#            the disk
#   os       the server restarted before each query, the relations read into
#            the page cache once beforehand (pg_prewarm(..., 'read')): every
#            page from the kernel, none in shared buffers
#   warm     every relation in shared buffers (pg_prewarm), each query run
#            once first, then KNN_RA_WARM_REPS times, the configurations of
#            a library taking turns in one session
#
# Each query runs under EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, COSTS OFF)
# with track_io_timing on and the scan's DEBUG1 line caught: the server's
# time, the pages read from outside shared buffers, the time waited on them
# (I/O Timings: shared read), and what the scan read ahead.  Then once more,
# for its rows: their recall against the truth, and a check that every
# configuration returned the same rows in the same order (they must: reading
# ahead changes which pages come first, never which are read).  In the cold
# states the configurations take turns query by query, so that a slow
# stretch of the disk falls on all of them.
#
#   knn_readahead.py NAME            into KNN_OUT/NAME.json: the medians of
#                                    each state, kind and configuration
#                                    (results), and every query's own row
#                                    (raw)
#   knn_readahead.py table FILE...   the results as markdown tables
#
# KNN_RA_CONFIGS (default main:main:on,off:branch:off,fadvise:branch:fadvise,
# stream:branch:stream): label:library:pg_lion.vector_prefetch[:direct], each
# library KNN_RA_SO_DIR/LIBRARY.so (default .local/so), copied over
# KNN_RA_INSTALL (default pg_config's pkglibdir/pg_lion.so) before the
# restart that loads it; direct, where given, pg_lion.vector_toast_direct
# (DESIGN.md §44.13: on reads the vectors in TOAST by their places, off as
# before).
# KNN_RA_STATES (default device,os,warm), KNN_RA_KINDS (default
# u3,u8,f10,f3,f1,x1,x3; KINDS below), KNN_RA_NQ (default 10) queries a kind,
# KNN_RA_WARM_REPS (default 3).  KNN_RESTART (default "./dev.sh restart") and
# KNN_DROP as knn_dbpedia.py's cold runs; KNN_RA_DEVICE (default vda) the disk
# whose reads /proc/diskstats counts.  The io_method is the server's: run
# once for each, restarting the server between.
import hashlib, json, os, re, subprocess, sys, time
import numpy as np
import psycopg

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import knn_dbpedia as K  # noqa: E402

LO = ['SET enable_sort = off', 'SET enable_indexscan = off']
EXACT_OFF = ['SET pg_lion.vector_exact_rows = 0', 'SET pg_lion.vector_exact_per_partition = 0']
# kind: (filter, settings)
KINDS = {
    'u3': (1.0, ['SET pg_lion.vector_margin = 3']),
    'u8': (1.0, ['SET pg_lion.vector_margin = 8']),
    'f10': (0.1, ['SET pg_lion.vector_margin = 3'] + EXACT_OFF + LO),
    'f3': (0.03, ['SET pg_lion.vector_margin = 3'] + EXACT_OFF + LO),
    'f1': (0.01, ['SET pg_lion.vector_margin = 3'] + EXACT_OFF + LO),
    'x1': (0.01, LO),
    'x3': (0.03, ['SET pg_lion.vector_exact_rows = 200000'] + LO),
}
STREAMED = re.compile(r'(\d+) pages streamed \((\d+) read in\), (\d+) read apart, (\d+) streamed unread'
                      r'(?:, (\d+) restarts)?'
                      r'(?:, (\d+) heap pages prefetched \((\d+) not in shared buffers\))?')
ASKED = re.compile(r'(\d+) pages prefetched \((\d+) not in shared buffers\)')
PLACED = re.compile(r'(\d+) read by place \((\d+) locator pages, (\d+) not\), (\d+) checked'
                    r'(?:, (\d+) TOAST pages prefetched \((\d+) not in shared buffers\))?')
READ = re.compile(r'Buffers: shared[^\n]*?read=(\d+)')
HIT = re.compile(r'Buffers: shared hit=(\d+)')
IOWAIT = re.compile(r'I/O Timings: shared read=([\d.]+)')


def configs():
    out = []
    spec = os.environ.get('KNN_RA_CONFIGS',
                          'main:main:on,off:branch:off,fadvise:branch:fadvise,stream:branch:stream')
    for c in spec.split(','):
        f = c.split(':')
        out.append(dict(label=f[0], lib=f[1], mode=f[2], direct=f[3] if len(f) > 3 else None))
    return out


def apply(conn, cfg):
    # a configuration's settings, in a session of its library
    conn.execute(f"SET pg_lion.vector_prefetch = {cfg['mode']}")
    if cfg.get('direct'):
        conn.execute(f"SET pg_lion.vector_toast_direct = {cfg['direct']}")


SO_DIR = os.environ.get('KNN_RA_SO_DIR', os.path.join(K.REPO, '.local', 'so'))
INSTALL = os.environ.get('KNN_RA_INSTALL') or os.path.join(
    subprocess.run(['pg_config', '--pkglibdir'], capture_output=True, text=True,
                   check=True).stdout.strip(), 'pg_lion.so')
RESTART = os.environ.get('KNN_RESTART', './dev.sh restart')
DROP = os.environ.get('KNN_DROP', 'sync; echo 3 > /proc/sys/vm/drop_caches')
DEVICE = os.environ.get('KNN_RA_DEVICE', 'vda')
_installed = [None]


def sha(path):
    return hashlib.sha256(open(path, 'rb').read()).hexdigest()[:16]


def restart(lib, drop):
    # the library in place, the server restarted, the page cache dropped if
    # asked; a connection once the server takes one
    if _installed[0] != lib:
        # a new file renamed over the old one: the running server's mapping
        # of the old is left alone until the restart
        subprocess.run(['cp', '-p', os.path.join(SO_DIR, lib + '.so'), INSTALL + '.new'], check=True)
        os.replace(INSTALL + '.new', INSTALL)
        _installed[0] = lib
    subprocess.run(RESTART, shell=True, check=True, capture_output=True)
    dropped = None
    if drop:
        dropped = subprocess.run(['sh', '-c', DROP], capture_output=True).returncode == 0
        time.sleep(2)
    for _ in range(100):
        try:
            return K.connect(), dropped
        except psycopg.OperationalError:
            time.sleep(0.2)
    raise SystemExit('the server did not come back')


def os_cache(conn):
    # every relation the queries read, into the kernel's page cache: the
    # table's, and its TOAST relation and that one's index
    t = K.TABLE
    for rel in (t, f'{t}_pkey', f'{t}_u', K.INDEXES['lionvec']):
        conn.execute(f"SELECT pg_prewarm('{rel}', 'read')")
    toast = f"(SELECT reltoastrelid FROM pg_class WHERE relname = '{t}')"
    conn.execute(f"SELECT pg_prewarm({toast}, 'read') WHERE {toast} <> 0")
    conn.execute(f"SELECT pg_prewarm(indexrelid, 'read') FROM pg_index WHERE indrelid = {toast}")


def prepare_session(conn, cfg, kind):
    sel, settings = KINDS[kind]
    K.cost_distances(conn)
    conn.execute('SET track_io_timing = on')
    apply(conn, cfg)
    for s in settings:
        conn.execute(s)
    return K.sql('lionvec', sel)


def device_kb():
    # what the disk holding the data has read, in kB (/proc/diskstats'
    # sectors read): asks of the kernel read too, unlike shared reads
    for line in open('/proc/diskstats'):
        f = line.split()
        if f[2] == DEVICE:
            return int(f[5]) // 2
    return 0


def measure(conn, query, q):
    # one query under EXPLAIN ANALYZE, its DEBUG1 lines caught
    notes = []
    handler = lambda d: notes.append(d.message_primary)
    conn.add_notice_handler(handler)
    conn.execute('SET client_min_messages = debug1')
    kb = device_kb()
    lines = [r[0] for r in conn.execute('EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, COSTS OFF) ' + query,
                                        {'q': K.lit(q)}).fetchall()]
    kb = device_kb() - kb
    conn.execute('SET client_min_messages = warning')
    conn.remove_notice_handler(handler)
    text = '\n'.join(lines)
    row = dict(ms=float(K.EXEC.search(text).group(1)), dev_kb=kb)
    buf = next((l for l in lines if 'Buffers:' in l), '')
    g = READ.search(buf)
    row['read'] = int(g.group(1)) if g else 0
    g = HIT.search(buf)
    row['hit'] = int(g.group(1)) if g else 0
    g = IOWAIT.search(text)
    row['io_ms'] = float(g.group(1)) if g else 0.0
    for m in notes:
        g = K.STATS.search(m)
        if g:
            row['probes'], row['pages'] = int(g.group(1)), int(g.group(9))
        g = STREAMED.search(m)
        if g:
            row.update(streamed=int(g.group(1)), stream_read_in=int(g.group(2)), apart=int(g.group(3)),
                       unread=int(g.group(4)), restarts=int(g.group(5) or 0), heap_asked=int(g.group(6) or 0),
                       heap_asked_missing=int(g.group(7) or 0))
        else:
            g = ASKED.search(m)
            if g:
                row.update(asked=int(g.group(1)), asked_missing=int(g.group(2)))
        g = K.EXACT.search(m)
        if g and 'partitions probed' not in m:
            row['members_fetched'] = int(g.group(1))
        g = PLACED.search(m)
        if g:
            row.update(placed=int(g.group(1)), loc_pages=int(g.group(2)), place_failed=int(g.group(3)),
                       checked=int(g.group(4)), toast_asked=int(g.group(5) or 0),
                       toast_asked_missing=int(g.group(6) or 0))
    return row


def answer(conn, query, q):
    return [r[0] for r in conn.execute(query, {'q': K.lit(q)}).fetchall()]


def summarize(rows, gt, kind):
    out = dict(n=len(rows))
    for k in ('ms', 'dev_kb', 'read', 'hit', 'io_ms', 'probes', 'pages', 'streamed', 'stream_read_in', 'apart',
              'unread', 'restarts', 'heap_asked', 'heap_asked_missing', 'asked', 'asked_missing', 'members_fetched',
              'placed', 'loc_pages', 'place_failed', 'checked', 'toast_asked', 'toast_asked_missing'):
        v = [r[k] for r in rows if k in r]
        if v:
            out[k + '_median'] = float(np.median(v))
            out[k + '_mean'] = float(np.mean(v))
    ms = [r['ms'] for r in rows]
    out['ms_p25'], out['ms_p75'] = float(np.percentile(ms, 25)), float(np.percentile(ms, 75))
    out['all_ms'] = [round(x, 2) for x in ms]
    out['all_read'] = [r['read'] for r in rows]
    out['all_io_ms'] = [round(r['io_ms'], 2) for r in rows]
    rec = [len(set(r['ids']) & set(gt[r['qi']].tolist())) / 10 for r in rows]
    out['recall'] = float(np.mean(rec))
    return out


def main(name):
    cfgs = configs()
    states = os.environ.get('KNN_RA_STATES', 'device,os,warm').split(',')
    kinds = os.environ.get('KNN_RA_KINDS', ','.join(KINDS)).split(',')
    nq = int(os.environ.get('KNN_RA_NQ', '10'))
    reps = int(os.environ.get('KNN_RA_WARM_REPS', '3'))
    X = np.load(os.path.join(K.CACHE, 'X.npy'), mmap_mode='r')
    qidx = np.load(os.path.join(K.CACHE, 'qidx.npy'))
    Q = np.asarray(X[qidx[:nq]])
    truth = dict(np.load(os.path.join(K.CACHE, 'truth.npz')))
    conn, _ = restart(cfgs[0]['lib'], False)
    settings = K.show(conn, 'server_version', 'shared_buffers', 'work_mem', 'random_page_cost',
                      'effective_io_concurrency', 'maintenance_io_concurrency', 'effective_cache_size',
                      'io_method', 'io_workers', 'io_combine_limit', 'max_parallel_workers_per_gather',
                      'jit', 'fsync', 'max_connections')
    others = K.only_index(conn, K.INDEXES['lionvec'])
    conn.close()
    meta = dict(name=name, settings=settings, queries=nq, warm_reps=reps, kinds={k: KINDS[k] for k in kinds},
                configs=[dict(c, so_sha256=sha(os.path.join(SO_DIR, c['lib'] + '.so'))) for c in cfgs],
                restart=RESTART, drop=DROP, run_at=time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime()),
                sizes=K.sizes())
    results = []
    raw = []                          # every query's own row
    ids = {}                          # (kind, qi) -> the first configuration's rows
    mismatches = []
    try:
        for state in states:
            for kind in kinds:
                sel = KINDS[kind][0]
                gt = truth[str(sel)]
                per = {c['label']: [] for c in cfgs}
                if state in ('device', 'os'):
                    if state == 'os':
                        conn, _ = restart(cfgs[0]['lib'], False)
                        os_cache(conn)
                        conn.close()
                    for qi in range(nq):
                        for c in cfgs:
                            conn, dropped = restart(c['lib'], state == 'device')
                            query = prepare_session(conn, c, kind)
                            r = measure(conn, query, Q[qi])
                            r.update(qi=qi, ids=answer(conn, query, Q[qi]), dropped=dropped)
                            per[c['label']].append(r)
                            conn.close()
                else:
                    # the configurations of one library in one session, taking
                    # turns query by query and repetition by repetition, so
                    # that what a restart or a stretch of the machine costs
                    # falls on all of them
                    libs = []
                    for c in cfgs:
                        if c['lib'] not in libs:
                            libs.append(c['lib'])
                    for lib in libs:
                        group = [c for c in cfgs if c['lib'] == lib]
                        conn, _ = restart(lib, False)
                        K.prewarm(conn, K.INDEXES['lionvec'])
                        query = prepare_session(conn, group[0], kind)
                        for c in group:
                            apply(conn, c)
                            for qi in range(nq):
                                answer(conn, query, Q[qi])
                        for qi in range(nq):
                            rs = {c['label']: [] for c in group}
                            for _ in range(reps):
                                for c in group:
                                    apply(conn, c)
                                    rs[c['label']].append(measure(conn, query, Q[qi]))
                            for c in group:
                                apply(conn, c)
                                r = dict(rs[c['label']][0])
                                for k in ('ms', 'io_ms', 'read', 'dev_kb'):
                                    r[k] = float(np.median([x[k] for x in rs[c['label']]]))
                                r.update(qi=qi, ids=answer(conn, query, Q[qi]))
                                per[c['label']].append(r)
                        conn.close()
                for c in cfgs:
                    for r in per[c['label']]:
                        raw.append(dict(r, state=state, kind=kind, config=c['label']))
                        key = (kind, r['qi'])
                        if key not in ids:
                            ids[key] = (c['label'], r['ids'])
                        elif ids[key][1] != r['ids']:
                            mismatches.append(dict(state=state, kind=kind, qi=r['qi'], config=c['label'],
                                                   against=ids[key][0]))
                    s = summarize(per[c['label']], gt, kind)
                    s.update(state=state, kind=kind, sel=sel, config=c['label'])
                    results.append(s)
                    print(json.dumps({k: v for k, v in s.items() if not k.startswith('all_')}), flush=True)
    finally:
        conn, _ = restart(cfgs[0]['lib'], False)
        K.restore_indexes(conn, others)
        conn.close()
    meta['mismatches'] = mismatches
    os.makedirs(K.OUT, exist_ok=True)
    json.dump(dict(meta=meta, results=results, raw=raw), open(os.path.join(K.OUT, f'{name}.json'), 'w'), indent=1)
    print(f'{len(mismatches)} answers differed between configurations', flush=True)


KIND_NAMES = {'u3': 'unfiltered, margin 3', 'u8': 'unfiltered, margin 8', 'f10': '10%, probed',
              'f3': '3%, probed', 'f1': '1%, probed', 'x1': '1%, exact', 'x3': '3%, exact'}


def table(files):
    # the results as markdown: for each file (an io_method) and state, each
    # kind's configurations side by side - the server's median ms (the
    # quartiles), the pages read from outside shared buffers, the wait on
    # them and what the disk read
    for f in files:
        d = json.load(open(f))
        st = d['meta']['settings']
        print(f"### {os.path.basename(f)}: io_method {st.get('io_method')}, io_workers {st.get('io_workers')}\n")
        cfgs = [c['label'] for c in d['meta']['configs']]
        for state in ('device', 'os', 'warm'):
            rows = [r for r in d['results'] if r['state'] == state]
            if not rows:
                continue
            print(f'**{state}**\n')
            print('| Query | ' + ' | '.join(cfgs) + ' |')
            print('| --- |' + ' --- |' * len(cfgs))
            for kind in KINDS:
                by = {r['config']: r for r in rows if r['kind'] == kind}
                if not by:
                    continue
                cells = []
                for c in cfgs:
                    r = by.get(c)
                    if r is None:
                        cells.append('')
                        continue
                    cell = f"{r['ms_median']:.1f} ({r['ms_p25']:.1f}-{r['ms_p75']:.1f})"
                    if state != 'warm':
                        cell += f", {r['read_median']:.0f} p, {r['io_ms_median']:.1f} w"
                        if state == 'device':
                            cell += f", {r['dev_kb_median'] / 1024:.1f} MB"
                    cells.append(cell)
                rec = next(iter(by.values()))['recall']
                print(f'| {KIND_NAMES.get(kind, kind)} ({rec:.3f}) | ' + ' | '.join(cells) + ' |')
            print()
            ratios(d, state, cfgs)


def ratios(d, state, cfgs):
    # each query's time against its own under the base configuration (off,
    # or the first), the median of those ratios and their quartiles: the
    # queries differ far more from one another than the configurations do,
    # so a ratio taken query by query shows what the medians above blur
    # a configuration whose library has an off of its own, in its session
    # (warm), is taken against that
    raw = [r for r in d.get('raw', []) if r['state'] == state]
    if not raw:
        return
    base = 'off' if 'off' in cfgs else cfgs[0]
    lib = {c['label']: c['lib'] for c in d['meta']['configs']}
    mode = {c['label']: c['mode'] for c in d['meta']['configs']}
    bases = {}
    for c in cfgs:
        own = [o for o in cfgs if o != c and lib[o] == lib[c] and mode[o] == 'off']
        bases[c] = own[0] if own and state == 'warm' else base
    others = [c for c in cfgs if c != base]
    print(f'*each query against itself under {base}, or its own library\'s off where named: '
          'the median ratio (quartiles)*\n')
    print('| Query | ' + ' | '.join(c if bases[c] == base else f'{c} / {bases[c]}' for c in others) + ' |')
    print('| --- |' + ' --- |' * len(others))
    for kind in KINDS:
        ms = {}
        for r in raw:
            if r['kind'] == kind:
                ms[(r['config'], r['qi'])] = r['ms']
        if not ms:
            continue
        cells = []
        for c in others:
            b = bases[c]
            v = [ms[(c, qi)] / ms[(b, qi)] for (cc, qi) in ms
                 if cc == c and (b, qi) in ms and ms[(b, qi)] > 0]
            if v:
                cells.append(f'{np.median(v):.2f} ({np.percentile(v, 25):.2f}-{np.percentile(v, 75):.2f})')
            else:
                cells.append('')
        print(f'| {KIND_NAMES.get(kind, kind)} | ' + ' | '.join(cells) + ' |')
    print()


if __name__ == '__main__':
    if len(sys.argv) > 1 and sys.argv[1] == 'table':
        table(sys.argv[2:])
    else:
        main(sys.argv[1] if len(sys.argv) > 1 else 'readahead')
