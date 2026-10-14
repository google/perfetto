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
duckdb -unsigned -c "LOAD '$PWD/out/duckdb_ext/perfetto.duckdb_extension'; ..."
```

`build.sh` pins DuckDB `v1.5.6`. On macOS, `LOAD` needs an absolute path.

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
