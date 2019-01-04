--
-- multi function in join queries aims to test the function calls that are
-- used in joins.
--
-- These functions are supposed to be executed on the worker and to ensure
-- that we wrap those functions inside (SELECT * FROM fnc()) sub queries.
--
-- We do not yet support those functions that:
--  - return records
--  - return tables
--  - are user-defined and immutable

CREATE SCHEMA functions_in_joins;
SET search_path TO 'functions_in_joins';
SET citus.next_shard_id TO 2500000;

CREATE TABLE table1 (id int, data int);
SELECT create_distributed_table('table1','id');

INSERT INTO table1
SELECT x, x*x
from generate_series(1, 100) as f (x);

-- Verbose messages for observing the subqueries that wrapped function calls
SET client_min_messages TO DEBUG;

-- Check joins on a sequence
CREATE SEQUENCE numbers;
SELECT * FROM table1 JOIN nextval('numbers') n ON (id = n);

-- Check joins of a function that returns a single integer
CREATE FUNCTION add(integer, integer) RETURNS integer
AS 'SELECT $1 + $2;'
LANGUAGE SQL;
SELECT * FROM table1 JOIN add(3,5) sum ON (id = sum);

-- Check join of plpgsql functions
-- a function returning a single integer
CREATE OR REPLACE FUNCTION increment(i integer) RETURNS integer AS $$
BEGIN
  RETURN i + 1;
END;
$$ LANGUAGE plpgsql;
SELECT * FROM table1 JOIN increment(2) val ON (id = val);

-- a function that returns a set of integers
CREATE OR REPLACE FUNCTION next_k_integers(IN first_value INTEGER,
                                           IN k INTEGER DEFAULT 3,
                                           OUT result INTEGER)
  RETURNS SETOF INTEGER AS $$
BEGIN
  RETURN QUERY SELECT x FROM generate_series(first_value, first_value+k-1) f(x);
END;
$$ LANGUAGE plpgsql;
SELECT *
FROM table1 JOIN next_k_integers(3,2) next_integers ON (id = next_integers.result)
ORDER BY id ASC;

-- a stable function
CREATE OR REPLACE FUNCTION the_minimum_id()
  RETURNS INTEGER STABLE AS 'SELECT min(id) FROM table1' LANGUAGE SQL;
SELECT * FROM table1 JOIN the_minimum_id() min_id ON (id = min_id);

-- a built-in immutable function
SELECT * FROM table1 JOIN abs(100) as hundred ON (id = hundred);

-- function joins inside a CTE
WITH next_row_to_process AS (
    SELECT * FROM table1 JOIN nextval('numbers') n ON (id = n)
    )
SELECT *
FROM table1, next_row_to_process
WHERE table1.data <= next_row_to_process.data;


-- The following tests will fail as we do not support  all joins on
-- all kinds of functions
SET client_min_messages TO ERROR;

-- function joins in CTE results can create lateral joins that are not supported
SELECT public.raise_failed_execution($cmd$
WITH one_row AS (
    SELECT * FROM table1 WHERE id=52
    )
SELECT table1.id, table1.data
FROM one_row, table1, next_k_integers(one_row.id, 5) next_five_ids
WHERE table1.id = next_five_ids;
$cmd$);

-- a function returning table
CREATE FUNCTION get_two_column_table() RETURNS TABLE(x int,y int) AS
$cmd$
SELECT x, x+1 FROM generate_series(0,4) f(x)
$cmd$
LANGUAGE SQL;
SELECT public.raise_failed_execution($cmd$
SELECT * FROM table1 JOIN get_two_column_table() t2 ON (id = x)
$cmd$);

-- a function returning records
CREATE FUNCTION get_set_of_records() RETURNS SETOF RECORD AS $cmd$
SELECT x, x+1 FROM generate_series(0,4) f(x)
$cmd$
LANGUAGE SQL;
SELECT public.raise_failed_execution($cmd$
SELECT * FROM table1 JOIN get_set_of_records() AS t2(x int, y int) ON (id = x)
$cmd$);

-- a user-defined immutable function
CREATE OR REPLACE FUNCTION the_answer_to_life()
  RETURNS INTEGER IMMUTABLE AS 'SELECT 42' LANGUAGE SQL;
SELECT public.raise_failed_execution($cmd$
SELECT * FROM table1 JOIN the_answer_to_life() the_answer ON (id = the_answer)
$cmd$);

-- Multiple functions in an RTE
-- NOTE: Wrapping the functions in subqueries does not work as postgres does
-- not allow having a subquery after a ROWS FROM clause.
SELECT public.raise_failed_execution($cmd$
SELECT * FROM ROWS FROM (next_k_integers(5), next_k_integers(10)) AS f(a, b),
    table1 WHERE id = a;
$cmd$);

-- WITH ORDINALITY clause forcing the result type to be RECORD/RECORDs
SELECT public.raise_failed_execution($cmd$
SELECT *
FROM table1
       JOIN next_k_integers(10,5) WITH ORDINALITY next_integers
         ON (id = next_integers.result)
ORDER BY id ASC;
$cmd$);


RESET client_min_messages;
DROP SCHEMA functions_in_joins CASCADE;
SET search_path TO DEFAULT;
