

























































- Perfetto supports natively recording traces in a Protobuf based tracing
  format.

Two primary ways Perfetto to operate:

- System mode where Perfetto can collect data from many sources and hook into
  low-level system functions for profiling.
- In-app mode where Perfetto provides APIs for collecting trace data fully
  inside a single process with no external deps

Moreover, three modes:

- Duration mode
- Flight recorder mode
- Long tracing mode

Data sources:

- ftrace
- atrace

## Perfetto

Perfetto is a suite of tools for software performance analysis. Its purpose is
to empower engineers to understand where resources are being used by their
systems. It helps identify the changes they can make to improve performance and
verify the impact of those changes.

NOTE: In Perfetto, since profiles and traces can be collected simultaneously, we
call everything a "trace" even if it may contain (only) profiling data inside.

### Recording traces

Perfetto is highly configurable when it comes to recording traces. There are
literally hundreds of knobs which can be tweaked to control what data is
collected, how it should be collected, how much information a trace should
contain etc.
<!-- 
[Record traces on Linux quickstart](/docs/quickstart/linux-tracing.md) is a good
place to start if you're unfamiliar with Perfetto. For Android developers,
[Record traces on Android quickstart](/docs/quickstart/android-tracing.md) will
be more applicable. The [trace configuration](/docs/concepts/config.md) page is
also useful to consult as a reference. -->

The following sub-sections give an overview of various points worth considering
when recording Perfetto traces.

#### Kernel tracing

Perfetto integrates closely with the Linux kernel's
[ftrace](https://www.kernel.org/doc/Documentation/trace/ftrace.txt) tracing
system to record kernel events (e.g. scheduling, syscalls, wakeups). The
[scheduling](/docs/data-sources/cpu-scheduling.md),
[syscall](/docs/data-sources/syscalls.md) and
[CPU frequency](/docs/data-sources/cpu-freq.md) data source pages give examples
of configuring ftrace collection.

Natively supported ftrace events can be found in the fields of
[this proto message](/docs/reference/trace-packet-proto.autogen#FtraceEvent).
Perfetto also supports collecting ftrace events it does not natively understand
(i.e. it does not have a protobuf message for) as a
["generic"](/docs/reference/trace-packet-proto.autogen#GenericFtraceEvent)
events. These events are encoded as key-value pairs, similar to a JSON
dictionary.

It is strongly discouraged to rely on generic events for production use cases:
the inefficient encoding causes trace size bloat and the
[trace processor](/docs/analysis/trace-processor.md) cannot parse them
meaningfully. Instead, support should be added for parsing important ftrace
events to Perfetto:
[here](/docs/contributing/common-tasks.md#add-a-new-ftrace-event) is a simple
set of steps to follow which are found.

#### Instrumentation with Perfetto SDK

Perfetto has a [C++ SDK](https://perfetto.dev/docs/instrumentation/tracing-sdk)
which can be used to instrument programs to emit tracing events. The SDK is
designed to be very low-overhead and is distributed in an "amalgamated" form of
a one `.cc` and one `.h` file, making it easy to integrate in any build system.

A C SDK is under active development and should be available for general usage by
Q2 2023. See [this doc](https://bit.ly/perfetto-c) for details (note viewing
this doc requires being a member of
[this group](https://groups.google.com/forum/#!forum/perfetto-dev))

A Java/Kotlin SDK for Android (as a
[JetPack library](https://developer.android.com/jetpack/androidx)). This is
under development but there is no set timescale for when an official release
will happen.

##### android.os.Trace (atrace) vs Perfetto SDK

NOTE: This section is only relevant for Android platform developers or Android
app developers with tracing experience. Other readers can safely skip this
section.

Perfetto has significant advantages over atrace. Some of the biggest advantages
include:

- performance: tracing to Perfetto from system/app code requires just a memory
  write which is far faster than the syscall latency imposed by atrace. This
  generally makes Perfetto anywhere from 3-4x faster than atrace
- features: atrace's API is extremely limited, lacking support for debug
  arguments, custom clocks, flow events. Perfetto has a far richer API allowing
  natural representation of data-flow.
- trace size: Perfetto supports various features (delta encoded timestamps,
  interned strings, protobuf encoding) which vastly reduce to size of trace
  files.

Unfortunately, there are also some downsides:

- dedicated thread: a thread dedicated to Perfetto is necessary for every
  process which wants to trace to Perfetto.
- wakeups on tracing start: currently, when tracing starts, every process
  registered for tracing is woken up which significantly limits how many
  processes can be traced. This limitation should be removed in coming quarters.

For now, the recommendation from the Perfetto team is to continue utilizing
atrace for most usecases: if you think you have a usecase which would benefit
from the SDK, please reach out to the team directly. By mid-2023, significant
progress should be made addressing the limitations of the current SDK allowing
more widespread adoption of the SDK.

<!--
TODO(lalitm): write the remainder of the doc using the following template

#### Native heap profiling

#### Java heap graphs

#### Callstack sampling


#### Flight recorder tracing
TODO(lalitm): write this.

##### Field tracing
TODO(lalitm): write this.

#### Clock sync
TODO(lalitm): write this.


#### Analysis
TODO(lalitm): write this.
* Trace processing
* UI
* httpd mode
* metrics
* Python


The remainder of this
page will focus on the applications of Perfetto to solve various performance
related problems.

## Solving problems with Perfetto
TODO(lalitm): write this.
* When to look into callstack sampling
* When to use memory profiling
* When to look at scheduling latency


TODO(lalitm): write this.

-->

# Trace Instrumentation

_In this page, you'll TODO_

Instrumentation is the process of adding annotations (i.e. code markers) to a
codebase with information about what the code is doing. These anntoations are
essentially structured logging: instead of arbitrary strings, the logging has a
schema and semantics associated with it.

Instrumentation allows understanding what the program is doing over time in
traces: this can be combined with Perfetto's other superpower on integration
with various data sources throughout Android and Linux to get full system view
into the behaviour of your app and its inteera

## Perfetto C SDK

## atrace (Android-only)

## ftrace (Linux kernel-only)

# Ad-hoc Analysis & Viz

_In this page, you'll TODO_

## Introduction

Often users already have a pre-existing tracing system they are using or
Perfetto's recording or instrumentation stack does not work for their needs.

In these cases, Perfetto's analysis and visualization tooling can be used
completely independently of the recording and instrumentation tools.

Perfetto already has support for a wide varierty of trace formats which are in
general use throughout the industry. And if your format is not natively
supported, we provide guidance on how you can convert your custom tracing format
to be visualized in Perfetto.

## Non-Perfetto formats

Support a wide range of formats in use in performance and tracing ecosystems:

1. Perfetto protobuf format
2. Chrome JSON format
3. Fuchsia tracing format
4. Firefox profiler JSON format
5. Android systrace format
6. Linux ftrace textual format
7. ART method tracing format
8. macOS instruments format
9. Perf textual format
10. Ninja logs format
11. Android logcat textual format
12. Android bugreport zip format

If a format is widely used, we generally prefer directly adding support for it
instead of having an external converter. Reasons are twofold

1. Allows us to better capture the semantics of the tracing format something
   which is often lost during conversion
2. Provides a more seamless experience by not requiring a pre-processing step
   before opening the trace.

## Converting to Perfetto

For formats that Perfetto does not support because they are not in wide use,
it's also possible to convert arbitrary tracing (or even timeline based data) to
Perfetto.
