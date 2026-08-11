# Trace Processor (C++)

Trace Processor is a C++ library ([src/trace_processor](/src/trace_processor))
that ingests traces in a variety of formats and exposes an SQL interface for
querying them through a consistent set of tables. It also computes trace
summaries, annotates traces with human-readable descriptions, and derives new
events from trace contents.

![Trace processor block diagram](/docs/images/trace-processor.png)

Most users interact with Trace Processor through the
[`trace_processor` shell](#shell), a command-line wrapper around the library
that opens an interactive PerfettoSQL prompt. To embed Trace Processor in
another C++ application, see [Embedding the C++ library](#embedding). Python
users should use the [Python API](trace-processor-python.md) instead.

## {#shell} The trace_processor shell

The `trace_processor` shell is a command-line binary that loads a trace and
opens an interactive SQL prompt on it.

### Downloading the shell

The shell is a thin Python wrapper that you download from the Perfetto
website. On first use it fetches and caches the native binary for your
platform (including `trace_processor_shell.exe` on Windows) under
`~/.local/share/perfetto/prebuilts`.

<?tabs>

TAB: Linux / macOS

```bash
curl -LO https://get.perfetto.dev/trace_processor
chmod +x ./trace_processor
```

TAB: Windows

```powershell
curl.exe -LO https://get.perfetto.dev/trace_processor
```

Python 3 is required to run the wrapper script. `curl` ships with Windows 10
and later.

</tabs?>

### Running the shell

Once downloaded, run it on a trace file:

<?tabs>

TAB: Linux / macOS

```bash
./trace_processor trace.perfetto-trace
```

TAB: Windows

```powershell
python trace_processor trace.perfetto-trace
```

</tabs?>

This opens an interactive SQL shell where you can query the trace. For how to
write queries, see the
[Getting Started with PerfettoSQL](perfetto-sql-getting-started.md) guide.

TIP: the trace file can also be a ZIP or TAR archive containing several
traces: they are merged onto a single timeline. See
[Merging traces from the command line](/docs/analysis/merging-traces.md).

For example, to see all the slices in a trace:

```sql
> SELECT ts, dur, name FROM slice LIMIT 10;
ts                   dur                  name
-------------------- -------------------- ---------------------------
     261187017446933               358594 eglSwapBuffersWithDamageKHR
     261187017518340                  357 onMessageReceived
     261187020825163                 9948 queueBuffer
     261187021345235                  642 bufferLoad
     261187121345235                  153 query
...
```

Or, to see the values of all counters:

```sql
> SELECT ts, value FROM counter LIMIT 10;
ts                   value
-------------------- --------------------
     261187012149954          1454.000000
     261187012399172          4232.000000
     261187012447402         14304.000000
     261187012535839         15490.000000
     261187012590890         17490.000000
     261187012590890         16590.000000
...
```

### {#sessions} Keeping a trace warm: sessions

Parsing a large trace takes time. If you plan to run several queries against
the same trace, load it once into a named background session and point each
invocation at that session:

```bash
# Load the trace once into a background session.
trace_processor server unix --name mysession --daemonize trace.pftrace

# Run queries against the warm session instead of re-loading the trace.
trace_processor query --remote mysession "SELECT count(*) FROM slice"
```

The `query`, `interactive`, `metrics` and `summarize` subcommands all accept
`--remote`, which talks to a session over the same TraceProcessor RPC
interface the Perfetto UI uses. See
[Analyzing traces from the command line](/docs/getting-started/command-line-analysis.md)
for a walkthrough, and the [`server` subcommand](#subcommand-server) below
for the mode and flag details.

### {#subcommands} Subcommand interface

Besides the interactive REPL, `trace_processor` takes a subcommand as its
first argument for non-interactive workflows:

```text
trace_processor <command> [flags] [positional args]
```

`trace_processor --help` prints the top-level summary below. To see the flags
of one subcommand, run `trace_processor <command> --help` (equivalently
`trace_processor help <command>`):

```text
Perfetto Trace Processor.
Usage: trace_processor [command] [flags] [trace_file]

If no command is given, opens an interactive SQL shell on the trace file.

Commands:
  query         Load a trace and run a SQL query.
  interactive   Interactive SQL shell (default if no command is given).
  server        Start an RPC server.
  summarize     Compute a trace summary from specs and/or built-in metrics.
  export        Export trace data (sqlite, arrow_tar, perfetto).
  metrics       Run v1 metrics (deprecated; use 'summarize --metrics-v2').
  convert       Convert trace format.

Common flags (apply to all commands):
  -h, --help                  Show help (per-command if after a command).
  -v, --version               Print version.
      --full-sort             Force full sort ignoring windowing.
      --no-ftrace-raw         Prevent ingestion of typed ftrace into raw table.
      --add-sql-package PATH  Register SQL files from a directory as a package.
  -m, --metatrace FILE        Enable metatracing, write to FILE.
```

> **Backwards compatibility.** The classic flat-flag interface (`-q`, `-Q`,
> `--httpd`, `--summary`, `--run-metrics`, `-e`, `--stdiod`) is still
> supported through an internal translation layer, so existing scripts keep
> working unchanged. Run `trace_processor --help-classic` for the full list
> of classic flags.

#### {#subcommand-query} `query`: run SQL

`query` loads a trace, runs one or more `;`-separated SQL statements, prints
the results to stdout, and exits. SQL can be passed as an argument, read from
a file, or piped on stdin:

```bash
# Pass SQL as an argument.
trace_processor query trace.pftrace "SELECT ts, dur, name FROM slice LIMIT 5"

# Read SQL from a file.
trace_processor query -f queries.sql trace.pftrace

# Pipe SQL on stdin.
cat queries.sql | trace_processor query trace.pftrace
```

Each statement's result set is printed as CSV, and consecutive result sets
are separated by a single blank line. The separator is unambiguous because
every string value is quoted.

Flags:

- `--remote ADDR`: run against a warm session instead of loading a local
  trace; see [sessions](#sessions). `ADDR` is a session name, a `*.sock` or
  absolute socket path, or `host:port`. No trace-file argument is passed in
  this mode.
- `-f, --query-file FILE`: read SQL from `FILE`; pass `-` to read from stdin.
- `-i, --interactive`: drop into the interactive REPL after the queries
  finish.
- `-W, --wide`: use double-width columns when printing results.
- `--perf-file FILE`: write trace-load and query timings to `FILE`.
- `--structured-query-id ID` plus `--summary-spec FILE` _(advanced)_: run a
  single structured query by ID from one or more
  [TraceSummarySpec](trace-summary.md) files, instead of the SQL sources
  above.

#### {#subcommand-interactive} `interactive`: REPL

`interactive` opens the same interactive PerfettoSQL prompt shown in the
previous section. It is the default subcommand, so
`trace_processor trace.pftrace` and
`trace_processor interactive trace.pftrace` are equivalent. The only
subcommand-specific flag is `-W, --wide`.

#### {#subcommand-server} `server`: HTTP, stdio, or unix RPC

`server` exposes trace processor over a remote-procedure-call protocol:

```bash
# HTTP server, used by ui.perfetto.dev. Listens on port 9001 by default.
trace_processor server http

# Pre-load a trace and serve it over HTTP.
trace_processor server http trace.pftrace

# stdio server: length-prefixed RPC for tooling that embeds
# trace_processor as a subprocess.
trace_processor server stdio

# Named unix-socket session: keeps the trace warm for repeated
# `query --remote <name>` calls (see the sessions section above).
trace_processor server unix --name mysession --daemonize trace.pftrace

# Stop a unix session by name or socket path.
trace_processor server kill mysession
```

Flags:

- `--port PORT`: HTTP port (default 9001).
- `--ip-address IP`: HTTP bind address.
- `--additional-cors-origins O1,O2,...`: extra CORS-allowed origins on top
  of the defaults (`https://ui.perfetto.dev`, `http://localhost:10000`,
  `http://127.0.0.1:10000`).
- `--name NAME`: session name for unix mode (default: auto-generated).
- `--path PATH`: explicit socket path for unix mode (mutually exclusive
  with `--name`).
- `--daemonize`: detach into the background (unix mode, POSIX only).
- `--idle-timeout auto|DUR`: reap the server after this much inactivity
  (e.g. `30m`, `90s`); `auto` means 30 minutes for unix and never for
  http, `0`/`never` disables.
- `--idle-start auto|orphaned|last-query`: when the idle clock applies
  (default `auto`: owner-aware).

The trace file is optional in `http` and `unix` modes; clients can also
load traces remotely. The most common client is the Perfetto UI, which
auto-detects a local server and offloads trace parsing to it. See
[Visualising large traces](/docs/visualization/large-traces.md) for the
end-user flow, or
[trace_processor.proto](/protos/perfetto/trace_processor/trace_processor.proto)
for the RPC wire schema.

#### {#subcommand-summarize} `summarize`: compute trace summaries

`summarize` computes a [trace summary](trace-summary.md). Pass the trace
file first, then any spec files; select built-in v2 metrics with
`--metrics-v2`:

```bash
# Run every available v2 metric.
trace_processor summarize --metrics-v2 all trace.pftrace

# Run two specific metrics defined in spec.textproto.
trace_processor summarize \
  --metrics-v2 startup_metric,memory_metric \
  trace.pftrace spec.textproto
```

Flags:

- `--metrics-v2 IDS`: comma-separated metric ids, or the literal `all`.
- `--metadata-query ID`: query id used to populate the summary's
  `metadata` field.
- `--format text|binary`: output format for the `TraceSummary` proto
  (default `text`).
- `--post-query FILE`: run this SQL file after summarization. When set, the
  summary proto is not printed; the SQL output is printed instead.
- `--perf-file FILE`: write load/query timings to `FILE`.
- `-i, --interactive`: drop into the REPL after summarization finishes.

Spec files are detected as binary or text by extension (`.pb` for binary,
`.textproto` for text), with content sniffing as a fallback.

#### {#subcommand-export} `export`: write trace data to a file

`export` writes the parsed trace data to a file. The format is the first
positional argument, the output path is given with `-o`:

```bash
# Version-coupled archive, loadable by the same version of trace processor.
trace_processor export perfetto -o archive.tar trace.pftrace

# Static tables as standard Arrow files in a tar.
trace_processor export arrow_tar -o tables.tar trace.pftrace

# Static tables and views as a SQLite database.
trace_processor export sqlite -o trace.db trace.pftrace
```

Formats:

- **`perfetto`**: a version-coupled archive of the non-empty static tables.
  A fresh trace processor instance from the same version can load it back as
  a trace; a different version may load it, but this is not guaranteed. The
  only format that can be reloaded.
- **`arrow_tar`**: a tar of standard [Apache Arrow](https://arrow.apache.org/)
  files, one per statically registered table, including empty tables and
  implicit ID columns. Stable and forwards-compatible across versions, for
  external consumers (e.g. pandas, Polars, pyarrow). Cannot be loaded back
  into trace processor.
- **`sqlite`**: the statically registered tables plus the trace's views, as
  a SQLite database readable by any SQLite tool.

Flags:

- `-o, --output FILE`: output file path (required).

All three formats export the statically registered tables; only `sqlite` also
includes views. Runtime tables created during the session (e.g.
`CREATE PERFETTO TABLE`) are not exported. Exports stream to disk, so memory
use stays bounded for large traces. For task-oriented recipes, see
[Export trace data](/docs/getting-started/command-line-analysis.md#export-trace-data).

#### {#global-flags} Global flags (apply to every subcommand)

These flags are accepted in addition to the subcommand-specific flags above
and behave the same across all subcommands:

- **Trace ingestion:** `--full-sort`, `--no-ftrace-raw`,
  `--analyze-trace-proto-content`, `--crop-track-events`.
- **PerfettoSQL packages:** `--add-sql-package PATH[@PKG]`,
  `--override-sql-package PATH[@PKG]`, `--override-stdlib PATH`
  (requires `--dev`).
- **Metric extensions:** `--metric-extension DISK_PATH@VIRTUAL_PATH`.
- **Auxiliary file content:** `--register-files-dir PATH` exposes the
  contents of files under `PATH` to importers (e.g. ETM decoders).
- **Development:** `--dev`, `--dev-flag KEY=VALUE`, `--extra-checks`.
- **Metatracing:** `-m, --metatrace FILE`, `--metatrace-buffer-capacity N`,
  `--metatrace-categories CATEGORIES`. This produces a Perfetto trace of
  trace processor itself, which you can load back into the UI for
  performance debugging.

## {#embedding} Embedding the C++ library

The public API centers on the `TraceProcessor` class in
[`trace_processor.h`](/include/perfetto/trace_processor/trace_processor.h).
All high-level operations (parsing trace bytes, executing SQL queries,
computing summaries) are member functions of this class.

Create an instance with `CreateInstance`:

```cpp
#include "perfetto/trace_processor/trace_processor.h"

using namespace perfetto::trace_processor;

Config config;
std::unique_ptr<TraceProcessor> tp = TraceProcessor::CreateInstance(config);
```

### Loading a trace

To ingest a trace, call `Parse` repeatedly with chunks of trace bytes, then
`NotifyEndOfFile` once the whole trace has been pushed:

```cpp
while (/* more data available */) {
  TraceBlobView blob = /* ... */;
  base::Status status = tp->Parse(std::move(blob));
  if (!status.ok()) { /* handle error */ }
}
base::Status status = tp->NotifyEndOfFile();
```

Because reading a trace from the filesystem is a common case, a helper
`ReadTrace` is provided in
[`read_trace.h`](/include/perfetto/trace_processor/read_trace.h):

```cpp
#include "perfetto/trace_processor/read_trace.h"

base::Status status = ReadTrace(tp.get(), "/path/to/trace.pftrace");
```

`ReadTrace` reads the file from disk, calls `Parse` with the contents, and
calls `NotifyEndOfFile` for you.

### Executing queries

Run queries with `ExecuteQuery`, which returns an `Iterator` that streams
rows back to the caller:

```cpp
auto it = tp->ExecuteQuery("SELECT ts, name FROM slice LIMIT 10");
while (it.Next()) {
  int64_t ts = it.Get(0).AsLong();
  std::string name = it.Get(1).AsString();
  // ...
}
if (!it.Status().ok()) {
  // Query produced an error.
}
```

Two important rules when using the iterator:

- **Always call `Next` before accessing values.** The iterator is positioned
  before the first row when returned, so `Get` cannot be called until `Next`
  has returned `true`.
- **Always check `Status` after iteration finishes.** A query may fail
  partway through; `Next` returning `false` only means iteration stopped, not
  that it succeeded. Inspect `Status()` to distinguish EOF from an error.

See the comments in
[`iterator.h`](/include/perfetto/trace_processor/iterator.h) for the full
iterator API.

### Other functionality

The `TraceProcessor` class also exposes:

- **Trace summarization** (`Summarize`): computes structured summaries of a
  trace. See [Trace Summarization](trace-summary.md) for a user-facing
  description.
- **Custom SQL packages** (`RegisterSqlPackage`): registers PerfettoSQL files
  under a package name so queries can `INCLUDE` them.
- **Out-of-band file content** (`RegisterFileContent`): passes auxiliary data
  to importers, e.g. binaries used to decode ETM traces.
- **Metatracing** (`EnableMetatrace` / `DisableAndReadMetatrace`): traces
  Trace Processor itself for performance debugging.

Refer to the comments in
[`trace_processor.h`](/include/perfetto/trace_processor/trace_processor.h) for
the complete API surface.
