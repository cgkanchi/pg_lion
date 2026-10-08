#!/usr/bin/env python3
"""The decision matrix of lion's cost model (DESIGN.md §31, §39).

For each query of a fixed set over synthetic tables (setup.sql: §31's
shapes), plans it under every ARM - the planner's own choice, lion's paths
forced, lion's paths off, and each core alternative forced with core's
enable_* settings - keeps each distinct plan once, and times each with
EXPLAIN (ANALYZE, TIMING OFF): the median of --repeats runs after --warmup,
interleaved round by round, each in its own transaction on one connection.
It prints, as Markdown:

  - per query: the plan the planner chose, the fastest plan, and whether the
    choice is a MISPICK - more than 15% and more than 0.05 ms slower than the
    fastest (§31's rule);
  - per category: mispicks, time lost, the chosen plans' total against the
    fastest plans' total;
  - per kind of plan: cost units a millisecond (EXPLAIN's total cost over the
    measured time), the number §10's "The reference" and §39's rates are made
    of;
  - for each --sweep NAME=V1,V2,..: the planner's choices with that setting
    at each value, and their mispicks (plans not yet timed are timed).

Every arm and sweep plans with max_parallel_workers_per_gather = 0 and JIT
off unless --parallel or --jit is given: serial plans, as §31 measured.

The script connects with psql and libpq's environment (PGHOST, PGPORT,
PGUSER, PGDATABASE, or --dbname); it needs only Python's standard library.

  python3 bench/calib/matrix.py --setup --rows 2000000      # build the tables
  python3 bench/calib/matrix.py --out after.json            # run, print
  python3 bench/calib/matrix.py --out own.json \\
      --set pg_lion.pushdown_margin=1 --set pg_lion.hashagg_rate=1 ...
                                              # lion's own units a millisecond
  python3 bench/calib/matrix.py compare before.json after.json

A run against a build without §39's settings (the "before" of DESIGN.md
§39's tables) skips the settings and sweeps it does not have.
"""
import argparse
import json
import os
import re
import statistics
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent

# ---------------------------------------------------------------- the queries

IN100 = ', '.join(str(v) for v in range(0, 20000, 200))
IN1000 = ', '.join(str(v) for v in range(0, 1000000, 1000))

QUERIES = [
    # eq: counts under equality, IN, AND, NULL, skew
    ('eq', 'eq.c2', 'SELECT count(*) FROM calib_s WHERE c2 = 1'),
    ('eq', 'eq.c20', 'SELECT count(*) FROM calib_s WHERE c20 = 3'),
    ('eq', 'eq.c200', 'SELECT count(*) FROM calib_s WHERE c200 = 17'),
    ('eq', 'eq.c20k', 'SELECT count(*) FROM calib_s WHERE c20k = 77'),
    ('eq', 'eq.c1m', 'SELECT count(*) FROM calib_s WHERE c1m = 4242'),
    ('eq', 'eq.c200.c20', 'SELECT count(*) FROM calib_s WHERE c200 = 17 AND c20 = 3'),
    ('eq', 'eq.c20k.c200.c2',
     'SELECT count(*) FROM calib_s WHERE c20k = 77 AND c200 = 17 AND c2 = 1'),
    ('eq', 'eq.c20k.in100', 'SELECT count(*) FROM calib_s WHERE c20k IN (%s)' % IN100),
    ('eq', 'eq.c1m.in1000', 'SELECT count(*) FROM calib_s WHERE c1m IN (%s)' % IN1000),
    ('eq', 'eq.skew.hot', 'SELECT count(*) FROM calib_s WHERE skew = 0'),
    ('eq', 'eq.skew.cold', 'SELECT count(*) FROM calib_s WHERE skew = 7'),
    ('eq', 'eq.n10.null', 'SELECT count(*) FROM calib_s WHERE n10 IS NULL'),
    ('eq', 'eq.cl200', 'SELECT count(*) FROM calib_s WHERE cl200 = 50'),
    ('eq', 'eq.c200.in2.c2', 'SELECT count(*) FROM calib_s WHERE c200 IN (17, 18) AND c2 = 1'),
    # range
    ('range', 'range.cl200', 'SELECT count(*) FROM calib_s WHERE cl200 BETWEEN 50 AND 59'),
    ('range', 'range.c20k', 'SELECT count(*) FROM calib_s WHERE c20k BETWEEN 100 AND 199'),
    ('range', 'range.c20k.c2',
     'SELECT count(*) FROM calib_s WHERE c20k BETWEEN 100 AND 1099 AND c2 = 1'),
    # group: GROUP BY, count(DISTINCT), the top k
    ('group', 'group.c20', 'SELECT c20, count(*) FROM calib_s GROUP BY c20'),
    ('group', 'group.c200', 'SELECT c200, count(*) FROM calib_s GROUP BY c200'),
    ('group', 'group.c20k', 'SELECT c20k, count(*) FROM calib_s GROUP BY c20k'),
    ('group', 'group.c200.w.c2', 'SELECT c200, count(*) FROM calib_s WHERE c2 = 1 GROUP BY c200'),
    ('group', 'group.c20k.w.c20',
     'SELECT c20k, count(*) FROM calib_s WHERE c20 = 3 GROUP BY c20k'),
    ('group', 'group.c20.c2', 'SELECT c20, c2, count(*) FROM calib_s GROUP BY c20, c2'),
    ('group', 'group.c200.c20', 'SELECT c200, c20, count(*) FROM calib_s GROUP BY c200, c20'),
    ('group', 'distinct.c20k.w.c200',
     'SELECT count(DISTINCT c20k) FROM calib_s WHERE c200 = 17'),
    ('group', 'distinct.c200', 'SELECT count(DISTINCT c200) FROM calib_s'),
    ('group', 'topk.c20k',
     'SELECT c20k, count(*) FROM calib_s GROUP BY c20k ORDER BY count(*) DESC LIMIT 10'),
    # fk: the FK-side join, semi and anti joins
    ('fk', 'fk.status',
     'SELECT count(*) FROM calib_f f JOIN calib_d d ON f.fk = d.pk WHERE d.status = 2'),
    ('fk', 'fk.status.country',
     'SELECT count(*) FROM calib_f f JOIN calib_d d ON f.fk = d.pk '
     'WHERE d.status = 2 AND d.country = 7'),
    ('fk', 'fk.status.kind',
     'SELECT count(*) FROM calib_f f JOIN calib_d d ON f.fk = d.pk '
     'WHERE d.status = 2 AND f.kind = 3'),
    ('fk', 'fk.group.country',
     'SELECT d.country, count(*) FROM calib_f f JOIN calib_d d ON f.fk = d.pk '
     'WHERE f.kind = 3 GROUP BY d.country'),
    ('fk', 'fk.small.grp',
     'SELECT count(*) FROM calib_f f JOIN calib_dk k ON f.fk2 = k.pk WHERE k.grp = 3'),
    ('fk', 'fk.semi.country',
     'SELECT d.pk FROM calib_d d WHERE d.country = 7 AND EXISTS '
     '(SELECT 1 FROM calib_f f WHERE f.fk = d.pk AND f.kind = 3)'),
    ('fk', 'fk.semi.count',
     'SELECT count(*) FROM calib_d d WHERE EXISTS '
     '(SELECT 1 FROM calib_f f WHERE f.fk = d.pk AND f.kind = 3 AND f.x = 1)'),
    ('fk', 'fk.anti.country',
     'SELECT d.pk FROM calib_d d WHERE d.country = 7 AND NOT EXISTS '
     '(SELECT 1 FROM calib_f f WHERE f.fk = d.pk AND f.kind = 3)'),
    # ordered: ORDER BY ... LIMIT under a lion filter
    ('ordered', 'ord.c200', 'SELECT id FROM calib_s WHERE c200 = 17 ORDER BY k, id LIMIT 10'),
    ('ordered', 'ord.c20k', 'SELECT id FROM calib_s WHERE c20k = 77 ORDER BY k, id LIMIT 10'),
    ('ordered', 'ord.c200.c2',
     'SELECT id FROM calib_s WHERE c200 = 17 AND c2 = 1 ORDER BY k, id LIMIT 25'),
    ('ordered', 'ord.c2', 'SELECT id FROM calib_s WHERE c2 = 1 ORDER BY k, id LIMIT 100'),
]

# ---------------------------------------------------------------- the arms

LION_OFF = {'pg_lion.enable_count_pushdown': 'off',
            'pg_lion.enable_semijoin': 'off',
            'pg_lion.enable_ordered_scan': 'off'}
SCANS = ['enable_seqscan', 'enable_bitmapscan', 'enable_indexscan',
         'enable_indexonlyscan']
JOINS = ['enable_hashjoin', 'enable_mergejoin', 'enable_nestloop']


def off(*names):
    return {n: 'off' for n in names}


def arm(*parts):
    d = {}
    for p in parts:
        d.update(p)
    return d


ARMS = {
    'chosen': {},
    # every core scan and join method disabled: what is left that is not is
    # lion's path, where the query has one
    'lion': arm(off(*SCANS), off(*JOINS), off('enable_tidscan')),
    'core': arm(LION_OFF),
    'seq': arm(LION_OFF, off('enable_bitmapscan', 'enable_indexscan',
                             'enable_indexonlyscan')),
    'bitmap': arm(LION_OFF, off('enable_seqscan', 'enable_indexscan',
                                'enable_indexonlyscan')),
    'index': arm(LION_OFF, off('enable_seqscan', 'enable_bitmapscan')),
    'hashagg': arm(LION_OFF, off('enable_sort')),
    'sortagg': arm(LION_OFF, off('enable_hashagg')),
    'hashjoin': arm(LION_OFF, off('enable_mergejoin', 'enable_nestloop')),
    'mergejoin': arm(LION_OFF, off('enable_hashjoin', 'enable_nestloop')),
    'nestloop': arm(LION_OFF, off('enable_hashjoin', 'enable_mergejoin')),
}

PASS_THROUGH = {'Limit', 'Sort', 'Incremental Sort', 'Gather', 'Gather Merge',
                'Result', 'Materialize', 'Memoize', 'Unique', 'Subquery Scan',
                'Hash', 'ProjectSet', 'WindowAgg', 'LockRows'}

# ---------------------------------------------------------------- psql


class Psql:
    def __init__(self, args):
        self.cmd = [args.psql, '-X', '-q', '-A', '-t', '-v', 'ON_ERROR_STOP=1']
        if args.dbname:
            self.cmd += ['-d', args.dbname]

    def run(self, script, timeout=None):
        p = subprocess.run(self.cmd + ['-f', '-'], input=script, text=True,
                           capture_output=True, timeout=timeout)
        if p.returncode != 0:
            raise RuntimeError('psql failed:\n%s\n--- script ---\n%s'
                               % (p.stderr, script[:2000]))
        return p.stdout

    def tagged(self, script):
        """Split psql's output at the lines \\echo @@<tag> printed."""
        out = {}
        tag = None
        buf = []
        for line in self.run(script).splitlines():
            if line.startswith('@@'):
                if tag is not None:
                    out.setdefault(tag, []).append('\n'.join(buf))
                tag = line[2:]
                buf = []
            else:
                buf.append(line)
        if tag is not None:
            out.setdefault(tag, []).append('\n'.join(buf))
        return out


def sql_literal(v):
    return "'" + str(v).replace("'", "''") + "'"


def set_local(settings):
    return ''.join('SET LOCAL %s = %s;\n' % (k, sql_literal(v))
                   for k, v in settings.items())

# ---------------------------------------------------------------- plans


class Catalog:
    def __init__(self, psql):
        out = psql.run("SELECT c.relname FROM pg_class c JOIN pg_am a ON a.oid = c.relam "
                       "WHERE a.amname = 'lion';\n")
        self.lion_indexes = set(out.split())
        # the library defines its settings when it is loaded
        out = psql.run("SET client_min_messages = warning;\nLOAD 'pg_lion';\n"
                       "SELECT name FROM pg_settings WHERE name LIKE 'pg_lion.%';\n")
        self.lion_settings = set(out.split())
        self.version = psql.run('SHOW server_version_num;\n').strip()

    def has(self, name):
        return not name.startswith('pg_lion.') or name in self.lion_settings

    def usable(self, settings):
        return {k: v for k, v in settings.items() if self.has(k)}


def node_name(n, cat):
    t = n['Node Type']
    if t == 'Custom Scan':
        return n.get('Custom Plan Provider', 'Custom')
    if t == 'Aggregate':
        t = {'Hashed': 'HashAggregate', 'Sorted': 'GroupAggregate',
             'Mixed': 'MixedAggregate'}.get(n.get('Strategy'), 'Aggregate')
        if n.get('Partial Mode', 'Simple') != 'Simple':
            t = n['Partial Mode'] + ' ' + t
    if t in ('Hash Join', 'Merge Join', 'Nested Loop') and n.get('Join Type') not in (None, 'Inner'):
        t += ' ' + n['Join Type']
    idx = n.get('Index Name')
    if idx is not None:
        t += ' on %s%s' % (idx, ' (lion)' if idx in cat.lion_indexes else '')
    elif 'Relation Name' in n and t != 'Custom Scan':
        t += ' on ' + n['Relation Name']
    return t


def signature(n, cat):
    kids = n.get('Plans', [])
    s = node_name(n, cat)
    if kids:
        s += ' (' + ', '.join(signature(k, cat) for k in kids) + ')'
    return s


def lion_node(n):
    if n['Node Type'] == 'Custom Scan' and n.get('Custom Plan Provider', '').startswith('Lion'):
        return n['Custom Plan Provider']
    for k in n.get('Plans', []):
        r = lion_node(k)
        if r:
            return r
    return None


def kind(n, cat):
    """What a plan's units a millisecond are the units of: its topmost lion
    node, or else the first node from the top that says (§39's kinds)."""
    lion = lion_node(n)
    if lion:
        return lion
    agg = False
    while True:
        t = n['Node Type']
        kids = n.get('Plans', [])
        if t == 'Aggregate':
            if n.get('Strategy') in ('Hashed', 'Mixed'):
                return 'HashAggregate'
            agg = True
        elif t == 'Group':
            agg = True
        elif t in ('Hash Join', 'Merge Join', 'Nested Loop'):
            return t
        elif t in ('Bitmap Heap Scan', 'Seq Scan', 'Index Scan', 'Index Only Scan'):
            name = t
            idx = n.get('Index Name')
            if t == 'Bitmap Heap Scan':
                idxs = [k.get('Index Name') for k in walk(n)]
                if any(i in cat.lion_indexes for i in idxs if i):
                    name += ' (lion)'
            elif idx in cat.lion_indexes:
                name += ' (lion)'
            return ('Aggregate over ' + name) if agg else name
        elif t not in PASS_THROUGH:
            return t
        if not kids:
            return t
        n = kids[0]


def walk(n):
    yield n
    for k in n.get('Plans', []):
        yield from walk(k)


def plan_cost(p):
    c = float(p['Plan']['Total Cost'])
    # disable_cost before PostgreSQL 18: a plan's own cost is what is left
    if c >= 1e10:
        c = c % 1e10
    return c


def plan_all(psql, cat, base, q, arms):
    script = base
    for name, settings in arms.items():
        script += 'BEGIN;\n' + set_local(cat.usable(settings))
        script += '\\echo @@%s\nEXPLAIN (FORMAT JSON) %s;\nROLLBACK;\n' % (name, q)
    out = psql.tagged(script)
    res = {}
    for name in arms:
        p = json.loads(out[name][0])[0]
        res[name] = (signature(p['Plan'], cat), plan_cost(p), kind(p['Plan'], cat))
    return res


def time_plans(psql, cat, base, q, plans, warmup, repeats):
    """plans: {sig: settings}; returns {sig: median ms} and checks that each
    run's plan is the plan it stands for."""
    script = base
    for r in range(warmup + repeats):
        for i, (sig, settings) in enumerate(plans.items()):
            script += 'BEGIN;\n' + set_local(cat.usable(settings))
            script += ('\\echo @@%d.%d\nEXPLAIN (ANALYZE, TIMING OFF, FORMAT JSON) %s;\n'
                       'ROLLBACK;\n' % (i, r, q))
    out = psql.tagged(script)
    res = {}
    for i, sig in enumerate(plans):
        ms = []
        for r in range(warmup, warmup + repeats):
            p = json.loads(out['%d.%d' % (i, r)][0])[0]
            got = signature(p['Plan'], cat)
            if got != sig:
                sys.stderr.write('warning: %s planned as %s, timed as %s\n' % (q[:60], sig, got))
            ms.append(float(p['Execution Time']))
        res[sig] = statistics.median(ms)
    return res

# ---------------------------------------------------------------- the run


def mispick(chosen_ms, best_ms):
    return chosen_ms > best_ms * 1.15 and chosen_ms - best_ms > 0.05


def load_queries(path):
    """Another query set: a Python file that defines QUERIES as above."""
    env = {}
    exec(compile(Path(path).read_text(), str(path), 'exec'), env)
    return env['QUERIES']


def run(args):
    global QUERIES
    psql = Psql(args)
    if args.queries:
        QUERIES = load_queries(args.queries)
    if args.setup:
        script = '\\set rows %d\n\\set dirty %d\n' % (args.rows, args.dirty)
        script += Path(args.setup_file or HERE / 'setup.sql').read_text()
        t0 = time.time()
        psql.run(script)
        print('setup: %d rows, %d%% dirty, %.0f s' % (args.rows, args.dirty, time.time() - t0),
              file=sys.stderr)
        if args.setup_only:
            return
    cat = Catalog(psql)
    base = "SET client_min_messages = warning;\nLOAD 'pg_lion';\n"
    if not args.parallel:
        base += 'SET max_parallel_workers_per_gather = 0;\n'
    if not args.jit:
        base += 'SET jit = off;\n'
    extra = {}
    for s in args.set:
        k, v = s.split('=', 1)
        extra[k] = v
    for k, v in cat.usable(extra).items():
        base += 'SET %s = %s;\n' % (k, sql_literal(v))
    sweeps = []
    for s in args.sweep:
        k, vs = s.split('=', 1)
        if cat.has(k):
            sweeps.append((k, vs.split(',')))
        else:
            print('sweep %s: no such setting in this build, skipped' % k, file=sys.stderr)
    pat = re.compile(args.only) if args.only else None

    result = {'version': cat.version, 'set': extra, 'repeats': args.repeats,
              'label': args.label, 'queries': []}
    for cat_name, qid, q in QUERIES:
        if pat and not pat.search(qid):
            continue
        arms = {n: arm(s, extra) for n, s in ARMS.items()}
        planned = plan_all(psql, cat, base, q, arms)
        plans = {}
        info = {}
        for name, (sig, cost, k) in planned.items():
            if sig not in plans:
                plans[sig] = arms[name]
                info[sig] = {'cost': cost, 'kind': k, 'arms': [name]}
            else:
                info[sig]['arms'].append(name)
        # the sweeps' choices
        swept = {}
        for k, vals in sweeps:
            for v in vals:
                s = arm(extra, {k: v})
                sig, cost, kd = plan_all(psql, cat, base, q, {'x': s})['x']
                swept['%s=%s' % (k, v)] = sig
                if sig not in plans:
                    plans[sig] = s
                    info[sig] = {'cost': cost, 'kind': kd, 'arms': ['%s=%s' % (k, v)]}
        times = time_plans(psql, cat, base, q, plans, args.warmup, args.repeats)
        for sig, ms in times.items():
            info[sig]['ms'] = ms
        chosen = planned['chosen'][0]
        best = min(times, key=times.get)
        rec = {'category': cat_name, 'id': qid, 'query': q, 'chosen': chosen,
               'best': best, 'plans': info, 'swept': swept}
        result['queries'].append(rec)
        print('%-22s %-40s %9.3f ms  fastest %9.3f ms%s' % (
            qid, info[chosen]['kind'][:40], times[chosen], times[best],
            '  MISPICK' if mispick(times[chosen], times[best]) else ''), file=sys.stderr)
    if args.out:
        Path(args.out).write_text(json.dumps(result, indent=1))
    report(result, sweeps)


def chosen_at(rec, key):
    return rec['chosen'] if key is None else rec['swept'].get(key, rec['chosen'])


def tally(recs, key=None):
    n = lost = chosen_total = best_total = 0
    for rec in recs:
        sig = chosen_at(rec, key)
        c = rec['plans'][sig]['ms']
        b = rec['plans'][rec['best']]['ms']
        chosen_total += c
        best_total += b
        if mispick(c, b):
            n += 1
            lost += c - b
    return n, lost, chosen_total, best_total


def report(result, sweeps):
    recs = result['queries']
    print('\n### Per query (%s, PostgreSQL %s)\n' % (result.get('label') or 'this build',
                                                    result['version']))
    print('| query | chosen | cost | ms | fastest | ms | |')
    print('|---|---|---|---|---|---|---|')
    for rec in recs:
        c = rec['plans'][rec['chosen']]
        b = rec['plans'][rec['best']]
        print('| `%s` | %s | %.0f | %.3f | %s | %.3f | %s |' % (
            rec['id'], c['kind'], c['cost'], c['ms'],
            'same' if rec['best'] == rec['chosen'] else b['kind'], b['ms'],
            '**mispick**' if mispick(c['ms'], b['ms']) else ''))
    print('\n### Mispicks\n')
    print('| category | queries | mispicks | time lost, ms | chosen total, ms | fastest total, ms |')
    print('|---|---|---|---|---|---|')
    cats = sorted(set(r['category'] for r in recs), key=lambda c: [q[0] for q in QUERIES].index(c))
    for c in cats + ['all']:
        rs = recs if c == 'all' else [r for r in recs if r['category'] == c]
        n, lost, ct, bt = tally(rs)
        print('| %s | %d | %d | %.1f | %.1f | %.1f |' % (c, len(rs), n, lost, ct, bt))
    print('\n### Cost units a millisecond, by kind of plan\n')
    print('| kind | plans | median | min | max |')
    print('|---|---|---|---|---|')
    by = {}
    for rec in recs:
        for sig, p in rec['plans'].items():
            if p.get('ms', 0) > 0:
                by.setdefault(p['kind'], []).append(p['cost'] / p['ms'])
    for k in sorted(by, key=lambda k: -len(by[k])):
        v = by[k]
        print('| %s | %d | %.0f | %.0f | %.0f |' % (k, len(v), statistics.median(v), min(v), max(v)))
    if sweeps:
        print('\n### Sweeps\n')
        print('| setting | mispicks | time lost, ms | chosen total, ms |')
        print('|---|---|---|---|')
        for k, vals in sweeps:
            for v in vals:
                key = '%s=%s' % (k, v)
                n, lost, ct, _ = tally(recs, key)
                print('| `%s` | %d | %.1f | %.1f |' % (key, n, lost, ct))


def compare(args):
    a = json.loads(Path(args.files[0]).read_text())
    b = json.loads(Path(args.files[1]).read_text())
    ra = {r['id']: r for r in a['queries']}
    rb = {r['id']: r for r in b['queries']}
    ids = [r['id'] for r in a['queries'] if r['id'] in rb]
    print('| category | queries | mispicks, time lost (%s) | mispicks, time lost (%s) |'
          % (a.get('label') or args.files[0], b.get('label') or args.files[1]))
    print('|---|---|---|---|')
    cats = []
    for i in ids:
        if ra[i]['category'] not in cats:
            cats.append(ra[i]['category'])
    for c in cats + ['all']:
        sel = [i for i in ids if c == 'all' or ra[i]['category'] == c]
        na, la, cta, _ = tally([ra[i] for i in sel])
        nb, lb, ctb, _ = tally([rb[i] for i in sel])
        print('| %s | %d | %d, %.1f ms (chosen %.1f) | %d, %.1f ms (chosen %.1f) |'
              % (c, len(sel), na, la, cta, nb, lb, ctb))
    print('\n| query | before | ms | after | ms | fastest, ms |')
    print('|---|---|---|---|---|---|')
    for i in ids:
        pa = ra[i]['plans'][ra[i]['chosen']]
        pb = rb[i]['plans'][rb[i]['chosen']]
        if ra[i]['chosen'] != rb[i]['chosen']:
            best = min(ra[i]['plans'][ra[i]['best']]['ms'], rb[i]['plans'][rb[i]['best']]['ms'])
            print('| `%s` | %s | %.3f | %s | %.3f | %.3f |'
                  % (i, pa['kind'], pa['ms'], pb['kind'], pb['ms'], best))


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('files', nargs='*', help='compare A.json B.json')
    ap.add_argument('--psql', default='psql')
    ap.add_argument('--dbname')
    ap.add_argument('--setup', action='store_true', help='build the tables first (setup.sql)')
    ap.add_argument('--setup-only', action='store_true')
    ap.add_argument('--setup-file', help='another setup script (default: setup.sql)')
    ap.add_argument('--queries', help='another query set: a Python file defining QUERIES '
                    '(heldout.py: tables and queries the rates were not fitted on)')
    ap.add_argument('--rows', type=int, default=2000000)
    ap.add_argument('--dirty', type=int, default=0,
                    help='percent of calib_s rows updated after the VACUUM')
    ap.add_argument('--repeats', type=int, default=5)
    ap.add_argument('--warmup', type=int, default=1)
    ap.add_argument('--only', help='regex over query ids')
    ap.add_argument('--set', action='append', default=[], metavar='NAME=VALUE',
                    help='a setting for every plan and arm')
    ap.add_argument('--sweep', action='append', metavar='NAME=V1,V2,..',
                    help='the choices at each value (default: pg_lion.pushdown_margin '
                    '1 to 0.5, where the build has it)')
    ap.add_argument('--parallel', action='store_true')
    ap.add_argument('--jit', action='store_true')
    ap.add_argument('--label')
    ap.add_argument('--out', help='write the results as JSON')
    args = ap.parse_args()
    if args.sweep is None:
        args.sweep = ['pg_lion.pushdown_margin=1,0.9,0.8,0.7,0.6,0.5']
    if args.files:
        if args.files[0] != 'compare' or len(args.files) != 3:
            ap.error('usage: matrix.py compare A.json B.json')
        args.files = args.files[1:]
        compare(args)
        return
    if args.setup_only:
        args.setup = True
    run(args)


if __name__ == '__main__':
    main()
