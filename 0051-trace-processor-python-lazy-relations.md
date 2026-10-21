# Lazy relations and DuckDB interop for the Trace Processor Python API

**Authors:** @LalitMaganti

**Status:** Draft

**PR:** N/A

## Problem

The Trace Processor Python API is built around `tp.query(sql)`. The call runs
the query to completion and returns every row. This makes three things harder
than they should be.

**Results are slow to convert and lose their types.** Results travel as the
row-major `QueryResult.CellsBatch` proto, which has a type tag per cell.
`as_pandas_dataframe()` builds a NumPy array of Python objects from it, so
every pandas column has the `object` dtype. `as_polars_dataframe()` goes
through Python lists and produces polars `Object` columns. Neither can be
exported as Arrow. This makes every hand-off to another tool both slow and
lossy.

**Tools working on a TP result cannot push work back into TP.** Users
increasingly combine TP with DuckDB or polars: they join trace data with
experiment metadata, write Parquet, or keep analysing in a dataframe library.
Today they first materialise the whole TP result in Python, then filter it in
the other tool. Any filter or projection written in DuckDB or polars runs after
TP has already produced every row and every column.

**There is no lazy handle on a query.** A query result cannot be passed around
and composed before it runs, or used as a table inside another query.

DuckDB's Python API is the reference point for what users expect. Its relations
are lazy. Arrow is its interchange format. Python objects can be referenced from
SQL by name. Its query engine pushes filters and projections into the sources it
reads.

## Decision

Pending

## Design

The design has four parts. Each part is useful without the parts after it.

1. Arrow as a query result format.
2. A compile-only statement RPC.
3. `tp.lazy_query()`: a lazy relation over a PerfettoSQL query.
4. Filter and projection pushdown from DuckDB into relations.

`tp.query()` does not change. Its eager behaviour, its error behaviour and its
result type stay exactly as they are today.

### 1. Arrow results

Add a result format field to `QueryArgs`. When the client asks for Arrow, the
server encodes each result as an Arrow IPC stream instead of `CellsBatch`
messages. The server reuses `core/dataframe/arrow_serializer.h`, which already
backs `export(..., 'arrow_tar')`.

SQLite is dynamically typed, so the server must pick one Arrow type per column:

* A column backed by a typed dataframe column keeps that column's type. This
  covers tables, `PERFETTO TABLE`s and views which only select their columns.
* Any other column is typed from its first non-null value. Integers are
  promoted to doubles if doubles appear later. Any other mix of types is an
  error. The error message names the column and suggests a `CAST`.
* A column with only nulls is typed as null.

A server which does not know the field answers with `invalid_request`. The
client then falls back to `CellsBatch`, so a new client works against an old
`trace_processor_shell`.

On the Python side, query results gain `as_arrow_table()` and the Arrow
PyCapsule `__arrow_c_stream__` protocol. Through the PyCapsule protocol,
pyarrow, polars, pandas and DuckDB can all consume a result without code
specific to each library.

This lands in two steps. The first step converts the `CellsBatch` cells to
Arrow in Python, typing each column from the cell types in it. This gives typed
interop immediately, with no protocol change. The server-side encoding above is
the second step, and makes the conversion faster without changing the Python
API.

`as_pandas_dataframe()` and `as_polars_dataframe()` do not change, for backwards
compatibility. Their columns keep the `object` dtype. Typed pandas and polars
output comes from `tp.lazy_query()`, described in part 3.

### 2. Compile-only statement RPC

Laziness needs three things before execution: the query's schema, its errors,
and the separation of setup statements from the final query. Add a mode to
`TPM_STATEMENT_STREAMING` or a new method. Given a multi-statement query, the
server will:

1. Execute every statement except the last.
2. Compile the last statement without stepping it.
3. Return the column names and types, whether the statement is read-only, and
   any error.

A statement counts as read-only only if SQLite reports it as such *and* it calls
no function with side effects. TP will mark functions with side effects
explicitly. Today these are `RUN_METRIC` and the file I/O intrinsics.

The existing statement-at-a-time RPC is not enough on its own: it reports where
a statement ends only after executing it.

### 3. `tp.lazy_query()`: lazy relations

`tp.lazy_query(sql)` returns a `LazyQuery`. The name says what distinguishes it
from `tp.query()`: nothing runs until the result is consumed. A shorter name
such as `sql()` would read as a synonym for `query()`.

Its semantics deliberately match DuckDB's `con.sql()`. These were verified
against DuckDB 2.0.0:

| Behaviour | DuckDB 2.0 `con.sql()` | `tp.lazy_query()` |
| --- | --- | --- |
| Statements that change state | run at the call | run at the call |
| Final `SELECT` | lazy relation | lazy relation |
| Syntax and binding errors | raised at the call | raised at the call |
| Columns and types | known before execution | known before execution |
| Consuming twice | runs twice; no cache | runs twice; no cache |
| Later changes to the database | visible | visible |
| Side-effecting final `SELECT` | lazy | run at the call |

The last row is the one deliberate difference. A final statement which is not
read-only, as defined in part 2, runs at the call. This keeps
`tp.lazy_query("SELECT RUN_METRIC(...)")` working the way users expect.

Consuming a relation runs its query. A relation is consumed by iterating it, by
`.df()`, `.pl()` and `.arrow()`, and through `__arrow_c_stream__`.
`.to_table(name)` materialises the relation as a `PERFETTO TABLE` for explicit
caching.

Results are typed by default. `.arrow()` and `.pl()` give each column an Arrow
or polars type, with NULLs as nulls. `.df()` converts through Arrow, so pandas
columns get numeric dtypes even when they contain NULLs. `lazy_query()` has no
existing callers, so this needs none of the compatibility compromises that
`as_pandas_dataframe()` would.

Relations compose inside TP. Passing a relation by name, as in
`tp.lazy_query("SELECT * FROM binder WHERE dur > 1e6", binder=rel)`, inlines the
relation's SQL as a subquery. Both relations live in the same TP instance, so
this costs nothing. The outer query's filters are then pushed down by SQLite;
see
[Why wrapping a query preserves pushdown](#why-wrapping-a-query-preserves-pushdown).

Relations have no dataframe-style methods such as `.filter()` or `.select()`.
Users compose relations in SQL, or push down from DuckDB as described next.

### 4. DuckDB pushdown into relations

The goal is for DuckDB to query a relation like a table, with DuckDB's filters
and projections pushed into TP:

```python
binder = tp.lazy_query("""
  INCLUDE PERFETTO MODULE slices.with_context;
  SELECT ts, dur, thread_name, depth
  FROM thread_slice WHERE name GLOB 'binder*'
""")
duckdb.sql("SELECT thread_name, dur FROM binder WHERE dur > 5e6 AND depth = 0")
```

TP receives the user's query wrapped with DuckDB's projection and filters:

```sql
SELECT "dur", "thread_name", "depth"
FROM (SELECT ts, dur, thread_name, depth
      FROM thread_slice WHERE name GLOB 'binder*')
WHERE ("depth" = 0) AND ("dur" > 5000000)
```

#### Mechanism

In duckdb-python 2.0, the object behind a Python variable can take part in
DuckDB's optimizer by being an instance of `ArrowScanSource`. For each filter it
is willing to push, DuckDB calls `accepts(predicate)`. The predicate is a small
tree of columns, literals, comparisons, `AND`, `OR`, `NOT`, `IN` and
`IS [NOT] NULL`. DuckDB then calls `stream(columns, filters)` with the projected
columns and the accepted filters, and expects an Arrow stream back. DuckDB's own
polars and pyarrow support is built on this protocol.

The protocol lives in the private module `duckdb._sources`. It is undocumented
and its only users are duckdb-python's own tests. We accept this dependency
because the integration below degrades gracefully: if the protocol breaks,
queries get slower, not wrong.

#### Integration

* **Feature detection.** On import, the integration tries
  `from duckdb._sources import ArrowScanSource, nodes`. It checks that the
  pieces it uses exist. It does not pin DuckDB versions.
* **Optional base class.** `LazyQuery` derives from `ArrowScanSource` only when
  the protocol is available. Otherwise it is a plain object with
  `__arrow_c_stream__`. DuckDB can still query it by variable name, and reads
  the full result.
* **Default to refusing.** `accepts()` refuses anything it does not recognise:
  unknown node types, literal types outside an allow-list, and any exception
  raised during translation. A refused predicate stays in DuckDB.
* **Semantic equivalence.** A predicate is accepted only when DuckDB and SQLite
  evaluate it identically. That means integer, float and string comparisons,
  `IN`, `IS [NOT] NULL` and the boolean connectives. NaN literals are refused
  because SQLite has no NaN. Boolean literals are refused, as is any literal
  whose type mapping is ambiguous.
* **Tolerant signatures.** `stream()` accepts `*args` and `**kwargs`, so a new
  optional argument does not break it.
* **Off switch.** Pushdown can be disabled with
  `tp.lazy_query(..., duckdb_pushdown=False)` or with an environment variable.

#### Error reporting

* If `duckdb>=2` is installed but the protocol is missing, the integration warns
  once. The warning says that pushdown is unavailable for that DuckDB version,
  that results are still correct, and where the documentation is. It stays
  silent when DuckDB is absent or older than 2.0.
* An exception raised inside the source is re-raised with Perfetto context: the
  operation, how to disable pushdown, and where to report it.

#### Documentation and testing

* The Python API docs describe what is pushed down, the dependency on DuckDB
  internals and the tested versions. They show how to verify pushdown with
  `EXPLAIN`, which lists `Filters:` under `PYTHON_ARROW_SCAN`, and how to
  disable it.
* A CI job runs against the latest DuckDB release. It asserts that pushdown
  actually happens, so a protocol change fails loudly in our CI before users
  notice.
* A differential test runs each query with and without pushdown and requires
  identical results. It covers NULLs, strings, mixed numeric types and empty
  results.

#### Why wrapping a query preserves pushdown

Pushdown only pays off if SQLite carries the outer filter into the inner query.
Measured on `test/data/android_startup_real.perfetto_trace` with
`SELECT * FROM (<base>) WHERE <filter>`:

| Base query | Filter pushed into TP | Wrapped | Full base |
| --- | --- | --- | --- |
| Table `slice` | yes | 0.1 ms | 3.2 ms |
| View with joins `thread_slice` | yes | 0.1 ms | 3.4 ms |
| Explicit `LEFT JOIN` | yes | 0.1 ms | 16.6 ms |
| `GROUP BY`, filter on the key | yes | 0.4 ms | 28.9 ms |
| Window, filter on the partition key | yes | 0.4 ms | n/a |
| `GROUP BY`, filter on an aggregate | no | 34.6 ms | 29.8 ms |
| Window, filter on another column | no | 159 ms | 159 ms |
| `LIMIT` inside the base | no | 25 ms | 11 ms |

The cases which are not pushed are the ones where pushing would change the
result. DuckDB stops at the same boundaries.

#### Limits

DuckDB does not offer `LIKE`, `GLOB`, `LIMIT` or join-derived filters to
sources. These run in DuckDB after TP has produced the rows.

#### Upstream

We will ask duckdb-python to make the source protocol public, ideally through a
dunder such as `__duckdb_source__()` so that objects can opt in without
subclassing. We will also ask for prefix `LIKE` and `LIMIT` to be offered to
sources. A public protocol removes the main risk of this design. Our prototype
is a concrete second user to point to.

## Alternatives considered

### Make `tp.query()` lazy

Pro:

* No new API.

Con:

* `query()` is called purely for its side effects. In this repository,
  `breakdown.py` runs `DROP VIEW`, `INCLUDE` and `CREATE VIEW` through it and
  never reads the result. A lazy `query()` would silently skip these calls.
* `SELECT`s with side effects exist, such as `RUN_METRIC` and the file I/O
  intrinsics. So DuckDB's rule of making only `SELECT`s lazy is not safe either.
* Errors would move from the call to the first use. `api_integrationtest.py`
  relies on errors being raised at the call.
* Results returned out of a `with TraceProcessor(...)` block would run after the
  instance is closed.

Making it safe needs flushing pending results before every state change and on
`close()`. An opt-in `tp.lazy_query()` with DuckDB's semantics is simpler and
breaks nothing.

### A dataframe-style relation API

This means methods such as `.filter()`, `.select()` and `.aggregate()` which
compile to SQL. TP already pushes predicates through wrapped queries, so the
compilation would be easy.

Con:

* It duplicates what users can already write in SQL.
* It adds a large API surface to maintain.
* It does not address the actual need: pushdown from the tools users already
  use.

### A native DuckDB extension

This is the prototype in #7757, #7758 and #7759. It covers `ATTACH`,
`perfetto_query()`, filter pushdown and `PERFETTO(t) (...)` blocks. We evaluated
it against DuckDB 2.0.0.

Pro:

* It works in the DuckDB CLI and in every DuckDB client, not only Python.
* It supports running one SQL statement over many traces in parallel.
* DuckDB 2.0 grammar extensions can replace `parser_override` for block syntax.
  A test extension parsed multi-statement blocks containing strings, comments,
  macros and `$` parameters, and loaded into both the stock CLI and the Python
  client.

Con:

* C API v2 ships with every function marked unstable (duckdb/duckdb#27410). So
  even an extension built on it is tied to the exact DuckDB version.
* Grammar extensions and `ATTACH` are C++-only.
* The stock 2.0 CLI exports no DuckDB symbols. Extensions must statically link
  DuckDB, which made the test extension 14 MB. They must also be built against
  libstdc++, while Perfetto's standalone Linux build defaults to its own libc++.
* Block syntax has rough edges. The interactive CLI splits a block at any line
  ending in `;`. duckdb-python's `.sql()` rejects `$name` inside blocks.
  Comments in the body are lost.

We defer the extension until C API v2 functions are marked stable. At that point
the core functions can be ported to it and published as a community extension.

### A polars LazyFrame bridge

`tp.lazy_query()` could return a polars LazyFrame built with the public
`register_io_source`. DuckDB pushes filters into LazyFrames, and polars passes
them on to the IO source. A prototype pushed DuckDB filters and projections into
TP using only public APIs.

Con:

* Polars passes predicates as expressions. The only way to inspect them is
  `Expr.meta.serialize(format="json")`, whose format polars does not keep stable
  across versions. `IN` lists arrive as Arrow IPC bytes.
* It makes polars a hard dependency, even for users who only want DuckDB.
* Adding methods of our own requires subclassing LazyFrame, which relies on
  polars internals.

### Expose TP tables to DuckDB directly

This means a source per TP table, with DuckDB building the query. A prototype
worked using the same protocol as part 4.

Con:

* The unit users work with is a PerfettoSQL query, which includes stdlib
  modules, macros and table functions, not a raw table.
* DuckDB and SQLite semantics differ for parts of the query that DuckDB would
  run, such as integer division, NULL ordering and collation.

### Compile relations to PerfettoSQL pipe syntax

Each relation step would map onto a pipe stage. The pipeline executor reads TP
dataframes directly.

Con:

* Pipe syntax is experimental.
* Its `WHERE` stage only supports numeric comparisons joined by `AND`.
* It has no `LIMIT` or join stage.
* It cannot appear inside a subquery.

It remains a possible backend for relations later. Adopting it would not change
the Python surface.

## Open questions

* Which pandas dtypes should `LazyQuery.df()` use for columns with NULLs:
  pandas' nullable dtypes such as `Int64`, or `ArrowDtype`? Both represent NULL
  as `pd.NA`, which behaves differently from `None` in boolean contexts.
* Does type inference by first non-null value hold up on real stdlib output, or
  should columns of mixed types become strings instead of an error?
* Which functions besides `RUN_METRIC` and the file I/O intrinsics need the
  side-effect flag?
* How should `BatchTraceProcessor` expose relations? One relation per trace, a
  union, or both?
* Should `register(name, df)` be part of this RFC? It would upload Arrow data
  for use inside PerfettoSQL using the existing `DeserializeFromArrow`.
* Is `ArrowScanSource` likely to change shape before duckdb-python 2.1? It is
  worth asking upstream before building on it.
