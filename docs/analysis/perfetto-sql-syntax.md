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

## CREATE PERFETTO VIEW

Views can be created via `CREATE PERFETTO VIEW`, taking an optional schema.
With the exception of the schema, they behave exactly the same as regular
SQLite views.

```sql
CREATE PERFETTO VIEW foo(x INT, y STRING) AS
SELECT 1 as x, 'test' as y
```