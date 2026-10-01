-- a delete of one row, or of a short run of them
\set id random(1, :maxid)
\set w random(0, 6)
DELETE FROM @T@ WHERE id BETWEEN :id AND :id + :w;
