-- WHERE clauses the partition bounds imply (DESIGN.md section 16, "Clauses
-- the partition bounds imply").
--
-- A count over a partitioned table needs a lion index for every WHERE clause
-- in every partition it counts - except for a clause the partition's bounds,
-- its ancestors' included, imply.  Every row of such a partition satisfies it,
-- so it selects nothing there, needs no index, and is left out of that
-- partition's count.  What has to be checked: the clause is left out exactly
-- where core's own implication proof says so (sub-partitions, a default
-- partition, a partition that accepts NULL, a clause implied by some of the
-- partitions counted and not by others), the query is still declined where
-- it is not, and every answer is the ordinary plan's.
\set VERBOSITY terse
SET client_min_messages = warning;
LOAD 'pg_lion';
SET synchronous_commit = on;
SET max_parallel_workers_per_gather = 0;

CREATE EXTENSION IF NOT EXISTS pg_lion;

/*
 * lion_ip() runs one query with the pushdown on and off, checks that the two
 * results are the same multiset, and says whether the node ran - and, when a
 * partition left a clause out, which one.
 */
CREATE FUNCTION lion_ip(q text) RETURNS text
LANGUAGE plpgsql AS $$
DECLARE
	ln text;
	pushed boolean := false;
	implied text := NULL;
	nrows bigint;
	ndiff bigint;
BEGIN
	PERFORM set_config('pg_lion.enable_count_pushdown', 'on', true);
	FOR ln IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		IF ln LIKE '%Custom Scan (LionCount)%' THEN
			pushed := true;
		END IF;
		IF ln LIKE '%Implied by Partition Bounds:%' THEN
			implied := btrim(split_part(ln, 'Implied by Partition Bounds:', 2));
		END IF;
	END LOOP;
	EXECUTE format('CREATE TEMP TABLE lion_ip_on AS %s', q);

	PERFORM set_config('pg_lion.enable_count_pushdown', 'off', true);
	EXECUTE format('CREATE TEMP TABLE lion_ip_off AS %s', q);
	PERFORM set_config('pg_lion.enable_count_pushdown', 'on', true);

	EXECUTE 'SELECT count(*) FROM lion_ip_on' INTO nrows;
	EXECUTE 'SELECT (SELECT count(*) FROM (SELECT * FROM lion_ip_on EXCEPT ALL SELECT * FROM lion_ip_off) a)'
			' + (SELECT count(*) FROM (SELECT * FROM lion_ip_off EXCEPT ALL SELECT * FROM lion_ip_on) b)'
		INTO ndiff;
	EXECUTE 'DROP TABLE lion_ip_on, lion_ip_off';

	IF ndiff <> 0 THEN
		RETURN format('MISMATCH: %s rows differ', ndiff);
	END IF;
	RETURN format('%s, %s rows%s',
				  CASE WHEN pushed THEN 'pushed down' ELSE 'not pushed down' END,
				  nrows,
				  CASE WHEN implied IS NULL THEN ''
					   ELSE '; implied: ' || implied END);
END $$;

/*
 * The plan of one query with the pushdown on - without the "Disabled: true"
 * PostgreSQL 18 prints under a disabled node, so that every release prints
 * the same plan.
 */
CREATE FUNCTION lion_ipplan(q text) RETURNS SETOF text
LANGUAGE plpgsql AS $$
DECLARE
	ln text;
BEGIN
	PERFORM set_config('pg_lion.enable_count_pushdown', 'on', true);
	FOR ln IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		CONTINUE WHEN ln ~ '^\s*Disabled: true$';
		RETURN NEXT ln;
	END LOOP;
END $$;

-- ---- the table ----------------------------------------------------------
/*
 * LIST-partitioned by kind, which no lion index covers: 'a' sub-partitioned
 * by year on ts, 'b' alone, 'c' and 'd' together (the one partition with a
 * lion index on kind itself), NULL with 'n', and a default partition for the
 * rest.  Every leaf has a lion index on (tags, ts, fk), and x is indexed on
 * its own.
 */
CREATE TABLE ip (
	id		int NOT NULL,
	kind	text,
	tags	text[],
	ts		timestamptz,
	fk		int,
	x		int
) PARTITION BY LIST (kind);
CREATE TABLE ip_a PARTITION OF ip FOR VALUES IN ('a') PARTITION BY RANGE (ts);
CREATE TABLE ip_a_2025 PARTITION OF ip_a
	FOR VALUES FROM ('2025-01-01') TO ('2026-01-01');
CREATE TABLE ip_a_2026 PARTITION OF ip_a
	FOR VALUES FROM ('2026-01-01') TO ('2027-01-01');
CREATE TABLE ip_b PARTITION OF ip FOR VALUES IN ('b');
CREATE TABLE ip_cd PARTITION OF ip FOR VALUES IN ('c', 'd');
CREATE TABLE ip_n PARTITION OF ip FOR VALUES IN (NULL, 'n');
CREATE TABLE ip_def PARTITION OF ip DEFAULT;

INSERT INTO ip
SELECT i,
	   CASE i % 8 WHEN 0 THEN 'a' WHEN 1 THEN 'a' WHEN 2 THEN 'b'
				  WHEN 3 THEN 'c' WHEN 4 THEN 'd' WHEN 5 THEN 'n'
				  WHEN 6 THEN NULL ELSE 'e' END,
	   ARRAY['x' || (i % 7), 'y' || (i % 11)],
	   timestamptz '2025-01-01' + ((i * 7) % 700) * interval '1 day',
	   CASE WHEN i % 13 = 0 THEN NULL ELSE i % 400 END,
	   i % 5
  FROM generate_series(1, 32000) i;
CREATE INDEX ip_tsf ON ip USING lion (tags, ts, fk);
CREATE INDEX ip_x ON ip USING lion (x);
CREATE INDEX ip_cd_kind ON ip_cd USING lion (kind);
VACUUM (FREEZE, ANALYZE) ip;

/* Force the node wherever it has a path, so that every answer is its own. */
SET enable_seqscan = off;
SET enable_bitmapscan = off;
SET enable_indexscan = off;
SET enable_indexonlyscan = off;

-- ---- implied by every partition counted ----------------------------------
/*
 * kind = 'a' leaves the two sub-partitions of ip_a, whose own bounds are on
 * ts: it is their parent's bound that implies it.  No index on kind is needed.
 */
SELECT lion_ipplan($q$SELECT count(*) FROM ip WHERE kind = 'a' AND tags && '{x1}'$q$);
SELECT lion_ip($q$SELECT count(*) FROM ip WHERE kind = 'a' AND tags && '{x1}'$q$);
SELECT lion_ip($q$SELECT count(*) FROM ip WHERE kind = 'b' AND tags && '{x1}'$q$);
SELECT lion_ip($q$SELECT count(*) FROM ip WHERE kind = 'a' AND tags && '{x1}' AND ts >= '2025-06-01'$q$);
SELECT lion_ip($q$SELECT count(*) FROM ip WHERE kind = 'a' AND tags && '{x1}' AND ts < '2025-03-01'$q$);
SELECT lion_ip($q$SELECT count(*) FROM ip WHERE kind = 'a' AND tags @> '{x1,y2}' AND x = 3$q$);
/* a stable expression, which only run-time pruning could use */
SELECT lion_ipplan($q$SELECT count(*) FROM ip WHERE kind = 'a' AND tags && '{x1}' AND ts >= now() - interval '100 years'$q$);
SELECT lion_ip($q$SELECT count(*) FROM ip WHERE kind = 'a' AND tags && '{x1}' AND ts >= now() - interval '100 years'$q$);
/* an IN list, the partitions it leaves each implying it */
SELECT lion_ip($q$SELECT count(*) FROM ip WHERE kind IN ('a', 'b') AND tags && '{x1}'$q$);
/* the null test a LIST bound without NULL carries */
SELECT lion_ip($q$SELECT count(*) FROM ip WHERE kind IS NOT NULL AND kind = 'b' AND tags && '{x2}'$q$);
/*
 * Ranges.  One on the key keeps the default partition, which could hold
 * values between any two of the others' (the planner does not prune it), and
 * which implies nothing of it: declined.  One on ts that ip_a_2026's own bound
 * implies: a range every partition implies does not drive the count while
 * another clause selects the rows, and is left out; with nothing else it
 * still drives, and only kind is left out.
 */
SELECT lion_ip($q$SELECT count(*) FROM ip WHERE kind >= 'b' AND kind <= 'b' AND tags && '{x3}'$q$);
SELECT lion_ipplan($q$SELECT count(*) FROM ip WHERE kind = 'a' AND ts >= '2026-01-01' AND tags && '{x1}'$q$);
SELECT lion_ip($q$SELECT count(*) FROM ip WHERE kind = 'a' AND ts >= '2026-01-01' AND tags && '{x1}'$q$);
SELECT lion_ipplan($q$SELECT count(*) FROM ip WHERE kind = 'a' AND ts >= '2026-01-01'$q$);
SELECT lion_ip($q$SELECT count(*) FROM ip WHERE kind = 'a' AND ts >= '2026-01-01'$q$);
/*
 * A range across the sub-partitions' bound, which ip_a_2026's alone implies:
 * it is not implied everywhere, so it drives the count, and a range that
 * bounds the driving walk is never left out.
 */
SELECT lion_ip($q$SELECT count(*) FROM ip WHERE kind = 'a' AND ts >= '2025-06-01' AND tags && '{x1}'$q$);
/*
 * No posting set answers an OR one of whose arms is not a plain column, but
 * every partition kind = 'a' leaves implies it: it is left out, and so is
 * what its first arm had been taken as.  Without kind = 'a' nothing prunes,
 * and the partitions that do not imply it decline the query.
 */
SELECT lion_ip($q$SELECT count(*) FROM ip WHERE kind = 'a' AND (kind = 'a' OR upper(kind) = 'Z') AND tags && '{x1}'$q$);
SELECT lion_ip($q$SELECT count(*) FROM ip WHERE (kind = 'a' OR upper(kind) = 'Z') AND tags && '{x1}'$q$);
/* grouped: the partial counts of every partition, with the clause left out */
SELECT lion_ipplan($q$SELECT fk, count(*) FROM ip WHERE kind = 'b' AND tags && '{x1}' GROUP BY fk$q$);
SELECT lion_ip($q$SELECT fk, count(*) FROM ip WHERE kind = 'b' AND tags && '{x1}' GROUP BY fk$q$);
SELECT lion_ip($q$SELECT x, count(*) FROM ip WHERE kind = 'a' GROUP BY x$q$);
SELECT lion_ip($q$SELECT x, count(*) FROM ip WHERE kind = 'a' AND ts >= '2026-03-01' GROUP BY x$q$);
SELECT lion_ip($q$SELECT fk, x, count(*) FROM ip WHERE kind = 'a' AND tags && '{y3}' GROUP BY fk, x$q$);

-- ---- implied by some of the partitions and not by others ----------------
/*
 * ip_cd holds 'd' too, so its bounds do not imply the list: it answers it from
 * its own index on kind, while ip_a's two leaves and ip_b leave it out.
 */
SELECT lion_ipplan($q$SELECT count(*) FROM ip WHERE kind IN ('a', 'b', 'c') AND tags && '{x1}'$q$);
SELECT lion_ip($q$SELECT count(*) FROM ip WHERE kind IN ('a', 'b', 'c') AND tags && '{x1}'$q$);
SELECT lion_ip($q$SELECT fk, count(*) FROM ip WHERE kind IN ('a', 'b', 'c') AND tags && '{x1}' GROUP BY fk$q$);
/* ... and a partition that answers the clause itself is not affected */
SELECT lion_ip($q$SELECT count(*) FROM ip WHERE kind = 'c' AND tags && '{x1}'$q$);
/*
 * A range taken as a source (a GROUP BY drives the count): ip_a_2026's bound
 * implies it, ip_b's does not, and ip_b reads it from its index.
 */
SELECT lion_ipplan($q$SELECT fk, count(*) FROM ip WHERE kind IN ('a', 'b') AND ts >= '2026-01-01' GROUP BY fk$q$);
SELECT lion_ip($q$SELECT fk, count(*) FROM ip WHERE kind IN ('a', 'b') AND ts >= '2026-01-01' GROUP BY fk$q$);
SELECT lion_ip($q$SELECT x, count(*) FROM ip WHERE kind IN ('a', 'b') AND ts >= '2026-01-01' AND ts < '2026-06-01' GROUP BY x$q$);
/* an OR across columns, implied as a whole by ip_a_2026 alone */
SELECT lion_ipplan($q$SELECT count(*) FROM ip WHERE kind IN ('a', 'b') AND (ts >= '2026-01-01' OR fk = 3) AND tags && '{x1}'$q$);
SELECT lion_ip($q$SELECT count(*) FROM ip WHERE kind IN ('a', 'b') AND (ts >= '2026-01-01' OR fk = 3) AND tags && '{x1}'$q$);

-- ---- NULLs and the default partition -------------------------------------
/*
 * ip_n holds NULL as well as 'n', so neither `kind = 'n'` nor `kind IS NULL`
 * is implied there and, with no index on kind, the query is declined; the OR
 * of the two is implied, and is left out.
 */
SELECT lion_ip($q$SELECT count(*) FROM ip WHERE kind = 'n' AND tags && '{x1}'$q$);
SELECT lion_ip($q$SELECT count(*) FROM ip WHERE kind IS NULL AND tags && '{x1}'$q$);
SELECT lion_ip($q$SELECT count(*) FROM ip WHERE (kind IS NULL OR kind = 'n') AND tags && '{x1}'$q$);
/*
 * The default partition's bound is the negation of every other one's, and
 * implies no equality: 'e' could share it with anything no partition lists.
 * It does imply the NOT IN of every listed value - no posting set answers
 * that, and it is left out altogether.  `kind IS NOT NULL` is implied by
 * every partition but ip_n, which holds the NULLs (the default partition
 * would, if no other partition took them) and has no index on kind:
 * declined.
 */
SELECT lion_ip($q$SELECT count(*) FROM ip WHERE kind = 'e' AND tags && '{x1}'$q$);
SELECT lion_ip($q$SELECT count(*) FROM ip WHERE kind NOT IN ('a', 'b', 'c', 'd', 'n') AND tags && '{x1}'$q$);
SELECT lion_ip($q$SELECT count(*) FROM ip WHERE kind IS NOT NULL AND tags && '{x1}'$q$);

-- ---- still declined ------------------------------------------------------
/* nothing left in the partitions to select rows by */
SELECT lion_ip($q$SELECT count(*) FROM ip WHERE kind = 'a'$q$);
SELECT lion_ip($q$SELECT count(*) FROM ip WHERE kind = 'a' AND fk IS NOT NULL$q$);
/* a pinned column the target list prints comes out of the index's key */
SELECT lion_ip($q$SELECT kind, count(*) FROM ip WHERE kind = 'b' AND tags && '{x1}' GROUP BY kind$q$);
/* a volatile clause is the query's to run per row */
SELECT lion_ip($q$SELECT count(*) FROM ip WHERE (kind = 'a' OR random() < 0) AND tags && '{x1}'$q$);
/* a parameter proves nothing at plan time: a generic plan declines */
PREPARE ip_kind(text) AS SELECT count(*) FROM ip WHERE kind = $1 AND tags && '{x1}';
SET plan_cache_mode = force_generic_plan;
SELECT lion_ipplan('EXECUTE ip_kind(''a'')');
EXECUTE ip_kind('a');
SET plan_cache_mode = force_custom_plan;
SELECT lion_ipplan('EXECUTE ip_kind(''a'')');
EXECUTE ip_kind('a');
RESET plan_cache_mode;
DEALLOCATE ip_kind;
SET pg_lion.enable_count_pushdown = off;
SELECT count(*) FROM ip WHERE kind = 'a' AND tags && '{x1}';
RESET pg_lion.enable_count_pushdown;

-- ---- a dirty heap ---------------------------------------------------------
DELETE FROM ip WHERE kind = 'a' AND id % 5 = 0;
UPDATE ip SET tags = ARRAY['x1'] WHERE kind = 'b' AND id % 9 = 0;
SELECT lion_ip($q$SELECT count(*) FROM ip WHERE kind = 'a' AND tags && '{x1}'$q$);
SELECT lion_ip($q$SELECT fk, count(*) FROM ip WHERE kind IN ('a', 'b', 'c') AND tags && '{x1}' GROUP BY fk$q$);
VACUUM (FREEZE) ip;
SELECT lion_ip($q$SELECT count(*) FROM ip WHERE kind = 'a' AND tags && '{x1}'$q$);
SELECT lion_ip($q$SELECT fk, count(*) FROM ip WHERE kind IN ('a', 'b', 'c') AND tags && '{x1}' GROUP BY fk$q$);

RESET enable_seqscan;
RESET enable_bitmapscan;
RESET enable_indexscan;
RESET enable_indexonlyscan;
DROP TABLE ip;
DROP FUNCTION lion_ip(text);
DROP FUNCTION lion_ipplan(text);
