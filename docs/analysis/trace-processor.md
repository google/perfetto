# Trace Processor (C++)

_The Trace Processor is a C++ library
([/src/trace_processor](/src/trace_processor)) that ingests traces encoded in a
wide variety of formats and exposes an SQL interface for querying trace events
contained in a consistent set of tables. It also has other features including
computation of trace summaries, annotating the trace with user-friendly
descriptions and deriving new events from the contents of the trace._

![Trace processor block diagram](/docs/images/trace-processor.png)

Most users will interact with Trace Processor through the
[`trace_processor` shell](#shell), a command-line wrapper around the library
that opens an interactive PerfettoSQL prompt. Embedders that want to integrate
Trace Processor into another C++ application should jump to
[Embedding the C++ library](#embedding). Python users should see the
[Python API](trace-processor-python.md) instead.

## {#shell} The trace_processor shell

The `trace_processor` shell is a command-line binary which wraps the C++
library, providing a convenient way to interactively analyze traces.

### Downloading the shell

The shell can be downloaded from the Perfetto website. The download is a thin
Python wrapper that fetches and caches the correct native binary for your
platform (including `trace_processor_shell.exe` on Windows) under
`~/.local/share/perfetto/prebuilts` on first use.

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

Once downloaded, you can immediately use it to open a trace file:

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

This will open an interactive SQL shell where you can query the trace. For
more information on how to write queries, see the
[Getting Started with PerfettoSQL](perfetto-sql-getting-started.md) guide.

For example, to see all the slices in a trace, you can run the following query:

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

## {#embedding} Embedding the C++ library

The public API is centered on the `TraceProcessor` class defined in
[`trace_processor.h`](/include/perfetto/trace_processor/trace_processor.h). All
high-level operations — parsing trace bytes, executing SQL queries, computing
summaries — are member functions on this class.

A `TraceProcessor` instance is created via `CreateInstance`:

```cpp
#include "perfetto/trace_processor/trace_processor.h"

using namespace perfetto::trace_processor;

Config config;
std::unique_ptr<TraceProcessor> tp = TraceProcessor::CreateInstance(config);
```

### Loading a trace

To ingest a trace, call `Parse` repeatedly with chunks of trace bytes, then
`NotifyEndOfFile` once the entire trace has been pushed:

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

Queries are submitted via `ExecuteQuery`, which returns an `Iterator` that
streams rows back to the caller:

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

- **Trace summarization** (`Summarize`) — computes structured summaries of a
  trace. See [Trace Summarization](trace-summary.md) for the user-facing
  description of this feature.
- **Custom SQL packages** (`RegisterSqlPackage`) — registers PerfettoSQL files
  under a package name so they can be `INCLUDE`d by queries.
- **Out-of-band file content** (`RegisterFileContent`) — passes auxiliary data
  to importers, e.g. binaries used to decode ETM traces.
- **Metatracing** (`EnableMetatrace` / `DisableAndReadMetatrace`) — traces the
  Trace Processor itself for performance debugging.

Refer to the comments in
[`trace_processor.h`](/include/perfetto/trace_processor/trace_processor.h) for
the complete API surface.
