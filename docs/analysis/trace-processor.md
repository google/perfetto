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
example, to see all the slices in a trace, you can run the following query:

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

## Concepts

The trace processor has some foundational terminology and concepts which are
used in the rest of documentation.

### Events

In the most general sense, a trace is simply a collection of timestamped
"events". Events can have associated metadata and context which allows them to
be interpreted and analyzed. Timestamps are in nanoseconds; the values
themselves depend on the
[clock][https://cs.android.com/android/platform/superproject/main/+/main:external/perfetto/protos/perfetto/config/trace_config.proto;l=114;drc=c74c8cf69e20d7b3261fb8c5ab4d057e8badce3e]
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

For example, to get a list of all the threads which emitted a `measure` slice

```sql
SELECT thread.name AS thread_name
FROM slice
JOIN thread_track ON slice.track_id = thread_track.id
JOIN thread USING(utid)
WHERE slice.name = 'measure'
GROUP BY thread_name
```

## Helper functions

Helper functions are functions built into C++ which reduce the amount of
boilerplate which needs to be written in SQL.

### Extract args

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

NOTE: while convinient, `EXTRACT_ARG` can inefficient compared to a `JOIN` when
working with very large tables; a function call is required for every row which
will be slower than the batch filters/sorts used by `JOIN`.

## Operator tables

SQL queries are usually sufficient to retrieve data from trace processor.
Sometimes though, certain constructs can be difficult to express pure SQL.

In these situations, trace processor has special "operator tables" which solve a
particular problem in C++ but expose an SQL interface for queries to take
advantage of.

### Span join

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

### Ancestor slice

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

### Ancestor slice by stack

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

### Descendant slice

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

### Descendant slice by stack

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

### Connected/Following/Preceding flows

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

## Embedding

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
