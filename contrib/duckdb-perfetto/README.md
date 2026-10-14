# PerfettoSQL inside DuckDB (prototype)

**Status:** Exploratory prototype. Not yet a proposed contrib/ project: it has
no OWNERS.github and has not gone through the contrib/ proposal process (see
[../README.md](../README.md)).

A DuckDB extension which embeds Perfetto's Trace Processor (TP) so that traces
can be attached to a DuckDB database, and so that **PerfettoSQL can be written
inline in DuckDB queries, without string literals, and executed entirely
inside TP**.

```sql
LOAD 'perfetto';  -- or LOAD '/abs/path/to/perfetto.duckdb_extension'

-- A trace is a read-only database. Every TP table is a DuckDB table.
ATTACH 'trace.pftrace' AS t (TYPE perfetto);

SELECT p.name, sum(s.dur) AS cpu_ns
FROM t.sched s JOIN t.thread USING (utid) JOIN t.process p USING (upid)
GROUP BY 1 ORDER BY 2 DESC;

-- A PERFETTO block is PerfettoSQL: stdlib, INCLUDE, macros, SPAN_JOIN, and
-- multiple statements all work. The whole block runs in TP; DuckDB only
-- sees the result rows, and treats the block like any other table.
SELECT s.package, s.dur, d.build_id
FROM PERFETTO(t) (
  INCLUDE PERFETTO MODULE android.startup.startups;
  SELECT * FROM android_startups WHERE dur > 100e6
) AS s
JOIN device_metadata d USING (package);

-- Anything TP creates becomes visible in the catalog.
SELECT count(*) FROM t.android_startups;

-- Materialize: plain DuckDB.
CREATE TABLE slices AS FROM t.slice;
COPY (PERFETTO(t) (SELECT * FROM thread_state)) TO 'thread_state.parquet';

-- Bulk: one PerfettoSQL query over many traces, in parallel.
SELECT trace, count(*), max(dur)
FROM perfetto_query('traces/*.pftrace', 'SELECT dur FROM slice WHERE depth = 0')
GROUP BY trace;
```

## Design

### Why a block, not implicit routing

The goal is to write PerfettoSQL directly, with no string subqueries. There
are two ways to get there:

1. **Implicit routing.** Write DuckDB SQL, and have the extension work out
   which parts can run in TP and send them there.
2. **An explicit block.** `PERFETTO(t) ( ... )` marks a region that is
   PerfettoSQL.

This prototype does (2). Implicit routing is unsafe: the same SQL text means
different things in DuckDB and SQLite (integer `/`, `||` with NULL, NULL
ordering, collation, and `LIKE` case sensitivity), so silently moving part of a
query between engines changes results. PerfettoSQL is also heading away from
DuckDB's grammar (pipe syntax, interval/tree/graph operators; see RFC-0040),
so DuckDB can't even parse it. A block boundary makes it obvious which
semantics apply, while everything inside the block is full, unrestricted
PerfettoSQL.

Composition comes free because a block is just a table expression. It works
with joins, CTEs, `CREATE TABLE AS`, `COPY`, `INSERT`, subqueries, and every
DuckDB client.

### How the syntax is implemented

DuckDB has two parser hooks for extensions:

* `parse_function` (fallback) only runs for statements DuckDB's parser
  rejects, **after** DuckDB has split the input on `;`. A block containing
  `INCLUDE ...; SELECT ...` would be cut in half before the extension saw it.
* `parser_override` sees the whole query string first and returns parsed
  statements. **This is the one we use.** It is also how DuckDB's own new
  PEG parser plugs in.

In [`block_rewriter.cc`](src/block_rewriter.cc), each dialect is handled by
its own engine's front end:

* **The outer query** is tokenized by **DuckDB's own tokenizer**
  (`Parser::Tokenize`). This finds each `PERFETTO(alias) (` opener and the
  token before it, with DuckDB's exact rules for `$$` strings, nested
  comments, and `E''` strings.
* **The block body** is tokenized by **syntaqlite's PerfettoSQL tokenizer**
  to find the matching close paren. syntaqlite is TP's own SQL front end, so
  `;`, strings, comments, quoted identifiers, and macro calls inside the block
  lex exactly as TP will lex them. Outer tokenization resumes after each
  block's closing paren.
* **The body is then parsed** with **syntaqlite's PerfettoSQL parser**, from
  a pool of parsers reused across queries and connections.
  Syntax errors are reported against the original query, with DuckDB's usual
  caret, before anything runs:

  ```
  Parser Error: PerfettoSQL syntax error in PERFETTO block: syntax error near 'FROM'

  LINE 1: SELECT 1 FROM PERFETTO(t) (SELECT 1; SELECT FROM slice);
                                                      ^
  ```

Each block is then replaced with a `perfetto_query('alias', '<body>')` table
function call, and the rewritten text is handed back to DuckDB's parser.
Queries without blocks fall straight through, untouched.

| Form | Rewritten to |
| ---- | ------------ |
| `FROM PERFETTO(t) (...)`, `JOIN PERFETTO(t) (...)` | `perfetto_query('t', '...')` |
| `PERFETTO(t) (...)` as a statement, CTE body, `AS ...` | `FROM perfetto_query('t', '...')` |
| `PERFETTO (...)` (no alias) | uses the only attached perfetto database |

The rewriter takes the outer tokenizer as a parameter and is unit tested
([`block_rewriter_unittest.cc`](src/block_rewriter_unittest.cc)), with
syntaqlite standing in for DuckDB's tokenizer.

### Does DuckDB 2 solve this?

Partly, eventually. DuckDB's roadmap lists "using the new PEG parser by
default". That parser is runtime-extensible: grammar rules live in `.gram`
files (`extension/autocomplete/grammar/` in v1.5.6), and it is installed
through the same `parser_override` hook we use. Once it is the default and
extensions can contribute rules, `PERFETTO(alias) ( <opaque balanced body> )`
becomes a real grammar rule instead of a lexical pre-pass. The benefits:

* the whole query is parsed in one pass;
* the CLI's statement splitter can understand blocks (see Limitations);
* DuckDB's syntax highlighting and autocomplete can understand blocks.

As of v1.5.6 the PEG parser is opt-in and has no public API for extensions to
add grammar, so the rewriter stays isolated in one file that can be swapped
out later.

### Catalog: `ATTACH ... (TYPE perfetto)`

A DuckDB `StorageExtension` (the mechanism used by the sqlite and postgres
scanners) provides a read-only catalog with one schema, `main`:

* **Lazy table entries.** Table entries are created on lookup from TP's own
  catalog (`pragma_table_info`). So tables and views a block creates (via
  `INCLUDE PERFETTO MODULE` or `CREATE PERFETTO TABLE`) show up as `t.<name>`
  with no refresh step.
* **Lifetime.** The trace's lifetime is tied to the attached database:
  `DETACH t` frees it.
* **Projection pushdown.** TP is only asked for the columns DuckDB needs.
* **Filter pushdown** via `pushdown_complex_filter`. Column-vs-constant
  comparisons (`= != < <= > >=`), `IS [NOT] NULL` and `IN (...)` on columns
  become a PerfettoSQL `WHERE`, so TP's indexes and dataframe planner apply.
  Anything else stays in DuckDB, so pushdown never changes results.
  `EXPLAIN` shows what was pushed:

  ```
  PERFETTO_SCAN
    Table: slice
    PerfettoSQL Filters: "dur" > 1000000
  ```

### Execution

`perfetto_query(source, sql)` runs the SQL with `TraceProcessor::ExecuteQuery`
and streams the rows into DuckDB vectors. `source` can be:

* an attached alias, which runs against the resident TP; or
* a path, a glob, or a `LIST` of those. Each worker thread loads its own TP
  instance, so many traces are processed in parallel, and a `trace` column
  identifies each row's origin.

Result types come from the declared types for tables. For arbitrary queries
they are inferred from the first 2048 rows (see Limitations).

Filters and projections on a `perfetto_query` result are pushed into the
query's final `SELECT` (see [Pushdown](#pushdown)).

### Build

* **Toolchain.** The extension builds with the standard Perfetto GN
  toolchain through a separate dotfile ([`.gn`](.gn)) rooted at this
  directory, so contrib/ never touches the root `BUILD.gn`. `BUILD.gn` uses
  relative labels. The one exception is three `configs -=` entries, which must
  match the absolute labels in `default_configs`.
* **No DuckDB build needed.** It compiles against DuckDB's headers (a sparse
  checkout of the matching tag) and links with `-undefined dynamic_lookup`.
  The released DuckDB binaries export their C++ symbols, which is also how
  official extensions load.
* **Exceptions and RTTI are confined to one file.** DuckDB's C++ API needs
  both, and only [`perfetto_extension.cc`](src/perfetto_extension.cc)
  includes DuckDB headers, so only it is built with them. Everything that
  touches TP ([`trace_session.cc`](src/trace_session.cc)) uses Perfetto's
  usual flags, and errors cross the boundary as `base::Status`.

```sh
contrib/duckdb-perfetto/build.sh                       # extension + unit tests
out/duckdb_ext/perfetto_duckdb_unittests               # rewriter tests
contrib/duckdb-perfetto/e2e_test.py                    # end-to-end tests
duckdb -unsigned -c "LOAD '$PWD/out/duckdb_ext/perfetto.duckdb_extension'; ..."
```

`build.sh` pins DuckDB `v1.5.6`. On macOS, `LOAD` needs an absolute path.
`e2e_test.py` needs `pip install duckdb==1.5.6`. On Linux, Python loads
DuckDB with `RTLD_LOCAL`, hiding its symbols from extensions: call
`sys.setdlopenflags(os.RTLD_GLOBAL | os.RTLD_NOW)` before `import duckdb`.

## Chain syntax

`|>` chains stages onto a query, one step at a time, in the style of
PerfettoSQL's pipe syntax (RFC-0040; reference:
[perfetto-sql-pipe-syntax.md](../../docs/analysis/perfetto-sql-pipe-syntax.md)).
It works both inside a PERFETTO block, where it is TP's own pipe syntax, and
after one, where it is DuckDB:

```sql
FROM PERFETTO(t) (SELECT name, dur, depth FROM slice)
|> WHERE depth = 0
|> AGGREGATE count(*) AS n, sum(dur) / 1e6 AS total_ms GROUP BY name
|> ORDER BY total_ms DESC
|> LIMIT 3;
```

```text
name                     | n  | total_ms
-------------------------+----+------------
animator                 | 18 | 3550.805108
startActivityAndWait     | 1  | 1110.688706
mdss_mdp_overlay_kickoff | 76 | 895.068466
```

### The boundary stays explicit

Everything inside the block's parentheses is PerfettoSQL and runs in TP.
Every stage after the closing parenthesis is DuckDB SQL and runs in DuckDB.
The extension never moves a stage from one side to the other.

This is the same rule as for blocks themselves, for the same reason: the same
text means different things in the two engines. `7 / 2` is `3` in TP and
`3.5` in DuckDB; ascending `ORDER BY` puts NULLs first in TP and last in
DuckDB; collation and `LIKE` case sensitivity differ. With an explicit
boundary, which engine's rules apply to a stage depends only on which side of
the parenthesis it is on.

The stages available on each side today:

| Stage | Inside a block (TP) | After a block (DuckDB) |
| ----- | ------------------- | ---------------------- |
| `WHERE` | not supported yet | yes |
| `SELECT`, `EXTEND` | columns only: `col [AS name]`, `*`, `* EXCEPT (...) REPLACE (...)` | any DuckDB select items |
| `SET col = ...` | another column | any expression |
| `DROP`, `RENAME`, `AS` | yes | yes |
| `ORDER BY` | numeric columns only | any expressions |
| `AGGREGATE ... [GROUP BY ...]` | not implemented yet (see below) | any aggregates |
| `JOIN`, `LIMIT` | no | yes |
| `INTERVAL FLATTEN`, `INTERVAL INTERSECTION OF`, `TREE ACCUMULATE` | yes | no |

Inside a block, TP's pipe syntax is experimental and must first be enabled
with `PERFETTO PRAGMA pipelines = 1;`.

### Grammar

```
pipeline := FROM source [AS alias] ( |> stage )+
source   := table | table_function(...) | ( query ) | PERFETTO(t) ( ... )
stage    := WHERE expr
          | SELECT item, ...
          | EXTEND item, ...
          | SET col = expr, ...
          | DROP col, ...
          | RENAME col AS new_name, ...
          | AGGREGATE expr [AS name], ... [GROUP BY expr [AS name], ...]
          | [INNER | LEFT | RIGHT | FULL | CROSS] [OUTER] JOIN table_ref
              [ON expr | USING (col, ...)]
          | ORDER BY expr [ASC | DESC], ...
          | LIMIT n [OFFSET m]
          | AS alias
```

* A pipeline starts a query: at the start of a statement, inside
  parentheses, after `AS` (CTE bodies, `CREATE TABLE ... AS`) or after
  `EXPLAIN`. A PERFETTO block on its own also starts one, without `FROM`.
* Stage keywords and their meaning follow PerfettoSQL's pipe syntax, so a
  stage both sides support can be copied between them. Stage bodies are
  DuckDB SQL.
* Each stage becomes a DuckDB subquery over the previous one, so
  `FROM x AS s |> WHERE p |> SELECT a` runs as
  `SELECT a FROM (SELECT * FROM x AS s WHERE p) AS s`.
* `AGGREGATE` outputs the `GROUP BY` columns first, then the aggregates.
* The pipeline's alias, from the source or the last `|> AS`, names the
  current row in every later stage.
* Queries without `|>` are passed to DuckDB untouched.
* [`chain_rewriter.cc`](src/chain_rewriter.cc) does this in the same
  `parser_override` pass as blocks: blocks are rewritten first, then
  pipelines, with DuckDB's tokenizer reading the outer query.

### Examples

These run against `test/data/api24_startup_cold.perfetto-trace` attached as
`t`. [`e2e_test.py`](e2e_test.py) runs every example in this section and
checks its output.

**DuckDB stages after a block**, here joined with a DuckDB table:

```sql
CREATE TABLE budgets AS
  SELECT 'androidx.benchmark.integration.macrobenchmark.target' AS package,
         200 AS budget_ms;

FROM PERFETTO(t) (
  INCLUDE PERFETTO MODULE android.startup.startups;
  SELECT package, dur FROM android_startups
)
|> JOIN budgets USING (package)
|> EXTEND dur / 1e6 AS dur_ms
|> SELECT package, dur_ms, budget_ms, dur_ms > budget_ms AS over_budget;
```

```text
package                                              | dur_ms   | budget_ms | over_budget
-----------------------------------------------------+----------+-----------+------------
androidx.benchmark.integration.macrobenchmark.target | 269.9465 | 200       | True
```

**TP's pipe syntax inside a block.** Here `TREE ACCUMULATE UP` sums `dur`
over each slice's subtree:

```sql
FROM PERFETTO(t) (
  PERFETTO PRAGMA pipelines = 1;
  FROM slice
  |> TREE ACCUMULATE UP SUM(dur) AS subtree_dur
  |> ORDER BY subtree_dur DESC
  |> SELECT name, depth, dur, subtree_dur
)
LIMIT 3;
```

```text
name                 | depth | dur        | subtree_dur
---------------------+-------+------------+------------
startActivityAndWait | 0     | 1110688706 | 2128502663
executeCommand       | 1     | 1012039477 | 1012866300
drawing              | 0     | 685631770  | 685631770
```

**Both.** TP's `INTERVAL FLATTEN` cuts the timeline into segments at every
start and end of a thread's run on a CPU, and counts the threads running in
each segment. DuckDB then adds up the time spent at each level of CPU
concurrency:

```sql
FROM PERFETTO(t) (
  PERFETTO PRAGMA pipelines = 1;
  FROM (SELECT ts, dur FROM sched WHERE utid != 0 AND dur > 0)
  |> INTERVAL FLATTEN AGGREGATE COUNT(*) AS running
)
|> AGGREGATE sum(dur) / 1e6 AS ms GROUP BY running
|> ORDER BY running;
```

```text
running | ms
--------+-----------
1       | 847.876726
2       | 417.002077
3       | 265.234189
4       | 126.81776
5       | 69.315007
6       | 22.398534
```

Errors point into the original query:

```sql
FROM PERFETTO(t) (SELECT * FROM slice) |> TREE ACCUMULATE UP SUM(dur) AS s;
```

```text
Parser Error: INTERVAL and TREE stages are only available inside PERFETTO blocks

LINE 1: FROM PERFETTO(t) (SELECT * FROM slice) |> TREE ACCUMULATE UP SUM(dur) AS s;
                                                  ^
```

### Pushdown

A `WHERE` and the projection right after a block, or after `t.<table>`, reach
TP through the same filter pushdown as `t.<table>` scans: column-vs-constant
comparisons, `IS [NOT] NULL` and `IN (...)` are translated into PerfettoSQL;
everything else stays in DuckDB. Only predicates which mean the same in both
engines are translated, so this changes where the work happens, never the
result. `EXPLAIN` shows what was pushed:

```sql
EXPLAIN
FROM PERFETTO(t) (SELECT name, dur, depth FROM slice)
|> WHERE depth = 0
|> SELECT name;
```

```text
┌───────────────────────────┐
│       PERFETTO_QUERY      │
│    ────────────────────   │
│    PerfettoSQL Filters:   │
│        "depth" = 0        │
│                           │
│     Projections: name     │
│                           │
│           ~1 row          │
└───────────────────────────┘
```

To push into a block, the extension wraps the block's final statement:
`SELECT <columns> FROM (<final statement>) WHERE <predicates>`. On an
attached trace, the statements before it are not run again. This needs the
final statement to be a `SELECT`. A TP pipeline can't be wrapped, as
PerfettoSQL doesn't accept one as a subquery, so after a block ending in a
pipeline nothing is pushed and DuckDB filters the rows itself.

### Limitations of chains

* **Pushdown re-runs the final statement.** A block runs at bind time so that
  DuckDB knows its result types (see "Block side effects" under
  [Limitations](#limitations)). When something is pushed, the wrapped final
  `SELECT` runs again. That costs little for a streaming query, but a final
  statement which sorts or aggregates does its work twice.
* **Aliases across stages.** Only the pipeline's alias survives from one
  stage to the next. The table aliases of a `JOIN` stage are visible in that
  stage only. After an `ON` join, later stages see a column name both sides
  share twice, the second copy renamed by DuckDB (`name`, `name_1`).
* **DuckDB errors in stage bodies** (e.g. an unknown column) quote the
  rewritten query, not the original. Errors in the pipe structure itself
  (unknown stage, misplaced `|>`) point into the original query.
* **Placement.** `SELECT ... FROM t |> ...`, and pipelines after `DESCRIBE`,
  `UNION` or `INSERT INTO`, are not supported: start the pipeline with
  `FROM`, inside parentheses if needed.
* **DuckDB vs PerfettoSQL items.** Stage bodies are DuckDB SQL: TP's
  `* EXCEPT (...)` is `* EXCLUDE (...)` in DuckDB.
* **TP's `|> AGGREGATE`** is accepted by TP's parser but not implemented by its
  pipeline engine yet, and currently aborts the process (`Unknown pipeline
  stage`). Aggregate after the block, in DuckDB, instead.
* **No duration literals**, as TP's grammar has none.

## Results (M-series Mac, 8 cores, release build, DuckDB 1.5.6 CLI and Python)

Verified working in the stock DuckDB CLI and the `duckdb` Python package:

* `ATTACH`, then joins across `t.*` tables;
* multi-statement blocks with stdlib `INCLUDE`;
* blocks joined with DuckDB tables, inside CTEs, and in `CREATE TABLE AS` /
  `COPY`;
* `INCLUDE`d tables appearing in the catalog;
* `DETACH`;
* read-only enforcement;
* TP error messages surfacing as DuckDB errors.

| Workload | Time |
| -------- | ---- |
| `ATTACH` a 15 MB Android trace | 150 ms |
| `CREATE TABLE AS FROM t.slice` (123k rows x 15 cols) | 42 ms |
| `CREATE TABLE AS SELECT ts, dur FROM t.slice` (projection pushdown) | 6 ms |
| `t.slice WHERE track_id = X`, filter pushed into TP | 0.19 ms |
| same predicate, not pushable (`abs(track_id) = X`) | 5.3 ms |
| 5-predicate filter on `t.slice`, pushed / not pushed | 0.33 / 15.1 ms |
| same as a `PERFETTO(t) (...)` block | 0.12 ms |
| `perfetto_query` over 16 copies of the trace (2M rows), 1 / 10 threads | 2.67 / 0.74 s |

Pushed and non-pushed filters return identical results.

## Limitations

* **CLI statement splitting.** The DuckDB CLI decides a statement is complete
  when a line ends in `;` (outside quotes and comments), before any parser
  runs. It doesn't track parens, so in the *interactive or piped CLI*, a line
  inside a block must not end in `;`. Put the `;` mid-line, or start the next
  line with it. Python, JDBC, Node etc. pass the whole string and are
  unaffected. The real fix is the PEG grammar rule, or a small upstream change
  to make `SQLIsComplete` track paren depth.
* **Exact DuckDB version.** C++ extensions only load into the DuckDB version
  they were built for, so a release means one binary per DuckDB version and
  platform. A C-API-only extension avoids this but cannot provide
  `parser_override` or storage extensions.
* **The override setting.** The extension sets
  `allow_parser_override_extension = 'strict'` on load. Without it,
  `parser_override` hooks are ignored. `strict` (rather than `fallback`) is
  needed for our error messages, e.g. "PERFETTO block is missing its closing
  parenthesis", to surface. Queries without blocks are unaffected either way.
* **Result types for blocks.** Types are inferred from a 2048-row sample of
  the first trace. Columns that are all NULL there become `VARCHAR`, and a
  later value that doesn't fit raises an error suggesting a `CAST`. The
  proper fix is a TP API that describes a statement's output types without
  running it. PerfettoSQL's pipeline planner is the natural place to get
  static output types from.
* **Runtime errors in blocks** (e.g. "no such table") come from TP at bind
  time and quote the block's PerfettoSQL.
* **Concurrency.** Each attached trace is a single TP instance behind its own
  mutex, so concurrent scans of *one* trace are serialized; different traces
  run fully in parallel. That requires SQLite in multi-thread mode: standalone
  Perfetto builds use `SQLITE_THREADSAFE=0`, which forbids using SQLite from
  two threads at once even through different connections, so the extension's
  dotfile sets the `perfetto_sqlite_threadsafe` GN arg (default 0) to 2. Peak memory for bulk queries is roughly
  `threads x trace size`.
* **Block side effects.** Blocks run at bind time, which DuckDB needs for
  the result schema. So `DESCRIBE` / `EXPLAIN` of a block containing
  `CREATE PERFETTO TABLE` executes it.
* **Local files only.** Traces are read through TP's `ReadTrace`, not
  DuckDB's filesystem, so httpfs and S3 don't work yet.
* Interrupting a DuckDB query is not wired to
  `TraceProcessor::InterruptQuery`, and there is no progress reporting.

## Possible next steps

1. **Values in, tables in.** Pass DuckDB values into a block, e.g.
   `PERFETTO(t, min_dur := 100e6) (...)`. This can't reuse `$x`, which
   already means macro arguments in PerfettoSQL. Expose DuckDB relations to
   a block, e.g. `PERFETTO(t USING my_table) (...)`, by registering them in
   TP for the duration of the query.
2. **Static result types** from TP (see Limitations), which also removes
   bind-time execution.
3. **A columnar scan path.** Hand TP dataframe columns to DuckDB vectors
   directly instead of iterating SQLite rows.
4. **Upstream.** Propose paren-aware `SQLIsComplete` to DuckDB, and track the
   PEG parser's extension API.
5. **Distribution.** Publish via DuckDB community extensions, which needs a
   CMake shim around the GN build.
