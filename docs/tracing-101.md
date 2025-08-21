# What is Tracing?

NOTE: the word "tracing" in this document is used in the context of
**client-side** side software (e.g. programs running on a single machine). In
the server world, **tracing** is usually short for _distributed tracing_, a way
to collect data from many different servers to understand the flow of a
"request" throughout multiple services. As such, this document will _not_ be
useful to you if you are interested in such traces.

This page provides a bird's-eye view of performance analysis and tracing. The
aim is to orient people who have no idea what "tracing" is.

## Introduction to Performance

Performance analysis is concerned with making software run _better_. The
definition of _better_ varies widely and depends on the situation. Examples
include:

- performing the same work using fewer resources (CPU, memory, network, battery,
  etc.)
- increasing utilization of available resources
- identifying and eliminating unnecessary work altogether

Much of the difficulty in improving performance comes from identifying the root
cause of performance issues. Modern software systems are complicated, having a
lot of components and a web of cross-interactions. Techniques which help
engineers understand the execution of a system and pinpoint issues that are
critical.

**Tracing** and **profiling** are two such widely-used techniques for
performance analysis.

## Introduction to Tracing

**Tracing** involves collecting highly detailed data about the execution of a
system. A single continuous session of recording is called a trace file or
**trace** for short.

Traces contain enough detail to fully reconstruct the timeline of events. They
often include low-level kernel events like scheduler context switches, thread
wakeups, syscalls, etc. With the "right" trace, reproduction of a performance
bug is not needed as the trace provides all necessary context.

Application code is also **instrumented** in areas of the program which are
considered to be _important_. This instrumentation keeps track of what the
program was doing over time (e.g. which functions were being run, or how long
each call took) and context about the execution (e.g. what were the parameters
to a function call, or why was a function run).

The level of detail in traces makes it impractical to read traces directly like
a log file in all but the simplest cases. Instead, a combination of **trace
analysis** libraries and **trace viewers** are used. Trace analysis libraries
provide a way for users to extract and summarize trace events in a programmatic
manner. Trace viewers visualize the events in a trace on a timeline which give
users a graphical view of what their system was doing over time.

### Logging vs tracing

A good intuition is that logging is to functional testing what tracing is to
performance analysis. Tracing is, in a sense, "structured" logging: instead of
having arbitrary strings emitted from parts of the system, tracing reflects the
detailed state of a system in a structured way to allow reconstruction of the
timeline of events.

Moreover, tracing frameworks (like Perfetto) place heavy emphasis on having
minimal overhead. This is essential so that the framework does not significantly
disrupt whatever is being measured: modern frameworks are fast enough that they
can measure execution at the nanosecond level without significantly impacting
the execution speed of the program.

_Small aside: theoretically, tracing frameworks are powerful enough to act as a
logging system as well. However, the utilization of each in practice is
different enough that the two tend to be separate._

### Metrics vs tracing

Metrics are numerical values which track the performance of a system over time.
Usually metrics map to high-level concepts. Examples of metrics include: CPU
usage, memory usage, network bandwidth, etc. Metrics are collected directly from
the app or operating system while the program is running.

After glimpsing the power of tracing, a natural question arises: why bother with
high level metrics at all? Why not instead just use tracing and compute metrics
on resulting traces? In some settings, this may indeed be the right approach. In
local and lab situations using **trace-based metrics**, where metrics are
computed from traces instead of collecting them directly, is a powerful
approach. If a metric regresses, it's easy to open the trace to root cause why
that happened.

However, trace-based metrics are not a universal solution. When running in
production, the heavyweight nature of traces can make it impractical to collect
them 24/7. Computing a metric with a trace can take megabytes of data vs bytes
for direct metric collection.

Using metrics is the right choice when you want to understand the performance of
a system over time but do not want to or can not pay the cost of collecting
traces. In these situations, traces should be used as a **root-causing** tool.
When your metrics show there is a problem, targeted tracing can be rolled out to
understand why the regression may have happened.

## Introduction to Profiling

**Profiling** involves sampling some usage of a resource by a program. A single
continuous session of recording is known as a **profile**.

Each sample collects the function callstack (i.e. the line of code along with
all calling functions). Generally this information is aggregated across the
profile. For each seen callstack, the aggregation gives the percentage of usage
of the resource by that callstack. By far the most common types of profiling are
**memory profiling** and **CPU profiling**.

Memory profiling is used to understand which parts of a program are allocating
memory on the heap. The profiler generally hooks into `malloc` (and `free`)
calls of a native (C/C++/Rust/etc.) program to sample the callstacks calling
`malloc`. Information about how many bytes were allocated is also retained. CPU
profiling is used for understanding where the program is spending CPU time. The
profiler captures the callstack running on a CPU over time. Generally this is
done periodically (e.g. every 50ms), but can be also be done when certain events
happen in the operating system.

### Profiling vs tracing

There are two main questions for comparing profiling and tracing:

1. Why profile my program statistically when I can just trace _everything_?
2. Why use tracing to reconstruct the timeline of events when profiling gives me
   the exact line of code using the most resources?

#### When to use profiling over tracing

Traces cannot feasibly capture execution of extreme high frequency events e.g.
every function call. Profiling tools fill this niche: by sampling, they can
significantly cut down on how much information they store. The statistical
nature of profilers are rarely a problem; the sampling algorithms for profilers
are specifically designed to capture data which is highly representative of the
real resource use.

*Aside: a handful of very specialized tracing tools exist which can capture
every function call (e.g.
[magic-trace](https://github.com/janestreet/magic-trace)) but they output
*gigabytes* of data every second which make them impractical for anything beyond
investigating tiny snippets of code. They also generally have higher overhead
than general purpose tracing tools.*

#### When to use tracing over profiling

While profilers give callstacks where resources are being used, they lack
information about _why_ that happened. For example, why was malloc being called
by function _foo()_ so many times? All they say is _foo()_ allocated X bytes
over Y calls to `malloc`. Traces are excellent at providing this exact context:
application instrumentation and low-level kernel events together provide deep
insight into why code was run in the first place.

NOTE: Perfetto supports collecting, analyzing and visualizing both profiles and
traces at the same time so you can have the best of both worlds!

## Next Steps

Now that you have a better understanding of tracing and profiling, you can use
Perfetto to:

- **Record a trace** of your application and system to understand its behavior.
- **Analyze a trace** to identify performance bottlenecks.
- **Visualize a trace** to see a timeline of events.

To learn how to do this, head to our
[How do I start using Perfetto?](/docs/getting-started/start-using-perfetto.md)
page.
