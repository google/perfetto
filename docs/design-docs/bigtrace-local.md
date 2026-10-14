# Local Bigtrace design

This document describes how `tools/bigtrace_server.py` runs PerfettoSQL over
many local traces. For usage see the
[user guide](/docs/analysis/bigtrace-local.md).

## Goals

- Query 1,000+ local traces in one pass at the speed of the trace processors:
  the server must not be the bottleneck.
- Serve the existing Bigtrace UI (same HTTP API as the cloud Bigtrace backend)
  and a command line, with results kept in DuckDB for fast follow-up analysis.
- Be a small tool: one Python module, one C++ file, no services to deploy.

## Architecture

```
                 ┌──────────────────── bigtrace_server.py ─────────────────────┐
 trace 1 ─► shell ─┐ stdin/stdout:                                             │
 trace 2 ─► shell ─┼─ TraceProcessorRpc ─► pf_feed (C++, one per shell) ─┐      │
   ...             │  QueryResult batches                                ▼      │
 trace N ─► shell ─┘                                          bounded queue     │
                   │                                                │          │
                   │  INSERT INTO result SELECT * FROM pf_scan(...) ◄┘          │
                   │  (DuckDB threads)                       ──► DuckDB tables  │
                   │  HTTP API (UI, CLI) ◄──── paging, DuckDB SQL ◄────┘        │
                   └────────────────────────────────────────────────────────────┘
```

- **Python server** (`python/perfetto/bigtrace/server.py`): the trace catalog,
  query executions and the HTTP API on `http.server.ThreadingHTTPServer`. The
  command line (`cli.py`) serves, or acts as a thin HTTP client.
- **One `trace_processor_shell --stdiod` per trace**, run by a thread pool of
  `--workers` threads. The shell loads its trace and answers queries on its
  stdin/stdout with the length-prefixed `TraceProcessorRpc` protocol
  (`TPM_QUERY_STREAMING`), so there are no ports to allocate and no readiness
  polling: the first reply arrives once the trace is loaded. Every query of a
  pass runs on the same shell, so each trace is loaded once. Shells run under
  `setpriv --pdeathsig KILL`, so they die with the server however it ends,
  and exit by themselves when their stdin closes.
- **C++ DuckDB extension** (`duckdb_ext/src/perfetto_ext.cc`, C API, built on
  first use). Decoding hundreds of millions of cells in Python would make the
  server the bottleneck, so a worker thread hands the shell's pipes to
  `pf_feed()`, which sends the query, decodes the `QueryResult` batches
  outside the GIL and queues the cells in chunks of one DuckDB vector. A full
  queue blocks the feed and, through the pipe, the shell, so memory stays
  bounded. `INSERT INTO t SELECT * FROM pf_scan(...)` drains the queue on
  DuckDB threads, converting cells to the column types and adding
  `_trace_uuid`. Replies are validated (frame sizes, cell counts) so a broken
  shell fails its trace, not the server.

## Data model

- **Result tables**: one per query, named by the user or `result_N`, with the
  query's columns plus `_trace_uuid`, an `ENUM` of the traces of the query.
  Duplicate column names get a `_2` suffix. `latest` is a view of the most
  recent table.
- **Types**: SQLite values are typed per cell, so the types are chosen once
  every column has seen a non-null value (or the queue fills). When a later
  trace returns a value its column cannot hold, the scan stops, the server
  widens the column (`BIGINT` → `DOUBLE` → `VARCHAR`) and resumes.
- **`trace_metadata`**: one row per trace, from the directory listing or a
  JSON/CSV/SQLite manifest, joined on `_trace_uuid`. The UI's trace list,
  trace filters and result metadata columns are SQL over it.
- **History**: `_bigtrace_history(query_uuid, execution JSON)`, written when a
  query ends. With `--duckdb-path` the database is a file, so tables and
  history survive restarts; otherwise it is in memory.

## Concurrency and cancellation

Each trace query has a stream (the queue) and a drain thread running the
INSERTs. A scan ends every 5 seconds so the INSERT commits and partial results
become visible to paging requests, which run on their own DuckDB cursors.
Cancelling a query cancels its stream: feeds stop reading within 100 ms, the
worker kills the shell and the INSERT commits the rows inserted so far. A
DuckDB SQL query is cancelled with `interrupt()`. Rows of a trace that fails
part way are deleted at the end; the query fails only if no trace succeeded.

DuckDB's memory limit is lowered to half its default so the trace processors
keep memory, and user SQL cannot access files (`enable_external_access =
false`, configuration locked).

## HTTP API

The routes the Bigtrace UI uses: `/execute_bigtrace_query[_async]`,
`/query_executions[/<uuid>[:status|:fetch_results|:cancel]]`,
`/trace_metadata[_schema]`, `/bigtrace_execution_config`,
`/check_table_exists`, `/trace_presets`, `/experiments`,
`/experiment_metadata`, plus `GET /` (advertising `features: ["duckdb"]`),
`/refresh`, `/duckdb/tables` and `/trace/<uuid>/<name>` (trace download for
trace links). `:fetch_results` and the sync execute also return CSV
(`"format": "csv"`) for the CLI. Requests must name a local host and come from
an allowed origin, and POST bodies must be JSON, so other websites cannot use
the server.

## Performance

`select * from slice` over 1,000 zstd-compressed trace processor exports
(318.7M rows) with 64 workers on a 128-core machine: see the table in the
change description. The trace processors (loading traces, running queries)
take most of the CPU; decoding and inserting take a few percent.
