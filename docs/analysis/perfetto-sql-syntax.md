# PerfettoSQL Syntax
*This page documents the syntax of PerfettoSQL, a dialect of SQL used in trace
processor and other Perfetto analysis tools to query traces.*

PerfettoSQL is a direct descendent of the
[dialect of SQL implemented by SQLite](https://www.sqlite.org/lang.html).
Specifically, any SQL valid in SQLite is also valid in PerfettoSQL.

Unfortunately, the SQLite syntax alone is not sufficient for two reasons:
1. It is quite basic e.g. it does not support creating functions or macros
2. It cannot be used to access features which are only available in Perfetto
tooling e.g. it cannot be used to create efficient analytic tables, import
modules from the PerfettoSQL standard library etc.

For this reason, PerfettoSQL adds new pieces of syntax which make the experience
of writing SQL queries better. All such additons include the keyword `PERFETTO`
to make it clear that they are PerfettoSQL-only.

<!-- TODO(b/290185551): we should really talk about our "recommendations" (e.g.
using CREATE PERFETTO TABLE instead of CREATE TABLE) somewhere and reference it
here. -->

## Including PerfettoSQL modules
`INCLUDE PERFETTO MODULE` is used to import all tables/views/functions/macros
defined in a PerfettoSQL module (e.g. from the
[PerfettoSQL standard library](/docs/analysis/stdlib-docs.autogen)).

Note that this statement acts more similar to `#include` statements in C++
rather than `import` statements from Java/Python. Specifically, all objects
in the module become available in the global namespace without being qualified
by the module name.

Example:
```sql
-- Include all tables/views/functions from the android.startup.startups module
-- in the standard library.
INCLUDE PERFETTO MODULE android.startup.startups;

-- Use the android_startups table defined in the android.startup.startups
-- module.
SELECT *
FROM android_startups;
```

For interactive development, the key can contain a wildcards:
```sql
-- Include all modules under android/.
INCLUDE PERFETTO MODULE android.*;

-- Or all stdlib modules:
INCLUDE PERFETTO MODULE *;

-- However, note, that both patterns are not allowed in stdlib.
```

## Defining functions
`CREATE PEFETTO FUNCTION` allows functions to be defined in SQL. The syntax is
similar to the syntax in PostgreSQL or GoogleSQL.

<!-- TODO(b/290185551): talk about different possible argument/return types. -->

Example:
```sql
-- Create a scalar function with no arguments.
CREATE PERFETTO FUNCTION constant_fn() RETURNS INT AS SELECT 1;

-- Create a scalar function taking two arguments.
CREATE PERFETTO FUNCTION add(x INT, y INT) RETURNS INT AS SELECT $x + $y;

-- Create a table function with no arguments
CREATE PERFETTO FUNCTION constant_tab_fn()
RETURNS TABLE(ts LONG, dur LONG) AS
SELECT column1 as ts, column2 as dur
FROM (
  VALUES
  (100, 10),
  (200, 20)
);

-- Create a table function with one argument
CREATE PERFETTO FUNCTION sched_by_utid(utid INT)
RETURNS TABLE(ts LONG, dur LONG, utid INT) AS
SELECT ts, dur, utid
FROM sched
WHERE utid = $utid;
```

## Creating efficient tables
`CREATE PERFETTO TABLE` allows defining tables optimized for analytic queries
on traces. These tables are both more performant and more memory efficient than
SQLite native tables created with `CREATE TABLE`.

Note however the full feature set of `CREATE TABLE` is not supported:
1. Perfetto tables cannot be inserted into and are read-only after creation
2. Perfetto tables must be defined and populated using a `SELECT` statement.
  They cannot be defined by column names and types.

Example:
```sql
-- Create a Perfetto table with constant values.
CREATE PERFETTO TABLE constant_table AS
SELECT column1 as ts, column2 as dur
FROM (
  VALUES
  (100, 10),
  (200, 20)
);

-- Create a Perfetto table with a query on another table.
CREATE PERFETTO TABLE slice_sub_table AS
SELECT *
FROM slice
WHERE name = 'foo';
```

### Schema

Perfetto tables can have an optional explicit schema. The schema syntax is the
same as the function argument or returned-from-a-function table,
i.e. a comma-separated list of (column name, colum type) pairs in parenthesis
after table or view name.

```sql
CREATE PERFETTO TABLE foo(x INT, y STRING) AS
SELECT 1 as x, 'test' as y
```

### Index

`CREATE PERFETTO INDEX` lets you create indexes on Perfetto tables, similar to
how you create indexes in SQLite databases. These indexes are built on specific
columns, and Perfetto internally maintains these columns in a sorted order.
This means operations benefiting from sorting on an indexed column (or group of
columns) will be significantly faster, as if you were operating on a column
that's already sorted.

NOTE: Indexes have non-trivial memory cost, so it's important to only use them
when there is a need for performance improvement.

NOTE: Indexes will be used by views created on the indexed table, but they will
not be inherited by any child tables, as shown in the below SQL.

NOTE: If the query filters/joins on `id` column of the table (one that is a
primary key of the table) there is no need to add a Perfetto index, as Perfetto
tables already have special performance optimizations for operations that can
benefit from sorting.

Example of usage:
```sql
CREATE PERFETTO TABLE foo AS
SELECT * FROM slice;

-- Creates and stores an index `foo_track` on column `track_id` of table foo.
CREATE PERFETTO INDEX foo_track ON foo(track_id);
-- Creates or replaces an index created on two columns. It will be used for
-- operations on `track_id` and can be used on operations on `name` only if
-- there has been an equality constraint on `track_id` too.
CREATE OR REPLACE PERFETTO INDEX foo_track_and_name ON foo(track_id, name);
```

The performance of those two queries should be very different now:
```sql
-- This doesn't have an index so it will have to linearily scan whole column.
SELECT * FROM slice WHERE track_id = 10 AND name > "b";

-- This has an index and can use binary search.
SELECT * FROM foo WHERE track_id = 10 AND name > "b";

-- The biggest difference should be noticeable on joins:
-- This join:
SELECT * FROM slice JOIN track WHERE slice.track_id = track.id;
-- will be noticeably slower than this:
SELECT * FROM foo JOIN track WHERE slice.track_id = track.id;
```

Indexes can be dropped:
```sql
DROP PERFETTO INDEX foo_track ON foo;
```


## Creating views with a schema

Views can be created via `CREATE PERFETTO VIEW`, taking an optional schema.
With the exception of the schema, they behave exactly the same as regular
SQLite views.

NOTE: the use of `CREATE PERFETTO VIEW` instead of `CREATE VIEW` is required in
the standard library where each column must be documented.

```sql
CREATE PERFETTO VIEW foo(x INT, y STRING) AS
SELECT 1 as x, 'test' as y
```

## Defining macros
`CREATE PEFETTO MACRO` allows macros to be defined in SQL. The design of macros
is inspired by the macros in Rust.

The following are recommended uses of macros:
- Passing tables as arguments to a "function-like" snippet of SQL.

Macros are powerful but also dangerous if used incorrectly, making debugging
extremely difficult. For this reason, it's recommended that they are used
sparingly when they are needed and only for the recommended uses described
above. If only passing around scalar SQL values, use functions as discussed
above.

NOTE: Macros are expanded with a pre-processing step *before* any execution
happens. Expansion is a purely syntatic operation involves replacing the macro
invocation with the SQL tokens in the macro definition.

As macros are syntactic, the types of arguments and return types in macros are
different to the types used in functions and correspond to parts of the SQL
parse tree. The following are the supported types:

| Type name         | Description                                       |
| ---------         | -----------                                       |
| `Expr`            | Corresponds to any SQL scalar expression.         |
| `TableOrSubquery` | Corresponds to either an SQL table or a subquery  |
| `ColumnName`      | Corresponds to a column name of a table           |

Example:
```sql
-- Create a macro taking no arguments. Note how the returned SQL fragment needs
-- to be wrapped in brackets to make it a valid SQL expression.
--
-- Note: this is a strongly discouraged use of macros as a simple SQL
-- function would also work here.
CREATE PERFETTO MACRO constant_macro() RETURNS Expr AS (SELECT 1);

-- Using the above macro. Macros are invoked by suffixing their names with !.
-- This is similar to how macros are invoked in Rust.
SELECT constant_macro!();

-- This causes the following SQL to be actually executed:
-- SELECT (SELECT 1);

-- A variant of the above. Again, strongly discouraged.
CREATE PERFETTO MACRO constant_macro_no_bracket() RETURNS Expr AS 2;

-- Using the above macro.
SELECT constant_macro_no_bracket!();

-- This causes the following SQL to be actually executed:
-- SELECT 2;

-- Creating a macro taking a single scalar argument and returning a scalar.
-- Note: again this is a strongly discouraged use of macros as functions can
-- also do this.
CREATE PERFETTO MACRO single_arg_macro(x Expr) RETURNS Expr AS (SELECT $x);
SELECT constant_macro!() + single_arg_macro!(100);

-- Creating a macro taking both a table and a scalar expression as an argument
-- and returning a table. Note again how the returned SQL statement is wrapped
-- in brackets to make it a subquery. This allows it to be used anywhere a
-- table or subquery is allowed.
--
-- Note: if tables are reused multiple times, it's recommended that they be
-- "cached" with a common-table expression (CTE) for performance reasons.
CREATE PERFETTO MACRO multi_arg_macro(x TableOrSubquery, y Expr)
RETURNS TableOrSubquery AS
(
  SELECT input_tab.input_col + $y
  FROM $x AS input_tab;
)
```