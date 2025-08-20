# Trace Processor (C++)

_The Trace Processor is a C++ library
([/src/trace_processor](/src/trace_processor)) that ingests traces encoded in a
wide variety of formats and exposes an SQL interface for querying trace events
contained in a consistent set of tables. It also has other features including
computation of trace summaries, annotating the trace with user-friendly
descriptions and deriving new events from the contents of the trace._

![Trace processor block diagram](/docs/images/trace-processor.png)

## Getting Started with the Shell

The `trace_processor` shell is a command-line binary which wraps the C++
library, providing a convenient way to interactively analyze traces.

### Downloading the shell

The shell can be downloaded from the Perfetto website:

```bash
# Download prebuilts (Linux and Mac only)
curl -LO https://get.perfetto.dev/trace_processor
chmod +x ./trace_processor
```

### Running the shell

Once downloaded, you can immediately use it to open a trace file:

```bash
# Start the interactive shell
./trace_processor trace.perfetto-trace
```

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


## Python API

The trace processor's C++ library is also exposed through Python. This is
documented on a [separate page](/docs/analysis/trace-processor-python.md).

## Testing

Trace processor is mainly tested in two ways:

1. Unit tests of low-level building blocks
2. "Diff" tests which parse traces and check the output of queries

### Unit tests

Unit testing trace processor is the same as in other parts of Perfetto and other
C++ projects. However, unlike the rest of Perfetto, unit testing is relatively
light in trace processor.

We have discovered over time that unit tests are generally too brittle when
dealing with code which parses traces leading to painful, mechanical changes
being needed when refactorings happen.

Because of this, we choose to focus on diff tests for most areas (e.g. parsing
events, testing schema of tables etc.) and only use unit testing for the
low-level building blocks on which the rest of trace processor is built.

### Diff tests

Diff tests are essentially integration tests for trace processor and the main
way trace processor is tested.

Each diff test takes as input a) a trace file b) a query file _or_ a metric
name. It runs `trace_processor_shell` to parse the trace and then executes the
query/metric. The result is then compared to a 'golden' file and any difference
is highlighted.

All diff tests are organized under [test/trace_processor](/test/trace_processor)
in `tests{_category name}.py` files as methods of a class in each file and are
run by the script
[`tools/diff_test_trace_processor.py`](/tools/diff_test_trace_processor.py). To
add a new test its enough to add a new method starting with `test_` in suitable
python tests file.

Methods can't take arguments and have to return `DiffTestBlueprint`:

```python
class DiffTestBlueprint:
  trace: Union[Path, Json, Systrace, TextProto]
  query: Union[str, Path, Metric]
  out: Union[Path, Json, Csv, TextProto]
```

_Trace_ and _Out_: For every type apart from `Path`, contents of the object will
be treated as file contents so it has to follow the same rules.

_Query_: For metric tests it is enough to provide the metric name. For query
tests there can be a raw SQL statement, for example `"SELECT * FROM SLICE"` or
path to an `.sql` file.

NOTE: `trace_processor_shell` and associated proto descriptors needs to be built
before running `tools/diff_test_trace_processor.py`. The easiest way to do this
is to run `tools/ninja -C <out directory>` both initially and on every change to
trace processor code.

#### Choosing where to add diff tests

`diff_tests/` folder contains four directories corresponding to different areas
of trace processor.

1. **stdlib**: Tests focusing on testing Perfetto Standard Library, both prelude
   and the regular modules. The subdirectories in this folder should generally
   correspond to directories in `perfetto_sql/stdlib`.
2. **parser**: Tests focusing on ensuring that different trace files are parsed
   correctly and the corresponding built-in tables are populated.
3. **syntax**: Tests focusing on testing the core syntax of PerfettoSQL (i.e.
   `CREATE PERFETTO TABLE` or `CREATE PERFETTO FUNCTION`).

**Scenario**: A new stdlib module `foo/bar.sql` is being added.

_Answer_: Add the test to the `stdlib/foo/bar_tests.py` file.

**Scenario**: A new event is being parsed, the focus of the test is to ensure
the event is being parsed correctly.

_Answer_: Add the test in one of the `parser` subdirectories. Prefer adding a
test to an existing related directory (i.e. `sched`, `power`) if one exists.

**Scenario**: A new dynamic table is being added and the focus of the test is to
ensure the dynamic table is being correctly computed...

_Answer_: Add the test to the `stdlib/dynamic_tables` folder

**Scenario**: The interals of trace processor are being modified and the test is
to ensure the trace processor is correctly filtering/sorting important built-in
tables.

_Answer_: Add the test to the `parser/core_tables` folder.

## {#embedding} Embedding

### Building

As with all components in Perfetto, the trace processor can be built in several
build systems:

- GN (the native system)
- Bazel
- As part of the Android tree

The trace processor is exposed as a static library `//:trace_processor` to Bazel
and `src/trace_processor:trace_processor` in GN; it is not exposed to Android
(but patches to add support for this are welcome).

The trace processor is also built as a WASM target
`src/trace_processor:trace_processor_wasm` for the Perfetto UI; patches for
adding support for other supported build systems are welcome.

The trace processor is also built as a shell binary, `trace_processor_shell`
which backs the `trace_processor` tool described in other parts of the
documentation. This is exposed as the `trace_processor_shell` target to Android,
`//:trace_processor_shell` to Bazel and
`src/trace_processor:trace_processor_shell` in GN.

### Library structure

The trace processor library is structured around the `TraceProcessor` class; all
API methods exposed by trace processor are member functions on this class.

The C++ header for this class is split between two files:
[include/perfetto/trace_processor/trace_processor_storage.h](/include/perfetto/trace_processor/trace_processor_storage.h)
and
[include/perfetto/trace_processor/trace_processor.h](/include/perfetto/trace_processor/trace_processor.h).

### Reading traces

To ingest a trace into trace processor, the `Parse` function can be called
multiple times to with chunks of the trace and `NotifyEndOfFile` can be called
at the end.

As this is a common task, a helper function `ReadTrace` is provided in
[include/perfetto/trace_processor/read_trace.h](/include/perfetto/trace_processor/read_trace.h).
This will read a trace file directly from the filesystem and calls into
appropriate `TraceProcessor`functions to perform parsing.

### Executing queries

The `ExecuteQuery` function can be called with an SQL statement to execute. This
will return an iterator which can be used to retrieve rows in a streaming
fashion.

WARNING: embedders should ensure that the iterator is forwarded using `Next`
before any other functions are called on the iterator.

WARNING: embedders should ensure that the status of the iterator is checked
after every row and at the end of iteration to verify that the query was
successful.
