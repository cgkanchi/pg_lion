-- an update of indexed columns (not HOT), one row
\set id random(1, :maxid)
\set c20 random(0, 19)
\set c1m random(0, 999999)
UPDATE @T@ SET c20 = :c20, c1m = :c1m,
	cn = CASE WHEN random() < 0.2 THEN NULL ELSE floor(random() * 50)::int END,
	tags = CASE WHEN random() < 0.5 THEN tags ELSE ARRAY['t' || floor(random() * 30)::int] END
 WHERE id = :id;
