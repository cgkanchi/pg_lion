# Queries over bench/calib/joins.sql's tables, for matrix.py --queries: the
# joins of DESIGN.md §40.10 and §46 to §49 - LionProbeJoin, LionOrderedJoin
# and its split, key sets, and a nested loop's LionBtreeScan probe,
# index-only and in heap mode - and competitor_cold.sql's shapes, at a scale
# where the dimension's heap (1.6 GB) is larger than shared_buffers.  The
# regression fixtures' shapes (probejoin.sql, ordjoin.sql,
# ordjoin_keyfold.sql, heapprobe.sql, competitor_cold.sql), their row counts
# chosen on both sides of the crossovers DESIGN.md measured.  Run with
#
#   --sweep pg_lion.competitor_pricing+pg_lion.enable_heap_probe=on+off,off+off,on+on,off+on
#   --arms core,hashjoin,nestloop,lion,probejoin,ordjoin,probe

# §46: the CASE over the dimension's lion columns beside a fact column, the
# fact rows chosen by their key's range (40,000, 400,000 and 1,600,000 of
# 8,000,000), with and without the NOT EXISTS
PJ = ("SELECT CASE WHEN d.region IN ('north', 'east') THEN 'ne' "
      "WHEN d.tier = 1 THEN 't1' ELSE 'other' END AS k, f.channel, count(*) "
      "FROM jn_fact f JOIN jn_dim d ON f.fk = d.pk "
      "WHERE f.id <= %d AND d.active AND d.tier < 4%s GROUP BY 1, 2")
ANTI = ' AND NOT EXISTS (SELECT 1 FROM jn_blocked b WHERE b.dim_id = f.fk)'

# §47: a page of the fact's score order across the key join; f.amount puts
# the fact's walk in heap mode, without it the walk is index-only (its
# B-tree is (score DESC) INCLUDE (fk, id))
OJ = ('SELECT f.id, f.score, f.amount, d.pk, d.region FROM jn_fact f '
      'JOIN jn_dim d ON f.fk = d.pk WHERE %s ORDER BY f.score DESC LIMIT 20%s')
OJIO = ('SELECT f.id, f.score, d.pk, d.region FROM jn_fact f '
        'JOIN jn_dim d ON f.fk = d.pk WHERE %s ORDER BY f.score DESC LIMIT 20')
DENSE = "f.chan = 3 AND d.region = 'north'"
MEDIUM = 'f.chan = 3 AND d.lvl = 3 AND d.flag'
SPARSE = "f.tags @> '{a1}' AND f.tags @> '{b2}' AND d.lvl = 3 AND d.kind = 3"

# §48: the ORDER BY led by a column of the dimension
OS = ('SELECT f.id, f.score, f.amount, d.pk, d.region FROM jn_fact f '
      'JOIN jn_dim d ON f.fk = d.pk WHERE %s ORDER BY %s, f.score DESC LIMIT 20')

# §47.6: key sets, 1,000 keys (product 7) and 80,000 (product 3); the
# score's B-tree holds the key (INCLUDE), the rank's does not; f.amount keeps
# both walks in heap mode
KS = ('SELECT f.id, f.%s, f.amount, d.pk, d.tier FROM jn_fact f '
      'JOIN jn_dim d ON f.fk = d.pk '
      'WHERE f.fk IN (SELECT k FROM jn_watch WHERE prod = %d) '
      'ORDER BY %sf.%s DESC LIMIT 100')

# §49: the heap probe - a column of the dimension its primary key does not
# hold - and §40.10: the index-only probe, the query reading d.pk alone; the
# outer rows a group of the fact (3,960), ten (39,600), fifty (198,000), or
# three rows; sets that pass 1%, 10% and every row of the dimension
HP = ('SELECT f.id, f.amount, d.name FROM jn_fact f JOIN jn_dim d ON d.pk = f.fk '
      'WHERE %s AND %s')
IO = ('SELECT d.pk, count(*) AS n FROM jn_dim d JOIN jn_fact f ON f.fk = d.pk '
      'WHERE %s AND %s GROUP BY d.pk ORDER BY n DESC LIMIT 100')
OUTER = [('3', 'f.id < 4'), ('4k', 'f.g = 0'), ('40k', 'f.g < 10'), ('200k', 'f.g < 50')]
PASS = [('1pct', "d.grade = 'alpha' AND d.tags && '{t1}'"),
        ('10pct', "d.grade = 'alpha'"),
        ('all', "d.grade IN ('delta', 'gamma', 'alpha', 'beta')")]

# competitor_cold.sql: an 8,000,000-row dimension not vacuumed
CCANTI = ('SELECT d.flag, f.kind, count(*) FROM %s f JOIN jn_ccd d ON f.fk = d.pk '
          'WHERE f.id <= %d AND d.seg IN (1, 2, 3, 4, 5, 6) AND NOT EXISTS '
          '(SELECT 1 FROM %s s WHERE s.fk = d.pk AND s.why = 1) GROUP BY 1, 2')

QUERIES = [
    ('probejoin', 'pj.anti.40k', PJ % (40000, ANTI)),
    ('probejoin', 'pj.anti.400k', PJ % (400000, ANTI)),
    ('probejoin', 'pj.anti.1600k', PJ % (1600000, ANTI)),
    ('probejoin', 'pj.40k', PJ % (40000, '')),
    ('probejoin', 'pj.400k', PJ % (400000, '')),
    ('ordjoin', 'oj.dense', OJ % (DENSE, '')),
    ('ordjoin', 'oj.medium', OJ % (MEDIUM, '')),
    ('ordjoin', 'oj.medium.tags', OJ % ("f.tags @> '{a1}' AND d.lvl = 3 AND d.flag", '')),
    ('ordjoin', 'oj.medium.io', OJIO % MEDIUM),
    ('ordjoin', 'oj.dimonly.io', OJIO % 'd.lvl = 3 AND d.flag'),
    ('ordjoin', 'oj.range', OJ % ('f.chan = 3 AND d.lvl < 50', '')),
    ('ordjoin', 'oj.deep', OJ % (DENSE, ' OFFSET 2000')),
    ('ordjoin', 'oj.sparse', OJ % (SPARSE, '')),
    ('split', 'os.flag.dense', OS % (DENSE, 'd.flag DESC')),
    ('split', 'os.flag.medium', OS % ('f.chan = 3 AND d.lvl = 3', 'd.flag DESC')),
    ('split', 'os.region.dense', OS % ('f.chan = 3 AND d.lvl < 50', 'd.region')),
    ('split', 'os.flag.sparse', OS % (SPARSE, 'd.flag DESC')),
    ('keyset', 'ks.inc.1k', KS % ('score', 7, '', 'score')),
    ('keyset', 'ks.inc.80k', KS % ('score', 3, '', 'score')),
    ('keyset', 'ks.noinc.1k', KS % ('rank', 7, '', 'rank')),
    ('keyset', 'ks.noinc.80k', KS % ('rank', 3, '', 'rank')),
    ('keyset', 'ks.split.inc.1k', KS % ('score', 7, 'd.tier, ', 'score')),
    ('keyset', 'ks.split.inc.80k', KS % ('score', 3, 'd.tier, ', 'score')),
    ('keyset', 'ks.split.noinc.1k', KS % ('rank', 7, 'd.tier, ', 'rank')),
    ('keyset', 'ks.split.noinc.80k', KS % ('rank', 3, 'd.tier, ', 'rank')),
] + [
    ('heapprobe', 'hp.%s.%s' % (o, p), HP % (oc, pc))
    for o, oc in OUTER for p, pc in PASS if o != '3' or p == '10pct'
] + [
    ('ioprobe', 'io.%s.%s' % (o, p), IO % (oc, pc))
    for o, oc in OUTER[1:] for p, pc in PASS if p != 'all' or o == '40k'
] + [
    ('cold', 'cc.anti.3k', CCANTI % ('jn_ccf', 3000, 'jn_ccs')),
    ('cold', 'cc.anti.30k', CCANTI % ('jn_ccf', 30000, 'jn_ccs')),
    ('cold', 'cc.memo', 'SELECT d.flag, f.kind, count(*) FROM jn_ccm f '
     'JOIN jn_ccd d ON f.fk = d.pk WHERE d.seg = 1 GROUP BY 1, 2'),
    ('cold', 'cc.sub', 'SELECT q.flag, q.kind, count(*) FROM (SELECT d.flag, f.kind '
     'FROM jn_ccm f JOIN jn_ccd d ON f.fk = d.pk WHERE d.seg = 1 OFFSET 0) q GROUP BY 1, 2'),
    ('cold', 'cc.big', CCANTI % ('jn_ccg', 3000, 'jn_ccs')),
    ('cold', 'cc.wide', CCANTI % ('jn_ccg', 100000, 'jn_cct')),
]
