# Tracing 101
*This page provides a birds-eye view of performance analysis.
The aim is to orient people who have no idea what "tracing" is.*

## Introduction to...
### Performance
Performance analysis is concerned with making software run *better*.
The definition of *better* varies widely and depends on the situation.
Examples include:
* performing the same work using fewer resources (CPU, memory,
  network, battery, etc.)
* increasing utilization of available resources
* identifying and eliminating unnecessary work altogether

Much of the difficulty in improving performance comes from
identifying the root cause of performance issues. Modern software systems are
complicated, having a lot of components and a web of cross-interactions.
Techniques which help engineers understand the execution of a system
and pinpoint issues that are critical.

**Tracing** and **profiling** are two such widely-used techniques for
performance analysis. **Perfetto** is an open-source suite of tools, combining
tracing and profiling to give users powerful insights into their system.

### Tracing
**Tracing** involves collecting highly detailed data about the execution
of a system. A single continuous session of recording is called a trace file
or **trace** for short.

Traces contain enough detail to fully reconstruct the timeline of events.
They often include low-level kernel events like scheduler context switches,
thread wakeups, syscalls, etc. With the "right" trace, reproduction of a
performance bug is not needed as the trace provides all necessary context.

Application code is also **instrumented** in areas of the program which are
considered to be *important*. This instrumentation keeps track of what the
program was doing over time (e.g. which functions were being run, or how long
each call took) and context about the execution (e.g. what were the parameters
to a function call, or why was a function run).

The level of detail in traces makes it impractical to read traces directly
like a log file in all but the simplest cases. Instead, a combination of
**trace analysis** libraries and **trace viewers** are used. Trace analysis
libraries provide a way for users to extract and summarize trace events in
a programmatic manner. Trace viewers visualize the events in a trace on a
timeline which give users a graphical view of what their system was doing
over time.

#### Logging vs tracing
A good intuition is that logging is to functional testing what
tracing is to performance analysis. Tracing is, in a sense, "structured"
logging: instead of having arbitrary strings emitted from parts of the system,
tracing reflects the detailed state of a system in a structured way to allow
reconstruction of the timeline of events.

Moreover, tracing frameworks (like Perfetto) place heavy emphasis
on having minimal overhead. This is essential so that the framework
does not significantly disrupt whatever is being measured: modern frameworks
are fast enough that they can measure execution at the nanosecond level
without significantly impacting the execution speed of the program.

*Small aside: theoretically, tracing frameworks are powerful enough to act as
a logging system as well. However, the utilization of each in practice is
different enough that the two tend to be separate.*

#### Metrics vs tracing
Metrics are numerical values which track the performance of a system over time.
Usually metrics map to high-level concepts. Examples of metrics include: CPU
usage, memory usage, network bandwidth, etc. Metrics are collected directly from
the app or operating system while the program is running.

After glimpsing the power of tracing, a natural question arises: why bother
with high level metrics at all? Why not instead just use use tracing and
compute metrics on resulting traces? In some settings, this may indeed be the
right approach. In local and lab situations using **trace-based metrics**,
where metrics are computed from traces instead of collecting them directly,
is a powerful approach. If a metric regresses, it's easy to open the trace
to root cause why that happened.

However, trace-based metrics are not a universal solution. When running in
production, the heavyweight nature of traces can make it impractical to collect
them 24/7. Computing a metric with a trace can take megabytes of data vs bytes
for direct metric collection.

Using metrics is the right choice when you want to understand the performance
of a system over time but do not want to or can not pay the cost of collecting
traces. In these situations, traces should be used as a **root-causing** tool.
When your metrics show there is a problem, targeted tracing can be rolled out
to understand why the regression may have happened.

### Profiling
**Profiling** involves sampling some usage of a resource by
a program. A single continuous session of recording is known as a **profile**.

Each sample collects the function callstack (i.e. the line of code along with
all calling functions). Generally this information is aggregated across the
profile. For each seen callstack, the aggregation gives the percentage of usage
of the resource by that callstack. By far the most common types of profiling are
**memory profiling** and **CPU profiling**.

Memory profiling is used to understand which parts of a program are allocating
memory on the heap. The profiler generally hooks into `malloc` (and `free`)
calls of a native (C/C++/Rust/etc.) program to sample the callstacks
calling `malloc`. Information about how many bytes were allocated is also
retained. CPU profiling is used for understanding where the program is
spending CPU time. The profiler captures the callstack running on a CPU
over time. Generally this is done periodically (e.g. every 50ms), but can be
also be done when certain events happen in the operating system.

#### Profiling vs tracing
There are two main questions for comparing profiling and tracing:
1. Why profile my program statistically when I can just trace *everything*?
2. Why use tracing to reconstruct the timeline of events when profiling gives me
   the exact line of code using the most resources?

##### When to use profiling over tracing
Traces cannot feasibly capture execution of extreme high frequency
events e.g. every function call. Profiling tools fill this niche: by
sampling, they can significantly cut down on how much information they store.
The statistical nature of profilers are rarely a problem; the sampling
algorithms for profilers are specifically designed to capture data which is
highly representative of the real resource use.

*Aside: a handful of very specialized tracing tools exist which
can capture every function call (e.g.
[magic-trace](https://github.com/janestreet/magic-trace)) but they output
*gigabytes* of data every second which make them impractical for anything
beyond investigating tiny snippets of code. They also generally have higher
overhead than general purpose tracing tools.*

##### When to use tracing over profiling
While profilers give callstacks where resources are being used, they lack
information about *why* that happened. For example, why was malloc being called
by function *foo()* so many times? All they say is *foo()* allocated X bytes
over Y calls to `malloc`. Traces are excellent at providing this exact context:
application instrumentation and low-level kernel events together provide
deep insight into why code was run in the first place.

NOTE: Perfetto supports collecting, analyzing and visualizing both profiles
and traces at the same time so you can have the best of both worlds!

## Perfetto
Perfetto is a suite of tools for performance analysis of software. Its purpose
is to empower engineers to understand where resources are being used by their
systems. It helps identify the changes they can make to improve performance
and verify the impact of those changes.

NOTE: In Perfetto, since profiles and traces can be collected simultaneously,
we call everything a "trace" even if it may contain (only) profiling data
inside.

### Recording traces
Perfetto is highly configurable when it comes to recording traces. There are
literally hundreds of knobs which can be tweaked to control what data is
collected, how it should be collected, how much information a trace should
contain etc.

[Record traces on Linux quickstart](/docs/quickstart/linux-tracing.md) is
a good place to start if you're unfamiliar with Perfetto. For Android
developers,
[Record traces on Android quickstart](/docs/quickstart/android-tracing.md) will
be more applicable. The [trace configuration](/docs/concepts/config.md) page
is also useful to consult as a reference.

The following sub-sections give an overview of various points worth considering
when recording Perfetto traces.

#### Kernel tracing
Perfetto integrates closely with the Linux kernel's
[ftrace](https://www.kernel.org/doc/Documentation/trace/ftrace.txt) tracing
system to record kernel events (e.g. scheduling, syscalls, wakeups). The
[scheduling](/docs/data-sources/cpu-scheduling.md),
[syscall](/docs/data-sources/syscalls.md) and
[CPU frequency](/docs/data-sources/cpu-freq.md) data source pages give
examples of configuring ftrace collection.

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
designed to be very low-overhead and is distributed in an "amalgamated" form
of a one `.cc` and one `.h` file, making it easy to integrate in any build
system.

A C SDK is under active development and should be available for general
usage by Q2 2023. See [this doc](https://bit.ly/perfetto-c) for details (note
viewing this doc requires being a member of
[this group](https://groups.google.com/forum/#!forum/perfetto-dev))

A Java/Kotlin SDK for Android (as a
[JetPack library](https://developer.android.com/jetpack/androidx)).
This is under development but there is no set timescale for when an official
release will happen.

##### android.os.Trace (atrace) vs Perfetto SDK
NOTE: This section is only relevant for Android platform developers or Android
app developers with tracing experience. Other readers can safely skip this
section.

Perfetto has significant advantages over atrace. Some of the biggest advantages
include:
* performance: tracing to Perfetto from system/app code requires just a memory
  write which is far faster than the syscall latency imposed by atrace. This
  generally makes Perfetto anywhere from 3-4x faster than atrace
* features: atrace's API is extremely limited, lacking support for debug
  arguments, custom clocks, flow events. Perfetto has a far richer API allowing
  natural representation of data-flow.
* trace size: Perfetto supports various features (delta encoded timestamps,
  interned strings, protobuf encoding) which vastly reduce to size of trace
  files.

Unfortunately, there are also some downsides:
* dedicated thread: a thread dedicated to Perfetto is necessary for every
  process which wants to trace to Perfetto.
* wakeups on tracing start: currently, when tracing starts, every process
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