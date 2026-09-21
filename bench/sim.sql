\timing on
set enable_hashagg = off;
set work_mem = '1GB';
create or replace function int_to_ctid(i bigint) returns tid language sql immutable strict
  as $$ select ('(' || (i/2048) || ',' || (i%2048) || ')')::tid $$;

-- Build simulated roaring "indexes" under four TID encodings:
--   idx2048  : 32-bit, block*2048+off   (the original demo; caps table at 16GB)
--   idx292   : 32-bit, block*292+off    (dense: MaxHeapTuplesPerPage=291 for 8K pages; caps at ~112GB)
--   idx64_11 : 64-bit, block<<11 | off  (GIN-style encoding, no size cap)
--   idx64_16 : 64-bit, block<<16 | off  (raw ItemPointer layout; one container per heap page)
\set cols '''c2'',''c10'',''c200'',''c20k'',''c1m'',''c200_clustered'',''c_skew'''
select format($f$
  drop table if exists rb_%1$s;
  create table rb_%1$s as
  select %1$s as key,
         rb_build_agg((blk*2048+off)::int)      as idx2048,
         rb_build_agg((blk*292+off)::int)       as idx292,
         rb64_build_agg((blk<<11)|off)          as idx64_11,
         rb64_build_agg((blk<<16)|off)          as idx64_16
  from fact_tids group by %1$s;
  create index on rb_%1$s (key);
  $f$, c) from unnest(array[:cols]) c \gexec

-- Sizes: raw vs run-optimized, next to the real btree and GIN indexes on the same column
select format($f$
  select %1$L as col, count(*) as keys,
         pg_size_pretty(pg_relation_size('fact_%1$s_btree')) as btree,
         pg_size_pretty(pg_relation_size('fact_%1$s_gin'))   as gin,
         pg_size_pretty(sum(octet_length(idx2048::bytea))::bigint)                   as rb32_2048,
         pg_size_pretty(sum(octet_length(rb_runoptimize(idx2048)::bytea))::bigint)   as rb32_2048_run,
         pg_size_pretty(sum(octet_length(rb_runoptimize(idx292)::bytea))::bigint)    as rb32_292_run,
         pg_size_pretty(sum(octet_length(rb64_runoptimize(idx64_11)::bytea))::bigint) as rb64_11_run,
         pg_size_pretty(sum(octet_length(rb64_runoptimize(idx64_16)::bytea))::bigint) as rb64_16_run
  from rb_%1$s;
  $f$, c) from unnest(array[:cols]) c \gexec

-- Visibility-map masks in the 2048 encoding: all pages visible, and 95% visible (5% random pages "dirty")
drop table if exists vm_mask;
create table vm_mask as
with p as (select (pg_relation_size('fact')/8192)::bigint as npages)
select rb_fill('{}'::roaringbitmap, 0, (npages*2048)::bigint) as mask_all,
       rb_andnot(rb_fill('{}'::roaringbitmap, 0, (npages*2048)::bigint),
                 (select rb_or_agg(rb_fill('{}'::roaringbitmap, (b*2048)::bigint, ((b+1)*2048)::bigint))
                  from generate_series(0, npages-1) b where random() < 0.05)) as mask_95
from p;
select octet_length(mask_all::bytea) as mask_all_bytes, octet_length(mask_95::bytea) as mask_95_bytes,
       rb_cardinality(mask_all) as slots_all, rb_cardinality(mask_95) as slots_95 from vm_mask;
