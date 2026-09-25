"""Deterministic datasets and explicit SQL/operator coverage."""

def am_name(family):
    """Access method name for a benchmark family label (the roaring family's AM is 'lion')."""
    return 'lion' if family in ('roaring', 'roaring_btree') else family
from dataclasses import dataclass, asdict

SCALAR_COLUMNS = ['c2', 'c20', 'c200', 'c20k', 'c1m', 'clustered', 'skew', 'nullable']
FAMILIES = ['seq', 'btree', 'btree_tuned', 'hash', 'gin', 'gist', 'brin', 'roaring', 'roaring_btree']
FOCUSED_FAMILIES = ['btree', 'gin', 'roaring', 'roaring_btree']
FOCUSED_SCALAR = [
    'eq_c2_0', 'eq_c200_17', 'eq_c20k_123', 'eq_c1m_12345', 'eq_skew_0', 'eq_skew_17',
    'in_c20k_10', 'in_c20k_1000', 'and2', 'and3_selective', 'in_and', 'or_columns',
    'is_null', 'fetch_medium', 'range_random', 'ordered_limit',
    'fetch_c1m', 'fetch_rare', 'fetch_broad', 'fetch_and2', 'fetch_and3', 'fetch_in_and',
    'group_c2', 'group_c200', 'group_filtered',
    'ordered_filter', 'ordered_filter_desc', 'ordered_broad',
]
FOCUSED_DOCS = [
    'array_common', 'array_and', 'array_or', 'ts_common', 'ts_rare', 'ts_and',
    'ts_tree', 'ts_phrase', 'ts_prefix', 'ts_fetch',
]
AFTER_MAINTENANCE = ['eq_c200_17', 'and2', 'is_null', 'group_c200']
LOW_MEMORY = ['eq_c2_0', 'in_c20k_1000', 'group_c200', 'fetch_medium']
# Row fetches that need the heap whatever the index: selectivity from a few rows to
# thousands, and multi-predicate ANDs.  Low memory is where a plain index scan
# (btree) and a lossy bitmap (lion, which has no amgettuple) part ways.
LOW_MEMORY_FETCH = ['fetch_c1m', 'fetch_rare', 'fetch_and3']
# A lion filter under ORDER BY a B-tree column with LIMIT (DESIGN.md §30, LionOrdered):
# selective (c200 and c2, 0.25% of the rows), the same descending, and unselective
# (c2 alone, half the rows), where the planner should keep core's ordered B-tree walk.
ORDERED_CASES = ['ordered_filter', 'ordered_filter_desc', 'ordered_broad']
# Families that measure only some cases: roaring_btree is the roaring portfolio plus a
# B-tree on the sort column, there for the ordered cases alone (scalar suite only).
FAMILY_CASES = {'roaring_btree': ORDERED_CASES}


def family_cases(family, names):
    """The case ids of names that family measures, in order."""
    keep = FAMILY_CASES.get(family)
    return [name for name in names if keep is None or name in keep]


def profile_cases(suite, profile):
    cases = scalar_cases() if suite == 'scalar' else doc_cases()
    if profile == 'full':
        return cases
    names = FOCUSED_SCALAR if suite == 'scalar' else FOCUSED_DOCS
    by_id = {c.id: c for c in cases}
    return [by_id[name] for name in names]


def profile_indexes(suite, family, profile):
    specs = index_specs(suite, family)
    if profile == 'full':
        return specs
    # Same full-width heaps; pay only for indexes exercised by the selected cases.
    unused = {'ix_clustered'} if suite == 'scalar' else {'ix_grp'}
    return [(name, sql) for name, sql in specs if name not in unused]


def measurement_matrix(suite, profile, maintenance=True):
    """The warm configurations, saved with each run for independent auditing."""
    cases = profile_cases(suite, profile)
    matrix = []

    def add(phase, names, modes=('default',), memory='64MB'):
        for mode in modes:
            matrix.append(dict(phase=phase, cases=list(names), mode=mode, memory=memory))

    modes = ('default', 'prefer_index') if profile == 'full' else ('default',)
    add('clean', [c.id for c in cases], modes)
    if suite == 'scalar':
        add('low_work_mem', LOW_MEMORY + LOW_MEMORY_FETCH, ('prefer_index',), '64kB')
        if profile == 'full':
            for phase in ('dirty_clustered_5pct', 'dirty_scattered'):
                add(phase, [c.id for c in cases if c.stress], modes)
        else:
            add('dirty_scattered', ['eq_c2_0', 'eq_c200_17', 'and2', 'group_c200'])
            add('dirty_scattered', ['group_c200'], ('prefer_index',))
        if maintenance:
            add('after_maintenance', AFTER_MAINTENANCE)
    return matrix


@dataclass(frozen=True)
class Case:
    id: str
    category: str
    sql: str
    stress: bool = False

    def record(self):
        return asdict(self)


def h(seed):
    return f'(hashint8extended(i::bigint,{seed}) & 9223372036854775807)'


def scalar_data(n, table='fact', start=1):
    return f"""SELECT i::bigint AS id,
        ({h(11)}%2)::int AS c2, ({h(22)}%20)::int AS c20,
        ({h(33)}%200)::int AS c200, ({h(44)}%20000)::int AS c20k,
        ({h(55)}%1000000)::int AS c1m,
        LEAST(199, ((i-1)*200/{n}))::int AS clustered,
        CASE WHEN {h(66)}%10<9 THEN 0 ELSE 1+({h(77)}%999)::int END AS skew,
        CASE WHEN {h(88)}%10=0 THEN NULL ELSE ({h(99)}%200)::int END AS nullable,
        repeat(md5(i::text),2) AS payload
        FROM generate_series({start}::bigint,{start+n-1}::bigint) i"""


def scalar_cases():
    cases = []
    for col, values in [('c2',[0,1]),('c20',[3,17]),('c200',[17,103]),
                        ('c20k',[123,12345]),('c1m',[12345,765432]),
                        ('clustered',[17,103]),('skew',[0,17]),('nullable',[17])]:
        for v in values:
            cases.append(Case(f'eq_{col}_{v}','equality',f'SELECT count(*) FROM fact WHERE {col}={v}',v==values[0]))
    cases.append(Case('eq_absent','equality','SELECT count(*) FROM fact WHERE c200=-1'))
    for length in [3,10,100,1000]:
        values=','.join(str(i) for i in range(length))
        cases.append(Case(f'in_c20k_{length}','in_list',f'SELECT count(*) FROM fact WHERE c20k IN ({values})',length in [10,1000]))
    cases += [Case('in_overlap','in_list','SELECT count(*) FROM fact WHERE c200=ANY(ARRAY[17,17,103,NULL])'),
              Case('in_and','intersection','SELECT count(*) FROM fact WHERE c200 IN (17,18,19) AND c20 IN (3,4,5)',True),
              Case('and2','intersection','SELECT count(*) FROM fact WHERE c200=17 AND c2=1',True),
              Case('and3','intersection','SELECT count(*) FROM fact WHERE c200=17 AND c20=3 AND c2=1',True),
              Case('and3_selective','intersection','SELECT count(*) FROM fact WHERE c20k=77 AND c200=17 AND c2=1',True),
              Case('or_columns','union','SELECT count(*) FROM fact WHERE c200=17 OR c20=3'),
              Case('is_null','null','SELECT count(*) FROM fact WHERE nullable IS NULL',True),
              Case('not_null','null','SELECT count(*) FROM fact WHERE nullable IS NOT NULL'),
              Case('null_and','null','SELECT count(*) FROM fact WHERE nullable IS NULL AND c200=17'),
              Case('fetch_medium','heap_fetch','SELECT sum(id),sum(length(payload)) FROM fact WHERE c200=17',True),
              Case('fetch_rare','heap_fetch','SELECT sum(id),sum(length(payload)) FROM fact WHERE c20k=123'),
              Case('fetch_c1m','heap_fetch','SELECT sum(id),sum(length(payload)) FROM fact WHERE c1m=12345',True),
              Case('fetch_broad','heap_fetch','SELECT sum(id),sum(length(payload)) FROM fact WHERE c20=3'),
              Case('fetch_and2','heap_fetch','SELECT sum(id),sum(length(payload)) FROM fact WHERE c200=17 AND c20=3',True),
              Case('fetch_and3','heap_fetch','SELECT sum(id),sum(length(payload)) FROM fact WHERE c200=17 AND c20=3 AND c2=1',True),
              Case('fetch_in_and','heap_fetch','SELECT sum(id),sum(length(payload)) FROM fact WHERE c200 IN (17,18,19) AND c20 IN (3,4,5)'),
              Case('fetch_clustered','heap_fetch','SELECT sum(id),sum(length(payload)) FROM fact WHERE clustered=17'),
              Case('range_random','range','SELECT count(*) FROM fact WHERE c20k BETWEEN 100 AND 199',True),
              Case('range_clustered','range','SELECT count(*) FROM fact WHERE clustered BETWEEN 17 AND 26'),
              Case('range_broad','range','SELECT count(*) FROM fact WHERE c20k BETWEEN 100 AND 10000'),
              Case('ordered_limit','ordered_limit','SELECT c20k FROM fact WHERE c20k>=100 ORDER BY c20k LIMIT 100'),
              Case('ordered_filter','ordered_filter','SELECT id,payload FROM fact WHERE c200=17 AND c2=1 ORDER BY c1m,id LIMIT 10'),
              Case('ordered_filter_desc','ordered_filter','SELECT id,payload FROM fact WHERE c200=17 AND c2=1 ORDER BY c1m DESC,id DESC LIMIT 10'),
              Case('ordered_broad','ordered_filter','SELECT id,payload FROM fact WHERE c2=1 ORDER BY c1m,id LIMIT 10'),
              Case('residual_filter','residual','SELECT count(*) FROM fact WHERE c200=17 AND length(payload)>60'),
              Case('count_distinct','distinct','SELECT count(DISTINCT c20k) FROM fact WHERE c200=17')]
    for col in ['c2','c20','c200','c20k','nullable']:
        cases.append(Case(f'group_{col}','grouping',f'SELECT {col},count(*) FROM fact GROUP BY {col}',col in ['c2','c200']))
    cases.append(Case('group_filtered','grouping','SELECT c20,count(*) FROM fact WHERE c200=17 GROUP BY c20',True))
    return cases


def doc_data(n):
    return f"""SELECT i::bigint AS id, ({h(13)}%20)::int AS grp,
        CASE WHEN i%101=0 THEN NULL WHEN i%97=0 THEN ARRAY[]::text[]
             ELSE ARRAY['t'||({h(14)}%20),'t'||({h(15)}%200),'t'||({h(16)}%2000)] END AS tags,
        CASE WHEN i%101=0 THEN NULL ELSE to_tsvector('simple',
             CASE WHEN i%97=0 THEN '' ELSE
             'common w'||({h(14)}%20)||' w'||({h(15)}%200)||' w'||({h(16)}%2000)||
             ' rare'||({h(17)}%10000)||' endword' END) END AS tsv,
        repeat(md5(i::text),2) AS payload
        FROM generate_series(1::bigint,{n}::bigint) i"""


def doc_cases():
    predicates = [
        ('array_common','array_contains',"tags @> ARRAY['t1']"),
        ('array_rare','array_contains',"tags @> ARRAY['t1234']"),
        ('array_and','array_contains',"tags @> ARRAY['t1','t17']"),
        ('array_or','array_overlap',"tags && ARRAY['t1','t17','t123']"),
        ('array_empty','array_fallback',"tags @> ARRAY[]::text[]"),
        ('array_contained','array_fallback',"tags <@ ARRAY['t1','t17','t123']"),
        ('array_null_element','array_fallback',"tags @> ARRAY['t1',NULL]"),
        ('array_is_null','null','tags IS NULL'),
        ('ts_common','text_search',"tsv @@ 'common'::tsquery"),
        ('ts_rare','text_search',"tsv @@ 'rare1234'::tsquery"),
        ('ts_and','text_search',"tsv @@ 'w1 & w17'::tsquery"),
        ('ts_or','text_search',"tsv @@ 'w1 | w17 | w123'::tsquery"),
        ('ts_tree','text_search',"tsv @@ '(w1 | w2) & (w17 | w18)'::tsquery"),
        ('ts_phrase','text_fallback',"tsv @@ 'common <-> w1'::tsquery"),
        ('ts_prefix','text_fallback',"tsv @@ 'rare12:*'::tsquery"),
        ('ts_not','text_fallback',"tsv @@ 'common & !w1'::tsquery"),
        ('ts_weight','text_fallback',"tsv @@ 'w1:D'::tsquery"),
    ]
    result=[Case(name,cat,f'SELECT count(*) FROM docs WHERE {pred}') for name,cat,pred in predicates]
    result += [Case('array_fetch','heap_fetch',"SELECT sum(id),sum(length(payload)) FROM docs WHERE tags @> ARRAY['t1']"),
               Case('ts_fetch','heap_fetch',"SELECT sum(id),sum(length(payload)) FROM docs WHERE tsv @@ 'w1'::tsquery"),
               Case('array_group','grouping',"SELECT grp,count(*) FROM docs WHERE tags && ARRAY['t1','t17'] GROUP BY grp"),
               Case('ts_group','grouping',"SELECT grp,count(*) FROM docs WHERE tsv @@ 'w1 & w17'::tsquery GROUP BY grp")]
    return result


def index_specs(suite, family):
    if family=='seq':
        return []
    if suite=='scalar':
        if family=='roaring_btree':
            return index_specs(suite, 'roaring') + [
                ('ix_c1m_btree', 'CREATE INDEX ix_c1m_btree ON fact USING btree (c1m)')]
        method='btree' if family=='btree_tuned' else family
        suffix=' WITH (fastupdate=on)' if family=='gin' else ' WITH (pages_per_range=32)' if family=='brin' else ''
        specs=[(f'ix_{c}',f'CREATE INDEX ix_{c} ON fact USING {am_name(method)} ({c}){suffix}') for c in SCALAR_COLUMNS]
        if family=='btree_tuned':
            specs += [('ix_composite','CREATE INDEX ix_composite ON fact (c200,c20,c2)'),
                      ('ix_covering','CREATE INDEX ix_covering ON fact (c200) INCLUDE (id,payload)')]
        return specs
    if suite=='documents':
        if family=='gist':
            return [('ix_tsv','CREATE INDEX ix_tsv ON docs USING gist (tsv)'),
                    ('ix_grp','CREATE INDEX ix_grp ON docs USING gist (grp)')]
        return [(f'ix_{c}',f'CREATE INDEX ix_{c} ON docs USING {am_name(family)} ({c})') for c in ['tags','tsv','grp']]
    raise ValueError(suite)
