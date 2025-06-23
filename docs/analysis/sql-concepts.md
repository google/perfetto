# SQL Concepts

The trace processor has some foundational terminology and concepts which are
used in the rest of documentation.

## Events

In the most general sense, a trace is simply a collection of timestamped
"events". Events can have associated metadata and context which allows them to
be interpreted and analyzed. Timestamps are in nanoseconds; the values
themselves depend on the
[clock](https://cs.android.com/android/platform/superproject/main/+/main:external/perfetto/protos/perfetto/config/trace_config.proto;l=114;drc=c74c8cf69e20d7b3261fb8c5ab4d057e8badce3e)
selected in TraceConfig.

Events form the foundation of trace processor and are one of two types: slices
and counters.

### Slices

![Examples of slices](/docs/images/slices.png)

A slice refers to an interval of time with some data describing what was
happening in that interval. Some example of slices include:

- Scheduling slices for each CPU
- Atrace slices on Android
- Userspace slices from Chrome

### Counters

![Examples of counters](/docs/images/counters.png)

A counter is a continuous value which varies over time. Some examples of
counters include:

- CPU frequency for each CPU core
- RSS memory events - both from the kernel and polled from /proc/stats
- atrace counter events from Android
- Chrome counter events

## Tracks

A track is a named partition of events of the same type and the same associated
context. For example:

- Scheduling slices have one track for each CPU
- Sync userspace slice have one track for each thread which emitted an event
- Async userspace slices have one track for each “cookie” linking a set of async
  events

The most intuitive way to think of a track is to imagine how they would be drawn
in a UI; if all the events are in a single row, they belong to the same track.
For example, all the scheduling events for CPU 5 are on the same track:

![CPU slices track](/docs/images/cpu-slice-track.png)

Tracks can be split into various types based on the type of event they contain
and the context they are associated with. Examples include:

- Global tracks are not associated to any context and contain slices
- Thread tracks are associated to a single thread and contain slices
- Counter tracks are not associated to any context and contain counters
- CPU counter tracks are associated to a single CPU and contain counters

## Thread and process identifiers

The handling of threads and processes needs special care when considered in the
context of tracing; identifiers for threads and processes (e.g. `pid`/`tgid` and
`tid` in Android/macOS/Linux) can be reused by the operating system over the
course of a trace. This means they cannot be relied upon as a unique identifier
when querying tables in trace processor.

To solve this problem, the trace processor uses `utid` (_unique_ tid) for
threads and `upid` (_unique_ pid) for processes. All references to threads and
processes (e.g. in CPU scheduling data, thread tracks) uses `utid` and `upid`
instead of the system identifiers.
