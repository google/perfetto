# Local Bigtrace: query many traces on one machine

Local Bigtrace runs a PerfettoSQL query on hundreds or thousands of traces at
once, on one machine, and collects the rows into [DuckDB](https://duckdb.org)
tables that you then analyse with DuckDB SQL. It is a single command,
`tools/bigtrace_server.py`, that serves the Bigtrace UI and has a command line
for scripts and AI agents.

Use it when the traces are on local disk and fit one machine (tested with
1,000 traces and 300M+ result rows). For a shared, multi-machine service use
the [Bigtrace deployment](/docs/deployment/deploying-bigtrace-on-kubernetes.md)
instead; for querying traces from Python, see
[Batch Trace Processor](/docs/analysis/batch-trace-processor.md). How it works
is described in the [design doc](/docs/design-docs/bigtrace-local.md).

## Requirements

- Linux (x86-64 or arm64) with `g++` and `curl`: the server builds a small
  DuckDB extension on first use.
- Python 3.11+ with `pip install duckdb pyarrow`.
- `trace_processor_shell`: pass `--shell-path` or set `$SHELL_PATH`, otherwise
  a prebuilt one is downloaded.

## Tutorial

Serve the test traces of a Perfetto checkout:

```bash
mkdir /tmp/traces && cp test/data/api*_startup_*.perfetto-trace /tmp/traces
tools/bigtrace_server.py serve /tmp/traces --generate-manifest
```

`--generate-manifest` loads every trace once and records per-trace metadata
(duration, slice/process/thread counts, build...) in
`bigtrace_manifest.json`, so the trace list can be filtered by it. It is
optional; without a manifest, every file in the directory is a trace.

### From the command line

In another shell, run a query on all traces. Each query fills a DuckDB table
with one row per result row and a `_trace_uuid` column:

```bash
tools/bigtrace_server.py query --table slices \
  "SELECT s.name, s.dur, t.name AS thread FROM slice s JOIN thread_track tt ON s.track_id = tt.id JOIN thread t USING (utid)"
```

It prints the first rows as CSV (`--limit`, default 100), and the row count and
any per-trace errors on stderr. Several queries in one call run in one pass,
loading each trace once, into one table each (`--table` per query). Use
`--filter "duration_ms > 1000"` to pick traces by their metadata.

Then analyse the tables with DuckDB SQL; `trace_metadata` has one row per
trace:

```bash
tools/bigtrace_server.py sql "
  SELECT m.file_name, count(*) AS slices, sum(dur) / 1e6 AS total_ms
  FROM slices JOIN trace_metadata m USING (_trace_uuid)
  GROUP BY ALL ORDER BY total_ms DESC"
tools/bigtrace_server.py sql "FROM duckdb_tables()"
```

Aggregate in SQL rather than printing whole tables. Run
`tools/bigtrace_server.py <command> --help` for every option.

DuckDB SQL mode also accepts PerfettoSQL's
[pipe (`|>`) syntax](/docs/analysis/perfetto-sql-pipe-syntax.md) (`WHERE`,
`SELECT` with `* EXCEPT` / `* REPLACE`, `EXTEND`, `SET`, `DROP`, `RENAME`,
`AGGREGATE ... GROUP BY`, `JOIN`, `ORDER BY`, `LIMIT`, `AS`, `FORK AS`) and
duration literals (`16ms`, `1.5s`), compiled to plain SQL by `pf_compile_pipe`:

```sql
FROM slices AS s
|> WHERE s.dur > 16ms
|> AGGREGATE count(*) AS slow_slices GROUP BY s._trace_uuid
|> JOIN trace_metadata AS m USING (_trace_uuid)
|> SELECT m.file_name, slow_slices
|> ORDER BY slow_slices DESC
```

It also provides native interval, tree and graph operators partitioned per
trace (`PER _trace_uuid, ...`; see `python/perfetto/bigtrace/duckdb_ext/docs/`):

```sql
INTERVAL INTERSECTION OF (slices AS s, sched AS c) PER _trace_uuid, utid
|> AGGREGATE sum(dur) AS cpu_ns GROUP BY _trace_uuid, s.name
```

### In the UI

Open `https://ui.perfetto.dev/bigtrace.html?server=http://127.0.0.1:5001`
(or a local UI from `ui/run-dev-server --bigtrace`). The settings page lists
the traces; filter the list, then write PerfettoSQL in a query tab and press
Run. Rows show up while the query runs; sort and filter the result grid as
you like. **Add Query** adds queries that run in the same pass. Name a tab
(double-click) to name its table; if the name is taken, Run offers to
overwrite it. Switch the engine to **DuckDB SQL** to query the result tables,
and click a trace's `link` to open it in the Perfetto UI.

## Manifests

A manifest lists the traces and their metadata, which become the columns of
`trace_metadata`. `bigtrace_manifest.json` or `metadata.db` in the trace
directory is loaded automatically; `-m` picks another one.

- JSON: `{"trace_dir": ".", "traces": [{"file_path": "a.pftrace", ...}],
  "schema": [...], "presets": [...], "experiments": [...]}` or just the list
  of traces. Paths are relative to `trace_dir`, itself relative to the
  manifest. `schema` sets display names and default visibility of columns;
  `presets` and `experiments` are passed to the UI.
- CSV (`.csv`): one row per trace with a `file_path` column.
- SQLite (`.db`): a `traces` table with a `file_path` or
  `absolute_local_file_path` column.

`examples/bigtrace` has examples. `--generate-manifest` keeps the entries of
traces that did not change, so rerunning it is cheap; `--metadata-query SQL`
adds the columns of the first row of a query to each trace.

## Results, history and persistence

Each query's table is named after `--table` (or the UI tab), else `result_N`;
`latest` is a view of the most recent one. Result tables and the query history
(`_bigtrace_history`) live in memory unless the server runs with
`--duckdb-path FILE`, which keeps them across restarts. Deleting a query from
the UI history drops its table.

A trace that fails to load or query is reported in the query's error and its
rows are left out; the query only fails if every trace does. Column types
come from the values the traces return and widen (integer, double, string)
when traces disagree.

## Archives and exports

`TRACES` can be a zip or tar archive of traces, which is extracted once to
`~/.cache/perfetto/bigtrace` and served recursively. Traces load several times
faster as trace processor exports (`trace_processor_shell export perfetto -o
t.perfetto.tar t`, optionally compressed with `zstd`), which are served like
any other trace.

## Troubleshooting

- *Building the DuckDB extension failed*: install `g++` and `curl`; the first
  build downloads two DuckDB headers.
- *No traces*: pass `-r` for traces in subdirectories; hidden files, manifests
  and `.duckdb` files are skipped.
- *Out of memory*: lower `--workers` (each loads one trace); DuckDB itself
  uses at most about 40% of RAM and spills beyond that.
- *Access*: the server has no authentication. It only accepts requests that
  address it as a local host and come from localhost pages,
  `https://ui.perfetto.dev`, `--origin` or `--cors-origin`; anyone who can
  connect to the port can read the traces, so keep `--host 127.0.0.1`.
