-- an update of one indexed column over a few neighbouring rows
\set id random(1, :maxid)
\set c200 random(0, 199)
UPDATE @T@ SET c200 = :c200, c2 = 1 - c2 WHERE id BETWEEN :id AND :id + 10;
