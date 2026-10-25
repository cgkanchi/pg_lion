#!/usr/bin/env python3
"""Lion's per-operation costs, measured against core's (DESIGN.md §49).

Each lion-only operation the planner prices is timed against a core
operation whose cost core states, on the same build, server and tables, and
printed as a multiple of the core cost setting it is written in, beside what
lion charges for it now.  A ratio of two operations on one machine carries
to another far better than a rate of cost units a millisecond, which is the
machine's: this is what "calibrated against core" means for a constant.

  anchor  cpu_operator_cost: the slope of a cached sequential scan's time
          over the number of `a <> -k` quals it evaluates a row - each an
          int4ne of procost 1, one cpu_operator_cost a row to core.
  probe   a nested loop probe's own work an outer row (DESIGN.md §40.10,
          §49): the heap probe over a set that passes every row, against
          core's Index Scan of the same B-tree with no filter - the same
          descents and the same heap fetches, the probe's set test and its
          rescans apart.  The slope over the outer rows, so that the set's
          build, made once, stays out of it.

Every query runs serial, JIT off, after a warm-up, interleaved round by
round, each in its own transaction on one connection; the time is
EXPLAIN (ANALYZE, TIMING OFF)'s Execution Time, its median over --repeats,
and each run's plan is checked to be the plan it stands for.  Measure on a
release build: an assert build slows lion's code and core's by different
factors.

  python3 bench/calib/ops.py --setup          # build calib_ops_* (a minute)
  python3 bench/calib/ops.py [--repeats 15]   # measure, print Markdown
  python3 bench/calib/ops.py --out ops.json   # ... and keep the numbers

The script connects with psql and libpq's environment (PGHOST, PGPORT,
PGUSER, PGDATABASE, or --dbname); it needs only Python's standard library.
"""
import argparse
import json
import statistics
import subprocess
import sys

SETUP = r"""
DROP TABLE IF EXISTS calib_ops_a, calib_ops_fact, calib_ops_dim;
CREATE EXTENSION IF NOT EXISTS pg_lion;

-- the anchor's table: 2M narrow rows, all-visible, read from shared buffers
CREATE TABLE calib_ops_a (a int NOT NULL, b int NOT NULL)
  WITH (autovacuum_enabled = off);
INSERT INTO calib_ops_a SELECT i, i % 1000 FROM generate_series(1, 2000000) i;

-- the probe's: a dimension of 100,000 rows under a primary key and a lion
-- index on a four-valued column, and 200,000 fact rows over its keys
CREATE TABLE calib_ops_dim (id int PRIMARY KEY, grade int NOT NULL,
							name text NOT NULL, pad text)
  WITH (autovacuum_enabled = off);
INSERT INTO calib_ops_dim
SELECT i, i % 4, 'd' || i, repeat(md5(i::text), 4)
  FROM generate_series(1, 100000) i;
CREATE INDEX calib_ops_dim_grade ON calib_ops_dim USING lion (grade);
CREATE TABLE calib_ops_fact (id int PRIMARY KEY, dim_id int NOT NULL)
  WITH (autovacuum_enabled = off);
INSERT INTO calib_ops_fact
SELECT i, 1 + ((hashint8extended(i::bigint, 7) & 9223372036854775807) % 100000)::int
  FROM generate_series(1, 200000) i;
VACUUM (FREEZE, ANALYZE) calib_ops_a, calib_ops_dim, calib_ops_fact;
"""

BASE = """SET max_parallel_workers_per_gather = 0;
SET jit = off;
"""

ANCHOR_QUALS = [0, 8, 16, 32]
PROBE_LOOPS = [50000, 100000, 200000]


class Psql:
    def __init__(self, args):
        self.cmd = [args.psql, '-X', '-q', '-A', '-t', '-v', 'ON_ERROR_STOP=1']
        if args.dbname:
            self.cmd += ['-d', args.dbname]

    def run(self, script):
        p = subprocess.run(self.cmd + ['-f', '-'], input=script, text=True,
                           capture_output=True)
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
                    out[tag] = '\n'.join(buf)
                tag = line[2:]
                buf = []
            else:
                buf.append(line)
        if tag is not None:
            out[tag] = '\n'.join(buf)
        return out


def nodes(plan):
    yield plan
    for p in plan.get('Plans', []):
        yield from nodes(p)


def time_queries(psql, cases, warmup, repeats):
    """cases: {name: (settings, query, check)}, check(plan) -> bool; the median
    Execution Time of each, its runs interleaved with the others'."""
    script = BASE
    for r in range(warmup + repeats):
        for name, (settings, q, _) in cases.items():
            script += 'BEGIN;\n%s\\echo @@%s.%d\n' % (settings, name, r)
            script += 'EXPLAIN (ANALYZE, TIMING OFF, FORMAT JSON) %s;\nROLLBACK;\n' % q
    out = psql.tagged(script)
    res = {}
    for name, (_, q, check) in cases.items():
        ms = []
        for r in range(warmup, warmup + repeats):
            p = json.loads(out['%s.%d' % (name, r)])[0]
            if not check(p['Plan']):
                raise RuntimeError('%s did not run the plan it stands for:\n%s'
                                   % (name, json.dumps(p['Plan'], indent=1)[:3000]))
            ms.append(float(p['Execution Time']))
        res[name] = statistics.median(ms)
    return res


def slope(xs, ys):
    """Least-squares slope and intercept."""
    mx = statistics.fmean(xs)
    my = statistics.fmean(ys)
    sxx = sum((x - mx) ** 2 for x in xs)
    sxy = sum((x - mx) * (y - my) for x, y in zip(xs, ys))
    b = sxy / sxx
    return b, my - b * mx


def settings_of(psql, names):
    out = psql.run(''.join("SELECT current_setting('%s');\n" % n for n in names))
    return dict(zip(names, (float(v) for v in out.split())))


def measure(args):
    psql = Psql(args)
    s = settings_of(psql, ['cpu_operator_cost', 'cpu_tuple_cost'])
    nrows = int(psql.run('SELECT count(*) FROM calib_ops_a;').strip())

    # the anchor: ns an int4ne qual a row
    def seqscan(p):
        return any(n['Node Type'] == 'Seq Scan' for n in nodes(p))
    cases = {}
    for k in ANCHOR_QUALS:
        where = ' AND '.join('a <> %d' % -(i + 1) for i in range(k))
        q = 'SELECT count(*) FROM calib_ops_a' + (' WHERE ' + where if k else '')
        cases['q%d' % k] = ('SET LOCAL enable_indexscan = off;\n'
                            'SET LOCAL enable_bitmapscan = off;\n', q, seqscan)
    t = time_queries(psql, cases, args.warmup, args.repeats)
    ms_per_qual, _ = slope(ANCHOR_QUALS, [t['q%d' % k] for k in ANCHOR_QUALS])
    ns_op = ms_per_qual * 1e6 / nrows

    # the probe: its own ns an outer row over core's Index Scan's
    def probe(p):
        return any(n.get('Custom Plan Provider') == 'LionBtreeScan' for n in nodes(p))

    def core(p):
        return any(n['Node Type'] == 'Index Scan' and
                   n.get('Relation Name') == 'calib_ops_dim' for n in nodes(p))
    loopset = ('SET LOCAL enable_hashjoin = off;\nSET LOCAL enable_mergejoin = off;\n'
               'SET LOCAL enable_memoize = off;\n')
    cases = {}
    for n in PROBE_LOOPS:
        q = ('SELECT count(d.name) FROM calib_ops_fact f JOIN calib_ops_dim d '
             'ON d.id = f.dim_id WHERE f.id <= %d' % n)
        cases['p%d' % n] = (loopset + 'SET LOCAL pg_lion.enable_heap_probe = on;\n'
                            'SET LOCAL pg_lion.pushdown_margin = 1000;\n',
                            q + ' AND d.grade IN (0, 1, 2, 3)', probe)
        cases['c%d' % n] = (loopset + 'SET LOCAL pg_lion.enable_btree_scan = off;\n'
                            'SET LOCAL pg_lion.enable_ordered_scan = off;\n', q, core)
    t2 = time_queries(psql, cases, args.warmup, args.repeats)
    d_ms, d_fixed = slope(PROBE_LOOPS, [t2['p%d' % n] - t2['c%d' % n] for n in PROBE_LOOPS])
    c_ms, _ = slope(PROBE_LOOPS, [t2['c%d' % n] for n in PROBE_LOOPS])
    probe_ns = d_ms * 1e6
    charged = 2 * s['cpu_tuple_cost'] + s['cpu_operator_cost']   # LO_PROBE_TUPLES

    res = {
        'anchor': {'rows': nrows, 'ms': t, 'ns_per_cpu_operator_cost': ns_op},
        'probe': {'ms': t2, 'core_ns_per_loop': c_ms * 1e6,
                  'probe_ns_per_loop_over_core': probe_ns,
                  'build_ms': d_fixed,
                  'measured_cpu_operator_costs': probe_ns / ns_op,
                  'charged_cpu_operator_costs': charged / s['cpu_operator_cost']},
    }
    return res


def report(res):
    a = res['anchor']
    p = res['probe']
    print('## Anchor\n')
    print('| quals a row | ms |')
    print('|---|---|')
    for k, v in sorted(a['ms'].items(), key=lambda kv: int(kv[0][1:])):
        print('| %s | %.2f |' % (k[1:], v))
    print('\n%.2f ns a qual a row over %d rows: one cpu_operator_cost.\n'
          % (a['ns_per_cpu_operator_cost'], a['rows']))
    print('## Probe\n')
    print('| outer rows | heap probe ms | core Index Scan ms |')
    print('|---|---|---|')
    for n in PROBE_LOOPS:
        print('| %d | %.2f | %.2f |' % (n, p['ms']['p%d' % n], p['ms']['c%d' % n]))
    print('\ncore: %.0f ns an outer row; the probe %+.1f ns an outer row over it '
          '(and %.2f ms once, its set);' % (p['core_ns_per_loop'],
                                            p['probe_ns_per_loop_over_core'],
                                            p['build_ms']))
    print('measured %.1f cpu_operator_costs an entry, charged %.1f.'
          % (p['measured_cpu_operator_costs'], p['charged_cpu_operator_costs']))


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--dbname')
    ap.add_argument('--psql', default='psql')
    ap.add_argument('--setup', action='store_true')
    ap.add_argument('--warmup', type=int, default=2)
    ap.add_argument('--repeats', type=int, default=15)
    ap.add_argument('--out')
    args = ap.parse_args()
    psql = Psql(args)
    if args.setup:
        psql.run(SETUP)
        return
    res = measure(args)
    report(res)
    if args.out:
        with open(args.out, 'w') as f:
            json.dump(res, f, indent=1)


if __name__ == '__main__':
    sys.exit(main())
