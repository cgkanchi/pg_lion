select rb_and_cardinality(rb_and(a.idx2048, b.idx2048), c.idx2048) from rb_c10 a, rb_c200 b, rb_c2 c where a.key = 3 and b.key = 17 and c.key = 1;
