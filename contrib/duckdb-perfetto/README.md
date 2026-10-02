# Trace Processor inside DuckDB (prototype)

**Status:** Exploratory prototype. Not yet a proposed contrib/ project: it has
no OWNERS.github and has not gone through the contrib/ proposal process (see
[../README.md](../README.md)).

A DuckDB extension which embeds Perfetto's Trace Processor (TP), so that traces
can be attached to a DuckDB database and queried with PerfettoSQL, executed
inside TP, from any DuckDB client.

```sql
LOAD 'perfetto';  -- or LOAD '/abs/path/to/perfetto.duckdb_extension'

-- A trace is a read-only database. Every TP table is a DuckDB table.
ATTACH 'trace.pftrace' AS t (TYPE perfetto);

SELECT p.name, sum(s.dur) AS cpu_ns
FROM t.sched s JOIN t.thread USING (utid) JOIN t.process p USING (upid)
GROUP BY 1 ORDER BY 2 DESC;

-- PerfettoSQL (stdlib, INCLUDE, macros, multiple statements) runs inside TP.
-- DuckDB only sees the result rows.
FROM perfetto_query('t', $$
  INCLUDE PERFETTO MODULE android.startup.startups;
  SELECT * FROM android_startups WHERE dur > 100e6
$$);

-- Anything TP creates becomes visible in the catalog.
SELECT count(*) FROM t.android_startups;

-- Materialize: plain DuckDB.
CREATE TABLE slices AS FROM t.slice;
COPY (FROM t.thread_state) TO 'thread_state.parquet';

-- Bulk: one PerfettoSQL query over many traces, in parallel.
SELECT trace, count(*), max(dur)
FROM perfetto_query('traces/*.pftrace', 'SELECT dur FROM slice WHERE depth = 0')
GROUP BY trace;
```

## Design

### Catalog: `ATTACH ... (TYPE perfetto)`

A DuckDB `StorageExtension` (the mechanism used by the sqlite and postgres
scanners) provides a read-only catalog with one schema, `main`:

* **Lazy table entries.** Table entries are created on lookup from TP's own
  catalog (`pragma_table_info`). So tables and views created later through
  `perfetto_query` (by `INCLUDE PERFETTO MODULE` or `CREATE PERFETTO TABLE`)
  show up as `t.<name>` with no refresh step.
* **Lifetime.** The trace's lifetime is tied to the attached database:
  `DETACH t` frees it.
* **Projection pushdown.** TP is only asked for the columns DuckDB needs.

### Execution

`perfetto_query(source, sql)` runs the SQL with `TraceProcessor::ExecuteQuery`
and streams the rows into DuckDB vectors. `source` can be:

* an attached alias, which runs against the resident TP. An empty string
  means the only attached perfetto database.
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
contrib/duckdb-perfetto/build.sh
duckdb -unsigned -c "LOAD '$PWD/out/duckdb_ext/perfetto.duckdb_extension'; ..."
```

`build.sh` pins DuckDB `v1.5.6`. On macOS, `LOAD` needs an absolute path.

## Results (M-series Mac, 8 cores, release build, DuckDB 1.5.6 CLI and Python)

| Workload | Time |
| -------- | ---- |
| `ATTACH` a 15 MB Android trace | 150 ms |
| `CREATE TABLE AS FROM t.slice` (123k rows x 15 cols) | 42 ms |
| `CREATE TABLE AS SELECT ts, dur FROM t.slice` (projection pushdown) | 6 ms |
| `perfetto_query` over 16 copies of the trace (2M rows), 1 / 10 threads | 2.67 / 0.74 s |

## Limitations

* **Exact DuckDB version.** C++ extensions only load into the DuckDB version
  they were built for, so a release means one binary per DuckDB version and
  platform. A C-API-only extension avoids this but cannot provide storage
  extensions (or parser hooks).
* **Result types for `perfetto_query`.** Types are inferred from a 2048-row
  sample of the first trace. Columns that are all NULL there become
  `VARCHAR`, and a later value that doesn't fit raises an error suggesting a
  `CAST`. The proper fix is a TP API that describes a statement's output types
  without running it.
* **Concurrency.** Each attached trace is a single TP instance behind its own
  mutex, so concurrent scans of *one* trace are serialized; different traces
  run fully in parallel. That requires SQLite in multi-thread mode: standalone
  Perfetto builds use `SQLITE_THREADSAFE=0`, which forbids using SQLite from
  two threads at once even through different connections, so the extension's
  dotfile sets the `perfetto_sqlite_threadsafe` GN arg (default 0) to 2. Peak memory for bulk queries is roughly
  `threads x trace size`.
* **Bind-time execution.** `perfetto_query` runs at bind time, which DuckDB
  needs for the result schema, so `DESCRIBE` / `EXPLAIN` execute it.
* **Local files only.** Traces are read through TP's `ReadTrace`, not
  DuckDB's filesystem, so httpfs and S3 don't work yet.
* Interrupting a DuckDB query is not wired to
  `TraceProcessor::InterruptQuery`, and there is no progress reporting.

## Possible next steps

1. **Filter pushdown** into TP for `t.<table>` scans.
2. **Inline PerfettoSQL syntax**, so queries don't need string literals.
3. **Static result types** from TP (see Limitations).
4. **A columnar scan path.** Hand TP dataframe columns to DuckDB vectors
   directly instead of iterating SQLite rows.
5. **Distribution.** Publish via DuckDB community extensions, which needs a
   CMake shim around the GN build.
