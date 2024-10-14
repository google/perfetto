# Embedding Perfetto

## Trace Processor

### Building

As with all components in Perfetto, the trace processor can be built in several build systems:

- GN (the native system)
- Bazel
- As part of the Android tree

The trace processor is exposed as a static library `//:trace_processor` to Bazel and `src/trace_processor:trace_processor` in GN; it is not exposed to Android (but patches to add support for this are welcome).

The trace processor is also built as a WASM target `src/trace_processor:trace_processor_wasm` for the Perfetto UI; patches for adding support for other supported build systems are welcome.

The trace processor is also built as a shell binary, `trace_processor_shell` which backs the `trace_processor` tool described in other parts of the documentation. This is exposed as the `trace_processor_shell` target to Android, `//:trace_processor_shell` to Bazel and `src/trace_processor:trace_processor_shell` in GN.

### Library structure

The trace processor library is structured around the `TraceProcessor` class; all API methods exposed by trace processor are member functions on this class.

The C++ header for this class is split between two files:  [include/perfetto/trace_processor/trace_processor_storage.h](/include/perfetto/trace_processor/trace_processor_storage.h) and [include/perfetto/trace_processor/trace_processor.h](/include/perfetto/trace_processor/trace_processor.h).

### Reading traces

To ingest a trace into trace processor, the `Parse` function can be called multiple times to with chunks of the trace and `NotifyEndOfFile` can be called at the end.

As this is a common task, a helper function `ReadTrace` is provided in [include/perfetto/trace_processor/read_trace.h](/include/perfetto/trace_processor/read_trace.h). This will read a trace file directly from the filesystem and calls into appropriate `TraceProcessor`functions to perform parsing.

### Executing queries

The `ExecuteQuery` function can be called with an SQL statement to execute. This will return an iterator which can be used to retrieve rows in a streaming fashion.

WARNING: embedders should ensure that the iterator is forwarded using `Next` before any other functions are called on the iterator.

WARNING: embedders should ensure that the status of the iterator is checked after every row and at the end of iteration to verify that the query was successful.

### Metrics

Any registered metrics can be computed using using the `ComputeMetric` function. Any metric in `src/trace_processor/metrics` is built-in to trace processor so can be called without any other steps.

Metrics can also be registered at run time using the `RegisterMetric` and `ExtendMetricsProto` functions. These can subsequently be executed with `ComputeMetric`.

WARNING: embedders should ensure that the path of any registered metric is consistent with the name used to execute the metric and output view in the SQL.
