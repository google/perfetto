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
for a walkthrough, and the [`server` reference](/docs/reference/trace-processor-cli.md#subcommand-server)
for the mode and flag details.

### {#subcommands} Command-line reference

See the [Trace Processor command-line reference](/docs/reference/trace-processor-cli.md)
for commands, options, environment variables, and output behavior.

#### {#subcommand-query} query

See [query](/docs/reference/trace-processor-cli.md#subcommand-query) in the CLI reference.

#### {#subcommand-interactive} interactive

See [interactive](/docs/reference/trace-processor-cli.md#subcommand-interactive) in the CLI reference.

#### {#subcommand-server} server

See [server](/docs/reference/trace-processor-cli.md#subcommand-server) in the CLI reference.

#### {#subcommand-summarize} summarize

See [summarize](/docs/reference/trace-processor-cli.md#subcommand-summarize) in the CLI reference.

#### {#subcommand-export} export

See [export](/docs/reference/trace-processor-cli.md#subcommand-export) in the CLI reference.

#### {#global-flags} Global flags

See [Global flags](/docs/reference/trace-processor-cli.md#global-flags) in the CLI reference.

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
