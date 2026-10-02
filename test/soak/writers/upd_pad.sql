-- an update of the column no index reads: HOT where the page has room
\set id random(1, :maxid)
UPDATE @T@ SET pad = repeat('y', 5 + floor(random() * 60)::int) WHERE id = :id;
