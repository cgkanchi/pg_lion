# The recorded order's comparison identity (DESIGN.md §21) checked in a
# backend that ALREADY has the index open (the 2026-09-24 external review).
#
# A backend keeps an index's state in its relcache entry (rd_amcache), and a
# CREATE OR REPLACE FUNCTION of the opclass's comparison sends no relcache
# invalidation for the index - only one for pg_proc.  So s1, which counted
# through the index before s2 replaced the comparison, used to go on reading
# the directory with the NEW comparison: a directory built ascending, searched
# descending, answered 0 rows for 10.  It has to notice the pg_proc change and
# ask for REINDEX, exactly as a new backend does - and answer again once the
# comparison is put back.
#
# The same for a replacement by a SQL-standard (`RETURN`) body, whose prosrc is
# empty: the identity changes all the same, because the source it was built
# with is gone.

setup
{
	CREATE EXTENSION IF NOT EXISTS pg_lion;
	CREATE FUNCTION oic_cmp(int4, int4) RETURNS int4 LANGUAGE sql IMMUTABLE STRICT
		AS 'SELECT btint4cmp($1, $2)';
	CREATE OPERATOR FAMILY oic_fam USING lion;
	CREATE OPERATOR CLASS oic_ops FOR TYPE int4 USING lion FAMILY oic_fam AS
		OPERATOR 1 = (int4, int4),
		OPERATOR 6 < (int4, int4),
		OPERATOR 7 <= (int4, int4),
		OPERATOR 8 >= (int4, int4),
		OPERATOR 9 > (int4, int4),
		FUNCTION 1 hashint4(int4),
		FUNCTION 4 oic_cmp(int4, int4);
	CREATE FUNCTION oic_lt(int4, int4) RETURNS bool LANGUAGE sql IMMUTABLE STRICT AS 'SELECT $1 < $2';
	CREATE FUNCTION oic_le(int4, int4) RETURNS bool LANGUAGE sql IMMUTABLE STRICT AS 'SELECT $1 <= $2';
	CREATE FUNCTION oic_eq(int4, int4) RETURNS bool LANGUAGE sql IMMUTABLE STRICT AS 'SELECT $1 = $2';
	CREATE FUNCTION oic_ge(int4, int4) RETURNS bool LANGUAGE sql IMMUTABLE STRICT AS 'SELECT $1 >= $2';
	CREATE FUNCTION oic_gt(int4, int4) RETURNS bool LANGUAGE sql IMMUTABLE STRICT AS 'SELECT $1 > $2';
	CREATE OPERATOR <<< (LEFTARG = int4, RIGHTARG = int4, FUNCTION = oic_lt);
	CREATE OPERATOR <<= (LEFTARG = int4, RIGHTARG = int4, FUNCTION = oic_le);
	CREATE OPERATOR === (LEFTARG = int4, RIGHTARG = int4, FUNCTION = oic_eq);
	CREATE OPERATOR >>= (LEFTARG = int4, RIGHTARG = int4, FUNCTION = oic_ge);
	CREATE OPERATOR >>> (LEFTARG = int4, RIGHTARG = int4, FUNCTION = oic_gt);
	/* makes oic_cmp sortable, so the index is built ORDERED (§21 rule 3) */
	CREATE OPERATOR CLASS oic_bt FOR TYPE int4 USING btree AS
		OPERATOR 1 <<<, OPERATOR 2 <<=, OPERATOR 3 ===, OPERATOR 4 >>=, OPERATOR 5 >>>,
		FUNCTION 1 oic_cmp(int4, int4);
	CREATE TABLE oic (k int);
	INSERT INTO oic SELECT i % 5000 FROM generate_series(1, 50000) i;
	CREATE INDEX oic_k ON oic USING lion (k oic_ops);
}

teardown
{
	DROP TABLE oic;
	DROP OPERATOR CLASS oic_bt USING btree;
	DROP OPERATOR FAMILY oic_fam USING lion;
	DROP OPERATOR <<< (int4, int4);
	DROP OPERATOR <<= (int4, int4);
	DROP OPERATOR === (int4, int4);
	DROP OPERATOR >>= (int4, int4);
	DROP OPERATOR >>> (int4, int4);
	DROP FUNCTION oic_lt(int4, int4), oic_le(int4, int4), oic_eq(int4, int4),
		oic_ge(int4, int4), oic_gt(int4, int4), oic_cmp(int4, int4);
}

# The backend that has the index open.
session s1
step s1_ordered	{ SELECT ordered FROM lion_index_stats('oic_k'); }
step s1_count	{ SELECT lion_index_count('oic_k', 7); }

# The one that changes the comparison.
session s2
step s2_desc	{
	CREATE OR REPLACE FUNCTION oic_cmp(int4, int4) RETURNS int4 LANGUAGE sql
		IMMUTABLE STRICT AS 'SELECT btint4cmp($2, $1)';
}
step s2_std		{
	CREATE OR REPLACE FUNCTION oic_cmp(int4, int4) RETURNS int4 LANGUAGE sql
		IMMUTABLE STRICT RETURN btint4cmp($1, $2);
}
step s2_restore	{
	CREATE OR REPLACE FUNCTION oic_cmp(int4, int4) RETURNS int4 LANGUAGE sql
		IMMUTABLE STRICT AS 'SELECT btint4cmp($1, $2)';
}

# A string body replaced under an open index: the REINDEX error, then the
# answer again once the original body is back.
permutation s1_ordered s1_count s2_desc s1_count s2_restore s1_count

# ... and a SQL-standard body in its place, which has no source to compare.
permutation s1_count s2_std s1_count s2_restore s1_count
