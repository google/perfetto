# Getting Started with PerfettoSQL

PerfettoSQL is the foundation of trace analysis in Perfetto. It is a dialect of
SQL that allows you to query the contents of your traces as if they were a
database. This page introduces the core concepts of trace querying with PerfettoSQL
and provides guidance on how to write queries.

## Overview of Trace Querying

The Perfetto UI is a powerful tool for visual analysis, offering call stacks, timeline views, thread tracks, and slices. However, it also includes a robust SQL query language (PerfettoSQL) which is interpreted by a query engine ([TraceProcessor](trace-processor.md)) which allows you to extract data programmatically.

While the UI is powerful for myriad of analyses, users are able to write and execute queries within the Perfetto UI for multiple purposes such as:

- Extracting performance data from traces.
- Create custom visualizations (Debug tracks) to perform more complex analyses.
- Creating derived metrics.
- Identify performance bottlenecks using data-driven logic.

Beyond the Perfetto UI, you can query traces programmatically using the [Python Trace Processor API](trace-processor-python.md) or the [C++ Trace Processor](trace-processor.md).

Perfetto also supports bulk trace analysis through the [Batch Trace Processor](batch-trace-processor.md). A key advantage of this system is query reusability: the same PerfettoSQL queries used for individual traces can be applied to large datasets without modification.

## Core Concepts

Before writing queries, it's important to understand the foundational concepts
of how Perfetto structures trace data.

### Events

In the most general sense, a trace is simply a collection of timestamped
"events". Events can have associated metadata and context which allows them to
be interpreted and analyzed. Timestamps are in nanoseconds; the values
themselves depend on the
[clock](https://cs.android.com/android/platform/superproject/main/+/main:external/perfetto/protos/perfetto/config/trace_config.proto;l=114;drc=c74c8cf69e20d7b3261fb8c5ab4d057e8badce3e)
selected in TraceConfig.

Events form the foundation of trace processor and are one of two types: slices
and counters.

#### Slices

![Examples of slices](/docs/images/slices.png)

A slice refers to an interval of time with some data describing what was
happening in that interval. Some example of slices include:

- Scheduling slices for each CPU
- Atrace slices on Android
- Userspace slices from Chrome

#### Counters

![Examples of counters](/docs/images/counters.png)

A counter is a continuous value which varies over time. Some examples of
counters include:

- CPU frequency for each CPU core
- RSS memory events - both from the kernel and polled from /proc/stats
- atrace counter events from Android
- Chrome counter events

### Tracks

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

### Querying traces in the Perfetto UI

Now that you understand the core concepts, you can start writing queries.

Perfetto provides an SQL free form multi line text input UI directly within the UI for executing free-form queries. To access it:

1. Open a trace in the [Perfetto UI](https://ui.perfetto.dev/).

2. Click the **Query (SQL)** tab in the navigation bar (see image below).

![Query (SQL) Tab](/docs/images/perfettosql_query_tab.png)

Upon selecting this tab, the querying UI will show up and you will be able to free-form write your PerfettoSQL queries, it will let you write queries, show query results and query history as shown in the image below.

![Query UI](/docs/images/perfetto-sql-cli-description.png)

3. Enter your query in the Query UI area and press Ctrl + Enter (or Cmd + Enter) to execute.

Once executed query results will be shown within the same window.

This method of querying is useful when you have some degree of knowledge about how and what to query.

In order to find out how to write queries refer to the [Syntax guide](perfetto-sql-syntax.md), then in order to find available tables, modules, functions, etc. refer to the [Standard Library](stdlib-docs.autogen).

A lot of times, it will be useful to transform query results into tracks to perform complex analyses within the UI, we encourage readers to take a look at [Debug Tracks](debug-tracks.md) for more information on how to achieve this.

### Example: Executing a basic query

The simplest way to explore a trace is to select from the raw tables. For
example, to see the first 10 slices in a trace, you can run:

```sql
SELECT ts, dur, name FROM slice LIMIT 10;
```

Which you can write and execute by clicking on **Run Query** within the PerfettoSQL  querying UI, below is an example from a trace.

![Basic Query](/docs/images/perfetto-sql-basic-query.png)


### Getting More Context with JOINs

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
JOIN process_counter_track ON process_counter_track.id = counter.track_id
WHERE process_counter_track.name = 'mem.swap' AND value > 1000
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

For example, to get a list of all the threads which emitted a `measure` slice:

```sql
SELECT thread.name AS thread_name
FROM slice
JOIN thread_track ON slice.track_id = thread_track.id
JOIN thread USING(utid)
WHERE slice.name = 'measure'
GROUP BY thread_name
```

## Simplifying Queries with the Standard Library

While it is always possible to write queries from scratch by joining the raw
tables, PerfettoSQL provides a rich **[Standard Library](stdlib-docs.autogen)**
of pre-built modules to simplify common analysis tasks.

To use a module from the standard library, you need to import it using the
`INCLUDE PERFETTO MODULE` statement. For example, instead of doing direct
joins with threads and processes, you can use the `slices.with_context` module:

```sql
INCLUDE PERFETTO MODULE slices.with_context;

SELECT thread_name, process_name, name, ts, dur
FROM thread_or_process_slice;
```

Once imported, you can use the tables and functions provided by the module in
your queries. For more information on the available modules, see the
[Standard Library documentation](stdlib-docs.autogen).

For more details on the `INCLUDE PERFETTO MODULE` statement and other PerfettoSQL
features, see the [PerfettoSQL Syntax](perfetto-sql-syntax.md) documentation.

## Advanced Querying

For users who need to go beyond the standard library or build their own
abstractions, PerfettoSQL provides several advanced features.

### Helper functions

Helper functions are functions built into C++ which reduce the amount of
boilerplate which needs to be written in SQL.

#### Extract args

`EXTRACT_ARG` is a helper function which retrieves a property of an event (e.g.
slice or counter) from the `args` table.

It takes an `arg_set_id` and `key` as input and returns the value looked up in
the `args` table.

For example, to retrieve the `prev_comm` field for `sched_switch` events in the
`ftrace_event` table.

```sql
SELECT EXTRACT_ARG(arg_set_id, 'prev_comm')
FROM ftrace_event
WHERE name = 'sched_switch'
```

Behind the scenes, the above query would desugar to the following:

```sql
SELECT
  (
    SELECT string_value
    FROM args
    WHERE key = 'prev_comm' AND args.arg_set_id = raw.arg_set_id
  )
FROM ftrace_event
WHERE name = 'sched_switch'
```

### Operator tables

SQL queries are usually sufficient to retrieve data from trace processor.
Sometimes though, certain constructs can be difficult to express pure SQL.

In these situations, trace processor has special "operator tables" which solve a
particular problem in C++ but expose an SQL interface for queries to take
advantage of.

#### Span join

Span join is a custom operator table which computes the intersection of spans of
time from two tables or views. A span in this concept is a row in a table/view
which contains a "ts" (timestamp) and "dur" (duration) columns.

A column (called the _partition_) can optionally be specified which divides the
rows from each table into partitions before computing the intersection.

![Span join block diagram](/docs/images/span-join.png)

```sql
-- Get all the scheduling slices
CREATE VIEW sp_sched AS
SELECT ts, dur, cpu, utid
FROM sched;

-- Get all the cpu frequency slices
CREATE VIEW sp_frequency AS
SELECT
  ts,
  lead(ts) OVER (PARTITION BY track_id ORDER BY ts) - ts as dur,
  cpu,
  value as freq
FROM counter
JOIN cpu_counter_track ON counter.track_id = cpu_counter_track.id
WHERE cpu_counter_track.name = 'cpufreq';

-- Create the span joined table which combines cpu frequency with
-- scheduling slices.
CREATE VIRTUAL TABLE sched_with_frequency
USING SPAN_JOIN(sp_sched PARTITIONED cpu, sp_frequency PARTITIONED cpu);

-- This span joined table can be queried as normal and has the columns from both
-- tables.
SELECT ts, dur, cpu, utid, freq
FROM sched_with_frequency;
```

NOTE: A partition can be specified on neither, either or both tables. If
specified on both, the same column name has to be specified on each table.

WARNING: An important restriction on span joined tables is that spans from the
same table in the same partition _cannot_ overlap. For performance reasons, span
join does not attempt to detect and error out in this situation; instead,
incorrect rows will silently be produced.

WARNING: Partitions mush be integers. Importantly, string partitions are _not_
supported; note that strings _can_ be converted to integers by applying the
`HASH` function to the string column.

Left and outer span joins are also supported; both function analogously to the
left and outer joins from SQL.

```sql
-- Left table partitioned + right table unpartitioned.
CREATE VIRTUAL TABLE left_join
USING SPAN_LEFT_JOIN(table_a PARTITIONED a, table_b);

-- Both tables unpartitioned.
CREATE VIRTUAL TABLE outer_join
USING SPAN_OUTER_JOIN(table_x, table_y);
```

NOTE: there is a subtlety if the partitioned table is empty and is either a)
part of an outer join b) on the right side of a left join. In this case, _no_
slices will be emitted even if the other table is non-empty. This approach was
decided as being the most natural after considering how span joins are used in
practice.

#### Ancestor slice

ancestor_slice is a custom operator table that takes a
[slice table's id column](/docs/analysis/sql-tables.autogen#slice) and computes
all slices on the same track that are direct parents above that id (i.e. given a
slice id it will return as rows all slices that can be found by following the
parent_id column to the top slice (depth = 0)).

The returned format is the same as the
[slice table](/docs/analysis/sql-tables.autogen#slice)

For example, the following finds the top level slice given a bunch of slices of
interest.

```sql
CREATE VIEW interesting_slices AS
SELECT id, ts, dur, track_id
FROM slice WHERE name LIKE "%interesting slice name%";

SELECT
  *
FROM
  interesting_slices LEFT JOIN
  ancestor_slice(interesting_slices.id) AS ancestor ON ancestor.depth = 0
```

#### Ancestor slice by stack

ancestor_slice_by_stack is a custom operator table that takes a
[slice table's stack_id column](/docs/analysis/sql-tables.autogen#slice) and
finds all slice ids with that stack_id, then, for each id it computes all the
ancestor slices similarly to
[ancestor_slice](/docs/analysis/trace-processor#ancestor-slice).

The returned format is the same as the
[slice table](/docs/analysis/sql-tables.autogen#slice)

For example, the following finds the top level slice of all slices with the
given name.

```sql
CREATE VIEW interesting_stack_ids AS
SELECT stack_id
FROM slice WHERE name LIKE "%interesting slice name%";

SELECT
  *
FROM
  interesting_stack_ids LEFT JOIN
  ancestor_slice_by_stack(interesting_stack_ids.stack_id) AS ancestor
  ON ancestor.depth = 0
```

#### Descendant slice

descendant_slice is a custom operator table that takes a
[slice table's id column](/docs/analysis/sql-tables.autogen#slice) and computes
all slices on the same track that are nested under that id (i.e. all slices that
are on the same track at the same time frame with a depth greater than the given
slice's depth.

The returned format is the same as the
[slice table](/docs/analysis/sql-tables.autogen#slice)

For example, the following finds the number of slices under each slice of
interest.

```sql
CREATE VIEW interesting_slices AS
SELECT id, ts, dur, track_id
FROM slice WHERE name LIKE "%interesting slice name%";

SELECT
  *
  (
    SELECT
      COUNT(*) AS total_descendants
    FROM descendant_slice(interesting_slice.id)
  )
FROM interesting_slices
```

#### Descendant slice by stack

descendant_slice_by_stack is a custom operator table that takes a
[slice table's stack_id column](/docs/analysis/sql-tables.autogen#slice) and
finds all slice ids with that stack_id, then, for each id it computes all the
descendant slices similarly to
[descendant_slice](/docs/analysis/trace-processor#descendant-slice).

The returned format is the same as the
[slice table](/docs/analysis/sql-tables.autogen#slice)

For example, the following finds the next level descendant of all slices with
the given name.

```sql
CREATE VIEW interesting_stacks AS
SELECT stack_id, depth
FROM slice WHERE name LIKE "%interesting slice name%";

SELECT
  *
FROM
  interesting_stacks LEFT JOIN
  descendant_slice_by_stack(interesting_stacks.stack_id) AS descendant
  ON descendant.depth = interesting_stacks.depth + 1
```

#### Connected/Following/Preceding flows

DIRECTLY_CONNECTED_FLOW, FOLLOWING_FLOW and PRECEDING_FLOW are custom operator
tables that take a
[slice table's id column](/docs/analysis/sql-tables.autogen#slice) and collect
all entries of [flow table](/docs/analysis/sql-tables.autogen#flow), that are
directly or indirectly connected to the given starting slice.

`DIRECTLY_CONNECTED_FLOW(start_slice_id)` - contains all entries of
[flow table](/docs/analysis/sql-tables.autogen#flow) that are present in any
chain of kind: `flow[0] -> flow[1] -> ... -> flow[n]`, where
`flow[i].slice_out = flow[i+1].slice_in` and
`flow[0].slice_out = start_slice_id OR start_slice_id = flow[n].slice_in`.

NOTE: Unlike the following/preceding flow functions, this function will not
include flows connected to ancestors or descendants while searching for flows
from a slice. It only includes the slices in the directly connected chain.

`FOLLOWING_FLOW(start_slice_id)` - contains all flows which can be reached from
a given slice via recursively following from flow's outgoing slice to its
incoming one and from a reached slice to its child. The return table contains
all entries of [flow table](/docs/analysis/sql-tables.autogen#flow) that are
present in any chain of kind: `flow[0] -> flow[1] -> ... -> flow[n]`, where
`flow[i+1].slice_out IN DESCENDANT_SLICE(flow[i].slice_in) OR flow[i+1].slice_out = flow[i].slice_in`
and
`flow[0].slice_out IN DESCENDANT_SLICE(start_slice_id) OR flow[0].slice_out = start_slice_id`.

`PRECEDING_FLOW(start_slice_id)` - contains all flows which can be reached from
a given slice via recursively following from flow's incoming slice to its
outgoing one and from a reached slice to its parent. The return table contains
all entries of [flow table](/docs/analysis/sql-tables.autogen#flow) that are
present in any chain of kind: `flow[n] -> flow[n-1] -> ... -> flow[0]`, where
`flow[i].slice_in IN ANCESTOR_SLICE(flow[i+1].slice_out) OR flow[i].slice_in = flow[i+1].slice_out`
and
`flow[0].slice_in IN ANCESTOR_SLICE(start_slice_id) OR flow[0].slice_in = start_slice_id`.

```sql
--number of following flows for each slice
SELECT (SELECT COUNT(*) FROM FOLLOWING_FLOW(slice_id)) as following FROM slice;
```

## Next Steps

Now that you have a foundational understanding of PerfettoSQL, you can explore
the following topics to deepen your knowledge:

- **[PerfettoSQL Syntax](perfetto-sql-syntax.md)**: Learn about the SQL syntax
  supported by Perfetto, including special features for creating functions,
  tables, and views.
- **[Standard Library](stdlib-docs.autogen)**: Explore the rich set of modules
  available in the standard library for analyzing common scenarios like CPU
  usage, memory, and power.
- **[Trace Processor (C++)](trace-processor.md)**: Learn how to use the
  interactive shell and the underlying C++ library.
- **[Trace Processor (Python)](trace-processor-python.md)**: Leverage the Python
  API to combine trace analysis with the rich data science and visualization
  ecosystem.
