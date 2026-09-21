"""Deterministic datasets and explicit SQL/operator coverage."""

def am_name(family):
    """Access method name for a benchmark family label (the roaring family's AM is 'lion')."""
    return 'lion' if family == 'roaring' else family
from dataclasses import dataclass, asdict

SCALAR_COLUMNS = ['c2', 'c20', 'c200', 'c20k', 'c1m', 'clustered', 'skew', 'nullable']
FAMILIES = ['seq', 'btree', 'btree_tuned', 'hash', 'gin', 'gist', 'brin', 'roaring']


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
              Case('or_columns','union','SELECT count(*) FROM fact WHERE c200=17 OR c20=3'),
              Case('is_null','null','SELECT count(*) FROM fact WHERE nullable IS NULL',True),
              Case('not_null','null','SELECT count(*) FROM fact WHERE nullable IS NOT NULL'),
              Case('null_and','null','SELECT count(*) FROM fact WHERE nullable IS NULL AND c200=17'),
              Case('fetch_medium','heap_fetch','SELECT sum(id),sum(length(payload)) FROM fact WHERE c200=17',True),
              Case('fetch_rare','heap_fetch','SELECT sum(id),sum(length(payload)) FROM fact WHERE c20k=123'),
              Case('fetch_clustered','heap_fetch','SELECT sum(id),sum(length(payload)) FROM fact WHERE clustered=17'),
              Case('range_random','range','SELECT count(*) FROM fact WHERE c20k BETWEEN 100 AND 199',True),
              Case('range_clustered','range','SELECT count(*) FROM fact WHERE clustered BETWEEN 17 AND 26'),
              Case('range_broad','range','SELECT count(*) FROM fact WHERE c20k BETWEEN 100 AND 10000'),
              Case('ordered_limit','ordered_limit','SELECT c20k FROM fact WHERE c20k>=100 ORDER BY c20k LIMIT 100'),
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
