\set base random(1, 1000000000)
INSERT INTO writes SELECT (:base)::bigint * 1000 + i, i % 2 FROM generate_series(1, 100) i;
