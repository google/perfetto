# Trace Processor

_The Trace Processor is a C++ library
([/src/trace_processor](/src/trace_processor)) that ingests traces encoded in a
wide variety of formats and exposes an SQL interface for querying trace events
contained in a consistent set of tables. It also has other features including
computation of summary metrics, annotating the trace with user-friendly
descriptions and deriving new events from the contents of the trace._

![Trace processor block diagram](/docs/images/trace-processor.png)

## Quickstart

The [quickstart](/docs/quickstart/trace-analysis.md) provides a quick overview
on how to run SQL queries against traces using trace processor.

## Introduction

Events in a trace are optimized for fast, low-overhead recording. Therefore
traces need significant data processing to extract meaningful information from
them. This is compounded by the number of legacy formats which are still in use and
need to be supported in trace analysis tools.

The trace processor abstracts this complexity by parsing traces, extracting the
data inside, and exposing it in a set of database tables which can be queried
with SQL.

Features of the trace processor include:

* Execution of SQL queries on a custom, in-memory, columnar database backed by
  the SQLite query engine.
* Metrics subsystem which allows computation of summarized view of the trace
  (e.g. CPU or memory usage of a process, time taken for app startup etc.).
* Annotating events in the trace with user-friendly descriptions, providing
  context and explanation of events to newer users.
* Creation of new events derived from the contents of the trace.

The formats supported by trace processor include:

* Perfetto native protobuf format
* Linux ftrace
* Android systrace
* Chrome JSON (including JSON embedding Android systrace text)
* Fuchsia binary format
* [Ninja](https://ninja-build.org/) logs (the build system)

The trace processor is embedded in a wide variety of trace analysis tools, including:

* [trace_processor](/docs/analysis/trace-processor.md), a standalone binary
   providing a shell interface (and the reference embedder).
* [Perfetto UI](https://ui.perfetto.dev), in the form of a WebAssembly module.
* [Android Graphics Inspector](https://gpuinspector.dev/).
* [Android Studio](https://developer.android.com/studio/).

## Concepts

The trace processor has some foundational terminology and concepts which are
used in the rest of documentation.

### Events

In the most general sense, a trace is simply a collection of timestamped
"events". Events can have associated metadata and context which allows them to
be interpreted and analyzed.

Events form the foundation of trace processor and are one of two types: slices
and counters.

#### Slices

![Examples of slices](/docs/images/slices.png)

A slice refers to an interval of time with some data describing what was
happening in that interval. Some example of slices include:

* Scheduling slices for each CPU
* Atrace slices on Android
* Userspace slices from Chrome

#### Counters

![Examples of counters](/docs/images/counters.png)

A counter is a continuous value which varies over time. Some examples of
counters include:

* CPU frequency for each CPU core
* RSS memory events - both from the kernel and polled from /proc/stats
* atrace counter events from Android
* Chrome counter events

### Tracks

A track is a named partition of events of the same type and the same associated
context. For example:

* Scheduling slices have one track for each CPU
* Sync userspace slice have one track for each thread which emitted an event
* Async userspace slices have one track for each “cookie” linking a set of async
  events

The most intuitive way to think of a track is to imagine how they would be drawn
in a UI; if all the events are in a single row, they belong to the same track.
For example, all the scheduling events for CPU 5 are on the same track:

![CPU slices track](/docs/images/cpu-slice-track.png)

Tracks can be split into various types based on the type of event they contain
and the context they are associated with. Examples include:

* Global tracks are not associated to any context and contain slices
* Thread tracks are associated to a single thread and contain slices
* Counter tracks are not associated to any context and contain counters
* CPU counter tracks are associated to a single CPU and contain counters

### Thread and process identifiers

The handling of threads and processes needs special care when considered in the
context of tracing; identifiers for threads and processes (e.g. `pid`/`tgid` and
`tid` in Android/macOS/Linux) can be reused by the operating system over the
course of a trace. This means they cannot be relied upon as a unique identifier
when querying tables in trace processor.

To solve this problem, the trace processor uses `utid` (_unique_ tid) for
threads and `upid` (_unique_ pid) for processes. All references to threads and
processes (e.g. in CPU scheduling data, thread tracks) uses `utid` and `upid`
instead of the system identifiers.

## Object-oriented tables

Modeling an object with many types is a common problem in trace processor. For
example, tracks can come in many varieties (thread tracks, process tracks,
counter tracks etc). Each type has a piece of data associated to it unique to
that type; for example, thread tracks have a `utid` of the thread, counter
tracks have the `unit` of the counter.

To solve this problem in object-oriented languages, a `Track` class could be
created and inheritance used for all subclasses (e.g. `ThreadTrack` and
`CounterTrack` being subclasses of `Track`, `ProcessCounterTrack` being a
subclass of `CounterTrack` etc).

![Object-oriented table diagram](/docs/images/oop-table-inheritance.png)

In trace processor, this "object-oriented" approach is replicated by having
different tables for each type of object. For example, we have a `track` table
as the "root" of the hierarchy with the `thread_track` and `counter_track`
tables "inheriting from" the `track` table.

NOTE: [The appendix below](#appendix-table-inheritance) gives the exact rules
for inheritance between tables for interested readers.

Inheritance between the tables works in the natural way (i.e. how it works in
OO languages) and is best summarized by a diagram.

![SQL table inheritance diagram](/docs/images/tp-table-inheritance.png)

NOTE: For an up-to-date of how tables currently inherit from each other as well
as a comprehensive reference of all the column and how they are inherited see
the [SQL tables](/docs/analysis/sql-tables.autogen) reference page.

## Writing Queries

### Context using tracks

A common question when querying tables in trace processor is: "how do I obtain
the process or thread for a slice?". Phrased more generally, the question is
"how do I get the context for an event?".

In trace processor, any context associated with all events on a track is found
on the associated `track` tables.

For example, to obtain the `utid` of any thread which emitted a `measure` slice

```sql
SELECT utid
FROM slice
JOIN thread_track ON thread_track.id = slice.track_id
WHERE slice.name = 'measure'
```

Similarly, to obtain the `upid`s of any process which has a `mem.swap` counter
greater than 1000

```sql
SELECT upid
FROM counter
JOIN process_counter_track ON process_counter_track.id = slice.track_id
WHERE process_counter_track.name = 'mem.swap' AND value > 1000
```

If the source and type of the event is known beforehand (which is generally the
case), the following can be used to find the `track` table to join with

| Event type | Associated with    | Track table           | Constraint in WHERE clause |
| :--------- | ------------------ | --------------------- | -------------------------- |
| slice      | N/A (global scope) | track                 | `type = 'track'`           |
| slice      | thread             | thread_track          | N/A                        |
| slice      | process            | process_track         | N/A                        |
| counter    | N/A (global scope) | counter_track         | `type = 'counter_track'`   |
| counter    | thread             | thread_counter_track  | N/A                        |
| counter    | process            | process_counter_track | N/A                        |
| counter    | cpu                | cpu_counter_track     | N/A                        |

On the other hand, sometimes the source is not known. In this case, joining with
the `track `table and looking up the `type` column will give the exact track
table to join with.

For example, to find the type of track for `measure` events, the following query
could be used.

```sql
SELECT type
FROM slice
JOIN track ON track.id = slice.track_id
WHERE slice.name = 'measure'
```

### Thread and process tables

While obtaining `utid`s and `upid`s are a step in the right direction, generally
users want the original `tid`, `pid`, and process/thread names.

The `thread` and `process` tables map `utid`s and `upid`s to threads and
processes respectively. For example, to lookup the thread with `utid` 10

```sql
SELECT tid, name
FROM thread
WHERE utid = 10
```

The `thread` and `process` tables can also be joined with the associated track
tables directly to jump directly from the slice or counter to the information
about processes and threads.

For example, to get a list of all the threads which emitted a `measure` slice

```sql
SELECT thread.name AS thread_name
FROM slice
JOIN thread_track ON slice.track_id = thread_track.id
JOIN thread USING(utid)
WHERE slice.name = 'measure'
GROUP BY thread_name
```

## Metrics

TIP: To see how to add to add a new metric to trace processor, see the checklist
[here](/docs/contributing/common-tasks.md#new-metric).

The metrics subsystem is a significant part of trace processor and thus is
documented on its own [page](/docs/analysis/metrics.md).

## Annotations

TIP: To see how to add to add a new annotation to trace processor, see the
checklist [here](/docs/contributing/common-tasks.md#new-annotation).

Annotations attach a human-readable description to a slice in the trace. This
can include information like the source of a slice, why a slice is important and
links to documentation where the viewer can learn more about the slice.
In essence, descriptions act as if an expert was telling the user what the slice
means.

For example, consider the `inflate` slice which occurs during view inflation in
Android. We can add the following description and link:

**Description**: Constructing a View hierarchy from pre-processed XML via
LayoutInflater#layout. This includes constructing all of the View objects in the
hierarchy, and applying styled attributes.

## Creating derived events

TIP: To see how to add to add a new annotation to trace processor, see the
     checklist [here](/docs/contributing/common-tasks.md#new-annotation).

This feature allows creation of new events (slices and counters) from the data
in the trace. These events can then be displayed in the UI tracks as if they
were part of the trace itself.

This is useful as often the data in the trace is very low-level. While low
level information is important for experts to perform deep debugging, often
users are just looking for a high level overview without needing to consider
events from multiple locations.

For example, an app startup in Android spans multiple components including
`ActivityManager`, `system_server`, and the newly created app process derived
from `zygote`. Most users do not need this level of detail; they are only
interested in a single slice spanning the entire startup.

Creating derived events is tied very closely to
[metrics subsystem](/docs/analysis/metrics.md); often SQL-based metrics need to
create higher-level abstractions from raw events as intermediate artifacts.

From previous example, the
[startup metric](/src/trace_processor/metrics/android/android_startup.sql)
creates the exact `launching` slice we want to display in the UI.

The other benefit of aligning the two is that changes in metrics are
automatically kept in sync with what the user sees in the UI.

## Alerts

Alerts are used to draw the attention of the user to interesting parts of the
trace; this are usually warnings or errors about anomalies which occurred in the
trace.

Currently, alerts are not implemented in the trace processor but the API to
create derived events was designed with them in mind. We plan on adding another
column `alert_type` (name to be finalized) to the annotations table which can
have the value `warning`, `error` or `null`. Depending on this value, the
Perfetto UI will flag these events to the user.

NOTE: we do not plan on supporting case where alerts need to be added to
      existing events. Instead, new events should be created using annotations
      and alerts added on these instead; this is because the trace processor
      storage is monotonic-append-only.

## Appendix: table inheritance

Concretely, the rules for inheritance between tables works are as follows:

* Every row in a table has an `id` which is unique for a hierarchy of tables.
  * For example, every `track` will have an `id` which is unique among all
    tracks (regardless of the type of track)
* If a table C inherits from P, each row in C will also be in P _with the same
  id_
  * This allows for ids to act as "pointers" to rows; lookups by id can be
    performed on any table which has that row
  * For example, every `process_counter_track` row will have a matching row in
    `counter_track` which will itself have matching rows in `track`
* If a table C with columns `A` and `B` inherits from P with column `A`, `A`
  will have the same data in both C and P
  * For example, suppose
    *  `process_counter_track` has columns `name`, `unit` and `upid`
    *  `counter_track` has `name` and `unit`
    *  `track` has `name`
  * Every row in `process_counter_track` will have the same `name`  for the row
    with the same id in  `track` and `counter_track`
  * Similarly, every row in `process_counter_track` will have both the same
    `name ` and `unit` for the row with the same id in `counter_track`
* Every row in a table has a `type` column. This specifies the _most specific_
  table this row belongs to.
  * This allows _dynamic casting_ of a row to its most specific type
  * For example, for if a row in the `track` is actually a
    `process_counter_track`, it's type column will be `process_counter_track`.
