#!/bin/bash
S=${SCRATCH:?set SCRATCH to the directory holding pginst/ and q/}
B=$S/pginst/bin; Q=$S/q; LOG=$S/bench_plans.log; RES=$S/bench_results.txt
export PGHOST=/tmp/claude-1000/pgsk PGPORT=54329 PGUSER=postgres PGDATABASE=postgres
T=${T:-5}
# ONLY="1 2b 2c" runs a subset of phases; default all
only() { [ -z "${ONLY:-}" ] || [[ " $ONLY " == *" $1 "* ]]; }
RESET_RES=${RESET_RES:-1}; [ "$RESET_RES" = 1 ] && { : > $LOG; : > $RES; }
run() { local name="$1" opts="$2" f="$Q/$3"
  echo "### $name  [$opts]" >> $LOG
  PGOPTIONS="$opts" $B/psql -X -q -c "explain (analyze, buffers, costs off, timing off) $(cat $f)" >> $LOG 2>&1
  lat=$(PGOPTIONS="$opts" $B/pgbench -n -M prepared -T $T -f $f 2>&1 | grep 'latency average' | awk '{print $4}')
  printf "%-28s %12s ms\n" "$name" "$lat" | tee -a $RES
}
valid()   { # usage: valid btree|gin|roaring
   $B/psql -X -q -c "update pg_index set indisvalid = true  where indexrelid::regclass::text like 'fact\_%\_$1'"; }
invalid() { $B/psql -X -q -c "update pg_index set indisvalid = false where indexrelid::regclass::text like 'fact\_%\_$1'"; }
$B/psql -X -q -c "select count(pg_prewarm(c.oid)) from pg_class c where relname like 'fact_%' and relname not like 'fact_tids%' or relname like 'rb_%' or relname = 'vm_mask' or relname = 'fact'" >/dev/null
BIT="-c enable_seqscan=off -c enable_indexonlyscan=off -c enable_indexscan=off"
IOS="-c enable_seqscan=off -c enable_bitmapscan=off"
if only 1; then
echo "== phase 1: btree only =="; valid btree; invalid gin; invalid roaring
for c in c2 c200 c20k c1m; do run "p_${c}_btree_ios" "$IOS" p_${c}_idx.sql; done
run "p_c200_btree_bitmap" "$BIT" p_c200_idx.sql
run "and3_btree" "-c enable_seqscan=off" and3_idx.sql
run "grp_c200_btree_ios" "$IOS -c enable_hashagg=off" grp_c200_idx.sql
run "grp_c2_btree_ios" "$IOS -c enable_hashagg=off" grp_c2_idx.sql
run "range_c20k_btree_ios" "$IOS" range_c20k_idx.sql
run "fetch_c200_btree" "-c enable_seqscan=off" fetch_c200_idx.sql
fi
if only 2; then
echo "== phase 2: gin only =="; invalid btree; valid gin
for c in c2 c200 c20k c1m; do run "p_${c}_gin_bitmap" "-c enable_seqscan=off" p_${c}_idx.sql; done
run "and3_gin" "-c enable_seqscan=off" and3_idx.sql
run "range_c20k_gin_bitmap" "-c enable_seqscan=off" range_c20k_idx.sql
run "fetch_c200_gin" "-c enable_seqscan=off" fetch_c200_idx.sql
fi
if only 2b; then
echo "== phase 2b: roaring AM only =="; invalid btree; invalid gin; valid roaring
NB="-c enable_seqscan=off -c roaring_index.enable_count_pushdown=off"
for c in c2 c200 c200_clustered c20k c1m; do run "p_${c}_roaring_bitmap" "$NB" p_${c}_idx.sql; done
run "and3_roaring" "$NB" and3_idx.sql
run "range_c20k_roaring_bitmap" "$NB" range_c20k_idx.sql
run "fetch_c200_roaring" "$NB" fetch_c200_idx.sql
fi
if only 2c; then
echo "== phase 2c: roaring count pushdown (CustomScan) =="; valid roaring
PD="-c roaring_index.enable_count_pushdown=on"
for c in c2 c200 c200_clustered c20k c1m; do run "p_${c}_pushdown" "$PD" p_${c}_idx.sql; done
run "and3_pushdown" "$PD" and3_idx.sql
for c in c200 c2; do run "grp_${c}_pushdown" "$PD" grp_${c}_idx.sql; done
fi
if only 3; then
echo "== phase 3: seqscan + roaring simulation =="; valid btree; valid gin; invalid roaring
run "grp_c200_seqscan" "-c enable_indexonlyscan=off -c enable_indexscan=off -c enable_bitmapscan=off -c roaring_index.enable_count_pushdown=off" grp_c200_idx.sql
run "grp_c2_seqscan" "-c enable_indexonlyscan=off -c enable_indexscan=off -c enable_bitmapscan=off -c roaring_index.enable_count_pushdown=off" grp_c2_idx.sql
for c in c2 c200 c20k c1m; do for v in naive vmall vm95; do run "p_${c}_rb_$v" "" p_${c}_rb_$v.sql; done; done
run "and3_rb_naive" "" and3_rb_naive.sql
run "and3_rb_vm95" "" and3_rb_vm95.sql
for c in c200 c2; do for v in naive vmall vm95; do run "grp_${c}_rb_$v" "" grp_${c}_rb_$v.sql; done; done
run "range_c20k_rb_naive" "" range_c20k_rb_naive.sql
run "fetch_c200_rb_tidscan" "" fetch_c200_rb_tidscan.sql
fi
echo BENCH_DONE | tee -a $RES
