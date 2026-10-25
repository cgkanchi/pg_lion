#!/usr/bin/env python3
"""Lion's per-operation costs, measured against core's (DESIGN.md §49).

Each lion-only operation the planner prices is timed against the core
operation it stands in for, on the same build, server and tables, and
priced as that core operation's own rate prices it: its time over core's
price of it.  Core's plans do not all run at one rate - a cached Index Scan
loop takes two or three times as long a cost unit as a sequential scan's
quals - and a lion operation that competes with one of them must be priced
in that one's units for the planner to weigh the two as their times weigh.
A ratio of operations on one machine carries to another far better than a
rate of cost units a millisecond, which is the machine's: this is what
"calibrated against core" means for a constant.

  anchor  cpu_operator_cost at a sequential scan's rate: the slope of a
          cached sequential scan's time over the number of `a <> -k` quals
          it evaluates a row - each an int4ne of procost 1, one
          cpu_operator_cost a row to core.  Printed for comparison.
  probe   a nested loop probe's own work (DESIGN.md §40.10, §49): the heap
          probe over a set that passes every row, against core's Index Scan
          of the same B-tree with no filter - the same descents and the same
          heap fetches, the probe's set tests and its rescans apart - at the
          Index Scan's rate: the slope of its time over the outer rows over
          the slope of core's price of it.  The slopes, so that the set's
          build, made once, stays out; taken of a unique key (an entry a
          scan) and of a key of PROBE_MANY entries, which apportion it
          between the scan (pg_lion.probe_rescan_cost) and the entry
          (pg_lion.probe_entry_cost).

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
DROP TABLE IF EXISTS calib_ops_a, calib_ops_fact, calib_ops_dim, calib_ops_many;
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

-- the probe of a key of eight entries: the dimension's keys eight times
-- over, a key's rows on one page, under a B-tree on the key and a lion
-- index on a four-valued column
CREATE TABLE calib_ops_many (k int NOT NULL, grade int NOT NULL, v int NOT NULL)
  WITH (autovacuum_enabled = off);
INSERT INTO calib_ops_many
SELECT 1 + (i - 1) / 8, i % 4, i FROM generate_series(1, 800000) i;
CREATE INDEX calib_ops_many_k ON calib_ops_many (k);
CREATE INDEX calib_ops_many_grade ON calib_ops_many USING lion (grade);
VACUUM (FREEZE, ANALYZE) calib_ops_a, calib_ops_dim, calib_ops_fact,
  calib_ops_many;
"""

BASE = """SET max_parallel_workers_per_gather = 0;
SET jit = off;
"""

ANCHOR_QUALS = [0, 8, 16, 32]
PROBE_LOOPS = [50000, 100000, 200000]
PROBE_MANY = 8


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


def time_queries(psql, cases, warmup, repeats, prices=None):
    """cases: {name: (settings, query, check)}, check(plan) -> bool; the median
    Execution Time of each, its runs interleaved with the others'; and into
    prices, when given, each one's plan's Total Cost."""
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
            if prices is not None:
                prices[name] = float(p['Plan']['Total Cost'])
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
    # planning over a lion index loads the library, whose settings then exist
    out = psql.run('\\o /dev/null\nEXPLAIN SELECT 1 FROM calib_ops_dim WHERE grade = 0;\n'
                   '\\o\n' +
                   ''.join("SELECT current_setting('%s');\n" % n for n in names))
    return dict(zip(names, (float(v) for v in out.split())))


def measure(args):
    psql = Psql(args)
    s = settings_of(psql, ['cpu_operator_cost', 'cpu_tuple_cost',
                           'pg_lion.probe_entry_cost', 'pg_lion.probe_rescan_cost'])
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

    # the probe: its own ns an outer row over core's Index Scan's, of a
    # unique key and of a key of PROBE_MANY entries
    def probe(p):
        return any(n.get('Custom Plan Provider') == 'LionBtreeScan' for n in nodes(p))

    def core_of(rel):
        def core(p):
            return any(n['Node Type'] == 'Index Scan' and
                       n.get('Relation Name') == rel for n in nodes(p))
        return core
    loopset = ('SET LOCAL enable_hashjoin = off;\nSET LOCAL enable_mergejoin = off;\n'
               'SET LOCAL enable_memoize = off;\n')
    shapes = {
        'one': ('calib_ops_dim', 'd.name', 'd.id', 1),
        'many': ('calib_ops_many', 'd.v', 'd.k', PROBE_MANY),
    }
    probes = {}
    for shape, (rel, col, key, entries) in shapes.items():
        cases = {}
        for n in PROBE_LOOPS:
            q = ('SELECT count(%s) FROM calib_ops_fact f JOIN %s d '
                 'ON %s = f.dim_id WHERE f.id <= %d' % (col, rel, key, n))
            cases['p%d' % n] = (loopset + 'SET LOCAL pg_lion.enable_heap_probe = on;\n'
                                'SET LOCAL pg_lion.pushdown_margin = 1000;\n',
                                q + ' AND d.grade IN (0, 1, 2, 3)', probe)
            cases['c%d' % n] = (loopset + 'SET LOCAL pg_lion.enable_btree_scan = off;\n'
                                'SET LOCAL pg_lion.enable_ordered_scan = off;\n', q,
                                core_of(rel))
        prices = {}
        t2 = time_queries(psql, cases, args.warmup, args.repeats, prices)
        d_ms, d_fixed = slope(PROBE_LOOPS, [t2['p%d' % n] - t2['c%d' % n]
                                            for n in PROBE_LOOPS])
        c_ms, _ = slope(PROBE_LOOPS, [t2['c%d' % n] for n in PROBE_LOOPS])
        c_price, _ = slope(PROBE_LOOPS, [prices['c%d' % n] for n in PROBE_LOOPS])
        # the Index Scan's own rate: its ns an outer row over core's price
        # of an outer row, in ns a cpu_operator_cost
        ns_op_core = c_ms * 1e6 / (c_price / s['cpu_operator_cost'])
        probes[shape] = {'entries': entries, 'ms': t2, 'prices': prices,
                         'core_ns_per_loop': c_ms * 1e6,
                         'core_price_per_loop': c_price,
                         'core_ns_per_cpu_operator_cost': ns_op_core,
                         'probe_ns_per_loop_over_core': d_ms * 1e6,
                         'probe_cpu_operator_costs_per_loop':
                             d_ms * 1e6 / ns_op_core,
                         'build_ms': d_fixed}

    # apportioned between the scan and the entry, from the two shapes, each
    # in its Index Scan's units
    one = probes['one']
    many = probes['many']
    entry = ((many['probe_cpu_operator_costs_per_loop'] -
              one['probe_cpu_operator_costs_per_loop']) /
             (many['entries'] - one['entries']))
    scan = one['probe_cpu_operator_costs_per_loop'] - entry

    res = {
        'anchor': {'rows': nrows, 'ms': t, 'ns_per_cpu_operator_cost': ns_op},
        'probe': probes,
        'probe_apportioned': {
            'scan_cpu_operator_costs': scan,
            'entry_cpu_operator_costs': entry,
            'charged_scan_cpu_operator_costs': s['pg_lion.probe_rescan_cost'],
            'charged_entry_cpu_operator_costs': s['pg_lion.probe_entry_cost']},
    }
    return res


def report(res):
    a = res['anchor']
    print('## Anchor\n')
    print('| quals a row | ms |')
    print('|---|---|')
    for k, v in sorted(a['ms'].items(), key=lambda kv: int(kv[0][1:])):
        print('| %s | %.2f |' % (k[1:], v))
    print('\n%.2f ns a qual a row over %d rows: one cpu_operator_cost.\n'
          % (a['ns_per_cpu_operator_cost'], a['rows']))
    print('## Probe\n')
    print('| entries a key | outer rows | heap probe ms | core Index Scan ms |')
    print('|---|---|---|---|')
    for shape, p in res['probe'].items():
        for n in PROBE_LOOPS:
            print('| %d | %d | %.2f | %.2f |' % (p['entries'], n, p['ms']['p%d' % n],
                                                p['ms']['c%d' % n]))
    for shape, p in res['probe'].items():
        print('\n%d entries a key: core %.0f ns an outer row, priced %.4f - %.2f ns a '
              'cpu_operator_cost; the probe %+.1f ns an outer row over it, %.1f '
              'cpu_operator_costs at that rate (and %.2f ms once, its set).'
              % (p['entries'], p['core_ns_per_loop'], p['core_price_per_loop'],
                 p['core_ns_per_cpu_operator_cost'],
                 p['probe_ns_per_loop_over_core'],
                 p['probe_cpu_operator_costs_per_loop'], p['build_ms']))
    ap = res['probe_apportioned']
    print('\nA scan %.1f cpu_operator_costs (pg_lion.probe_rescan_cost %.1f); an entry '
          '%.1f (pg_lion.probe_entry_cost %.1f).'
          % (ap['scan_cpu_operator_costs'], ap['charged_scan_cpu_operator_costs'],
             ap['entry_cpu_operator_costs'], ap['charged_entry_cpu_operator_costs']))


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
