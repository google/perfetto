# Perfetto - System profiling, app tracing and trace analysis

Perfetto is a production-grade open-source stack for performance
instrumentation and trace analysis. It offers services and libraries for
recording system-level and app-level traces, native + java heap profiling, a
library for analyzing traces using SQL and a web-based UI to visualize and
explore multi-GB traces.

![Perfetto stack](/docs/images/perfetto-stack.svg)

## Recording traces

At its core, Perfetto introduces a novel userspace-to-userspace
[tracing protocol](/docs/design-docs/api-and-abi.md#tracing-protocol-abi) based
on direct protobuf serialization onto a shared memory buffer. The tracing
protocol is used both internally for the built-in data sources and exposed to
C++ apps through the [Tracing SDK](/docs/instrumentation/tracing-sdk.md) and the
[Track Event Library](/docs/instrumentation/track-events.md).

This new tracing protocol allows dynamic configuration of all aspects of tracing
through an extensible protobuf-based capability advertisement and data source
configuration mechanism (see
[Trace configuration docs](/docs/concepts/config.md)).
Different data sources can be multiplexed onto different subsets of
user-defined buffers, allowing also streaming of
[arbitrarily long traces](/docs/concepts/config.md#long-traces) into the
filesystem.

### System-wide tracing on Android and Linux

On Linux and Android, Perfetto bundles a number of data sources that are able to
gather detailed performance data from different system interfaces. For the full
sets and details see the _Data Sources_ section of the documentation. Some
examples:

* [Kernel tracing](/docs/data-sources/cpu-scheduling.md): Perfetto integrates
  with [Linux's ftrace][ftrace] and allows to record kernel events (e.g
  scheduling events, syscalls) into the trace.

* [/proc and /sys pollers](/docs/data-sources/memory-counters.md), which allow
  to sample the state of process-wide or system-wide cpu and memory counters
  over time.

* Integration with Android HALs modules for recording [battery and energy-usage
  counters](/docs/data-sources/battery-counters.md).

* [Native heap profiling](/docs/data-sources/native-heap-profiler.md): a
  low-overhead heap profiler for hooking malloc/free/new/delete and associating
  memory to call-stacks, based on out-of-process unwinding, configurable
  sampling, attachable to already running processes.

* Capturing [Java heap dumps](/docs/data-sources/java-heap-profiler.md) with an
  out-of-process profiler tightly integrated with the Android RunTime that
  allows to get full snapshots of the managed heap retention graph (types,
  field names, retained size and references to other objects) without, however,
  dumping the full heap contents (strings and bitmaps) and hence reducing the
  serialization time and output file size.

On Android, Perfetto is the next-generation system tracing system and replaces
the chromium-based systrace.
[ATrace-based instrumentation](/docs/data-sources/atrace.md) remains fully
supported.
See [Android developer docs](https://developer.android.com/topic/performance/tracing)
for more details.

### Tracing SDK and user-space instrumentation

The [Perfetto Tracing SDK](/docs/instrumentation/tracing-sdk.md) enables C++
developers to enrich traces with app-specific trace points. You can choose
between the flexibility of defining your own strongly-typed events and creating
custom data sources or using the easier-to-use
[Track Event Library](/docs/instrumentation/track-events.md) which allows to
easily create time-bounded slices, counters and time markers using annotations
of the form `TRACE_EVENT("category", "event_name", "x", "str", "y", 42)`.

The SDK is designed for tracing of multi-process systems and multi-threaded
processes. It is based on [ProtoZero](/docs/design-docs/protozero.md), a library
for direct writing of protobuf events on thread-local shared memory buffers.

The same code can work both in fully-in-process mode, hosting an instance of the
Perfetto tracing service on a dedicated thread, or in _system mode_, connecting
to the Linux/Android tracing daemon through a UNIX socket, allowing to combine
app-specific instrumentation points with system-wide tracing events.

The SDK is based on portable C++17 code [tested](/docs/contributing/testing.md)
with the major C++ sanitizers (ASan, TSan, MSan, LSan). It doesn't rely on
run-time code modifications or compiler plugins.

### Tracing in Chromium

Perfetto has been designed from the grounds to replace the internals of the
[chrome://tracing infrastructure][chrome-tracing]. Tracing in Chromium and its
internals are based on Perfetto's codebase on all major platforms (Android,
CrOS, Linux, MacOS, Windows).
The same [service-based architecture](/docs/concepts/service-model.md) of
system-wide tracing applies, but internally the Chromium Mojo IPC system is
used instead of Perfetto's own UNIX socket.

By default tracing works in in-process mode in Chromium, recording only data
emitted by Chromium processes. On Android (and on Linux, if disabling the
Chromium sandbox) tracing can work in hybrid in-process+system mode, combining
chrome-specific trace events with Perfetto system events.

_(Googlers: see [go/chrometto](https://goto.google.com/chrometto) for more)_

## Trace analysis

Beyond the trace recording capabilities, the Perfetto codebase includes a
dedicated project for importing, parsing and querying new and legacy trace
formats, [Trace Processor](/docs/analysis/trace-processor.md).

Trace Processor is a portable C++17 library that provides column-oriented
table storage, designed ad-hoc for efficiently holding hours of trace data
into memory and exposes a SQL query interface based on the popular SQLite query
engine.
The trace data model becomes a set of
[SQL tables](/docs/analysis/sql-tables.autogen) which can be queried and joined
in extremely powerful and flexible ways to analyze the trace data.

On top of this, Trace Processor includes also a
[trace-based metrics subsystem](/docs/analysis/metrics.md) consisting of
pre-baked and extensible queries that can output strongly-typed summaries
about a trace in the form of JSON or protobuf messages (e.g., the CPU usage
at different frequency states, breakdown by process and thread).

Trace-based metrics allow an easy integration of traces in performance testing
scenarios or batch analysis or large corpuses of traces.

Trace Processor is also designed for low-latency queries and for building
trace visualizers. Today Trace Processor is used by the
[Perfetto UI](https://ui.perfetto.dev) as a Web Assembly module,
[Android Studio](https://developer.android.com/studio) and
[Android GPU Inspector](https://gpuinspector.dev/) as native C++ library.

## Trace visualization

Perfetto provides also a brand new trace visualizer for opening and querying
hours-long traces, available at [ui.perfetto.dev](https://ui.perfetto.dev).
The new visualizer takes advantage of modern web platform technologies.
Its multi-threading design based WebWorkers keeps the UI always responsive;
the analytical power of Trace Processor and SQLite is fully available in-browser
through WebAssembly.

The Perfetto UI works fully offline after it has been opened once. Traces opened
with the UI are processed locally by the browser and do not require any
server-side interaction.

![Perfetto UI screenshot](/docs/images/perfetto-ui-screenshot.png)

## Contributing

See the [Contributing -> Getting started page](/docs/contributing/getting-started.md).

## Bugs

For bugs affecting Android or the tracing internals:

* **Googlers**: use the internal bug tracker [go/perfetto-bugs](http://goto.google.com/perfetto-bugs)

* **Non-Googlers**: use [GitHub issues](https://github.com/google/perfetto/issues).

For bugs affecting Chrome Tracing:

* **Googlers**: use the internal bug tracker [go/chrometto-bugs](http://goto.google.com/chrometto-bugs)

* **Non-Googlers**: use [crbug.com](https://bugs.chromium.org/p/chromium/issues/list?q=component%3ASpeed%3ETracing%20label%3APerfetto)
to [file new bugs](https://bugs.chromium.org/p/chromium/issues/entry?components=Speed%3ETracing&labels=Perfetto).

[ftrace]: https://www.kernel.org/doc/Documentation/trace/ftrace.txt
[chrome-tracing]: https://www.chromium.org/developers/how-tos/trace-event-profiling-tool
