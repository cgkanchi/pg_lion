select sum(c1m) from fact where ctid = any(array(select int8_to_tid(x) from rb_c200, rb_iterate(idx2048) x where key = 17));
