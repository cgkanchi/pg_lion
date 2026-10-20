-- The decoder of a LionCount plan's custom_private (src/lion_plan_private.c),
-- and its check of the rules between members, handed lists no planner makes:
-- each one a plan of one equality clause with one thing changed, which they
-- must refuse rather than read at the wrong offsets.  The first two are the lists a planner does make, at the plan
-- and the path stage.
\set VERBOSITY terse
SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS pg_lion;
CREATE EXTENSION IF NOT EXISTS lion_hooktest;

SELECT lion_hooktest_priv_decode('plan') AS nclause;
SELECT lion_hooktest_priv_decode('path') AS nclause;

-- the shape marker, and the plan's number of members
SELECT lion_hooktest_priv_decode('magic');
SELECT lion_hooktest_priv_decode('path at plan');

-- a member's node type, and its length against the number of clauses
SELECT lion_hooktest_priv_decode('oids as ints');
SELECT lion_hooktest_priv_decode('short oids');
SELECT lion_hooktest_priv_decode('partition length');
SELECT lion_hooktest_priv_decode('or length');

-- a kind there is no such clause of
SELECT lion_hooktest_priv_decode('clause kind');

-- an expression column VCOLS does not have, and one that is no expression
SELECT lion_hooktest_priv_decode('expression column');
SELECT lion_hooktest_priv_decode('bare expression column');

-- the members that grow at plan time, still the path's
SELECT lion_hooktest_priv_decode('path join at plan');
SELECT lion_hooktest_priv_decode('path wagg at plan');
-- a target list that reads state the plan has none of
SELECT lion_hooktest_priv_decode('aggregate without aggregates');
SELECT lion_hooktest_priv_decode('distinct count without its column');
-- a dimension folded into a fact filter whose IN list is the join key's
SELECT lion_hooktest_priv_decode('join filter on its key');
-- the anti join over a small inner side without the child it reads, with no
-- key to look up, and a column of its inner side in a plan without one
SELECT lion_hooktest_priv_decode('anti join without its child');
SELECT lion_hooktest_priv_decode('anti join of no key');
SELECT lion_hooktest_priv_decode('inner column without an anti join');
