# perfetto DuckDB extension

`perfetto.duckdb_extension` streams trace_processor query results into DuckDB
for the local Bigtrace server (`../server.py`); see
[docs/design-docs/bigtrace-local.md](/docs/design-docs/bigtrace-local.md).

- `pf_open/pf_feed/pf_wait_typed/...` (C ABI, called with ctypes): a feed
  sends a query to a `trace_processor_shell --stdiod` over its stdin/stdout
  pipes, decodes the `QueryResult` batches outside the GIL and queues their
  cells in a bounded queue.
- `pf_scan(stream, threads, types, uuids, ms)`: a table function that drains a
  stream on DuckDB threads as columns of `types` (`BIGINT`, `DOUBLE`,
  `VARCHAR`) plus `_trace_uuid`. It stops early when cells need a wider type,
  or after `ms`, so the server can widen the table or commit.
- `pf_compile_pipe(sql)` (`src/pipe.cc`): compiles PerfettoSQL `|>` chain
  syntax (and duration literals such as `16ms`) into plain DuckDB SQL; SQL
  without `|>` is returned unchanged.

The server builds it on first use into
`~/.cache/perfetto/bigtrace/duckdb_ext/<hash of the sources>/`. By hand:
`./build.sh [OUT]` (Linux, `g++`, downloads the DuckDB v1.5.6 C API headers
with `curl`).
