-- Run against the benchmark cluster after `create extension roaring_index`.
\timing on
create extension if not exists roaring_index;
-- the count pushdown requires NOT NULL group-by columns (NULL keys are not indexed)
alter table fact alter column c2 set not null, alter column c10 set not null, alter column c200 set not null,
                 alter column c20k set not null, alter column c1m set not null,
                 alter column c200_clustered set not null, alter column c_skew set not null;
\set cols '''c2'',''c10'',''c200'',''c20k'',''c1m'',''c200_clustered'',''c_skew'''
select format('drop index if exists fact_%1$s_roaring; create index fact_%1$s_roaring on fact using roaring (%1$s);', c)
  from unnest(array[:cols]) c \gexec
select relname, pg_size_pretty(pg_relation_size(oid)) as size
  from pg_class where relname like 'fact_%' and relname not like 'fact_tids%' order by regexp_replace(relname, '_(btree|gin|roaring)$', ''), relname;
select c, s.* from unnest(array[:cols]) c, lateral roaring_index_stats(('fact_' || c || '_roaring')::regclass) s;
