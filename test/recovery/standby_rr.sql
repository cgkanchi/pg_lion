-- The standby side of run.sh's recovery-conflict test (DESIGN.md section 9,
-- "Hot standby").  Run on the standby with
--   psql -X -At -v ON_ERROR_STOP=1 -v key=<k> -v hold=<seconds> -f standby_rr.sql
--
-- A REPEATABLE READ transaction counts key `key` from the index, then holds
-- its snapshot for `hold` seconds while the primary deletes those rows and
-- vacuums them away.  Exactly two outcomes are legal:
--
--   * the snapshot survives: the second count prints the same number (run.sh
--     checks n1 = n2), or
--   * the backend is cancelled or terminated with a recovery conflict, and
--     psql exits non-zero with "conflict with recovery" (run.sh accepts that
--     only when hot_standby_feedback is off).
--
-- A second count with a *different* number would mean the standby answered a
-- REPEATABLE READ snapshot from an index the primary had vacuumed under it.

BEGIN ISOLATION LEVEL REPEATABLE READ;

SELECT 'n1 ' || lion_index_count('lion_rec_k4'::regclass, :key ::int4);

-- Long enough for the primary's DELETE + VACUUM to be generated and replayed,
-- and for max_standby_streaming_delay to expire if replay is blocked on us.
SELECT pg_sleep(:hold);

SELECT 'n2 ' || lion_index_count('lion_rec_k4'::regclass, :key ::int4);

COMMIT;
