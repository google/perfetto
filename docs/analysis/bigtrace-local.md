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
- Python 3.11+ with `pip install duckdb pyarrow` (the installer of the
  [always-on server](#always-on-server) sets these up in a virtualenv).
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

### In the UI

Open `https://ui.perfetto.dev/bigtrace.html?server=http://127.0.0.1:5001`
(or a local UI from `ui/run-dev-server --bigtrace`). The settings page lists
the traces; filter the list, then write PerfettoSQL in a query tab and press
Run. Rows show up while the query runs; sort and filter the result grid as
you like. **Add Query** adds queries that run in the same pass. Name a tab
(double-click) to name its table; if the name is taken, Run offers to
overwrite it. Switch the engine to **DuckDB SQL** to query the result tables,
and click a trace's `link` to open it in the Perfetto UI.

The server button at the top right picks the server: the Bigtrace service,
the **Local server** (`http://127.0.0.1:5001/v1`), or servers you add (for
example a second local server on another port). Each shows whether it
answers; picking one reloads the UI on it.

## Always-on server

Rather than starting `serve` by hand, install the server as a systemd user
service that is always running, like a daemon: it starts at login (and at
boot with `loginctl enable-linger`), restarts if it fails, and keeps its
result tables and query history in
`~/.local/share/perfetto/bigtrace/bigtrace.duckdb` across restarts.

```bash
curl -fsSL https://raw.githubusercontent.com/google/perfetto/main/tools/install-bigtrace | python3 -
```

The installer creates a virtualenv in `~/.local/share/perfetto/bigtrace/venv`
with the `perfetto` package and DuckDB, puts a `bigtrace` command in
`~/.local/bin`, builds the DuckDB extension, downloads `trace_processor_shell`
and starts the service on port 5001, serving `~/traces`. Options after the
script go to `bigtrace install`, e.g. `python3 - --traces ~/my_traces`. Rerun
it to update. From a checkout, `tools/bigtrace_server.py install` does the
same with the checkout's code.

```bash
bigtrace status       # running? what does it serve? (--json for scripts)
bigtrace logs -f      # the service's log
bigtrace stop         # also: start, restart
bigtrace install --traces ~/other_traces   # change settings, keeps the others
bigtrace uninstall    # --purge also deletes the result tables
```

The UI can switch to any directory inside the served one (the trace
directory setting), so serve the parent of the directories you use.

### Other Bigtrace UIs

The server answers pages from `https://ui.perfetto.dev` and localhost. Allow
another deployment of the Bigtrace UI with `--cors-origin`; the setup guide of
such a UI (**Local server** > **Set up**) shows the install command with it:

```bash
curl -fsSL https://raw.githubusercontent.com/google/perfetto/main/tools/install-bigtrace | python3 - --cors-origin https://bigtrace.example.com
```

That guide waits for the server and switches the UI to it once it answers.

### Traces on another machine

The server only listens on `127.0.0.1`. When the browser runs on another
machine than the traces (e.g. a laptop and a cloud workstation), forward the
port over SSH from the browser's machine, and use the **Local server** there:

```bash
ssh -N -L 5001:127.0.0.1:5001 WORKSTATION
# or, with the bigtrace command on the laptop (`... | python3 - --client-only`):
bigtrace tunnel WORKSTATION
```

`bigtrace tunnel` reconnects when the connection drops and says whether the
server answers. Chrome may ask to let the page access devices on your local
network; allow it.

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
- *The service does not start*: `bigtrace logs` has the server's output.
  `bigtrace install` refuses a port in use; a missing trace directory stops
  the service rather than restarting it in a loop.
- *"no systemd user session"*: log in normally (not with `su`/`sudo`), or run
  `bigtrace serve TRACES` in the foreground.
- *The UI says "Origin not allowed"*: rerun `bigtrace install --cors-origin
  https://YOUR-UI` (the setup guide's command has the right one).
- *The Local server is unreachable from a laptop*: is `bigtrace tunnel` (or
  `ssh -L`) running there? Did Chrome's local network prompt get denied (site
  settings > Local network access)?
