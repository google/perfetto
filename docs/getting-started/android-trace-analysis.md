# Cookbook: Analysing Android Traces

This page will take you through some real world examples on how you can analyse
issues with SQL and more advanced features of the Perfetto UI.

## Finding slices

Demonstrates:

- Querying slices.
- GLOB and similar operators.
- Common aggregators: COUNT, SUM, PERCENTILE.
- JOIN tables.

Slices seen in Perfetto’s timeline UI can also be queried with PerfettoSQL.
Press “Query (SQL)” in the sidebar and enter this query:

```sql
SELECT *
FROM slice
WHERE name GLOB '*interesting_slice*'
LIMIT 10;
```

Navigating back to the timeline (press “Show timeline”) will show the results
table in the bottom bar. You can click slice IDs to jump to the slice in the
timeline.

PerfettoSQL supports multiple
[pattern matching operators](https://sqlite.org/lang_expr.html#like) like
`GLOB`, `LIKE`, and `REGEXP`. You can also use different aggregators to generate
statistics on your selection.

```sql
SELECT
  name,
  COUNT(dur) AS count_slice,
  -- Convert nanoseconds to milliseconds
  AVG(dur) / 1000000 AS avg_dur_ms,
  CAST(MAX(dur) AS DOUBLE) / 1000000 AS max_dur_ms,
  CAST(MIN(dur) AS DOUBLE) / 1000000 AS min_dur_ms,
  PERCENTILE(dur,50) / 1000000 AS P50_dur_ms,
  PERCENTILE(dur,90) / 1000000 AS P90_dur_ms,
  PERCENTILE(dur,99) / 1000000 AS P99_dur_ms
FROM slice
WHERE name REGEXP '.*interesting_slice.*'
GROUP BY name
ORDER BY count_slice DESC
LIMIT 10;
```

You can join information across multiple tables to surface more information in
query results or to narrow down your search.

```sql
SELECT
  s.id AS id,
  s.ts AS ts,
  s.track_id AS track_id,
  s.slice_id AS slice_id,
  s.dur AS dur,
  s.name AS slice,
  p.name AS process,
  t.name AS thread
FROM slice s
JOIN thread_track tt ON s.track_id = tt.id
JOIN thread t on tt.utid = t.utid
JOIN process p on t.upid = p.upid
WHERE s.name LIKE '%interesting_slice%'
-- Only look for slices in your app's process
AND p.name = 'com.example.myapp'
-- Only look for slices on your app's main thread
AND t.is_main_thread
ORDER BY dur DESC;
```

After running the query in the SQL view, click “Show timeline” in the sidebar
and the query results will appear in the bottom bar. Queries that include the
slice columns id, ts, dur, track_id, and slice_id can link to slices in the
Timeline view for easy navigation. Click the value under id and the timeline
will jump straight to that slice.

![](/docs/images/analysis-cookbook-unint-sleep.png)

## Find top causes for uninterruptible sleep

Demonstrates:

- Joining tables on PerfettoSQL unique IDs.
- SQL aggregation.

Thread tracks show a
[thread’s state](/docs/data-sources/cpu-scheduling.md#decoding-code-end_state-code-),
such as if it’s running, is runnable but not running, sleeping, etc. A common
source of performance problems is when application threads enter
“uninterruptible sleep”, i.e. call a kernel function that blocks on an
uninterruptible condition.

To troubleshoot uninterruptible sleep you will need the following snippet in
your Perfetto configuration when recording traces:

```
data_sources: {
    config {
        name: "linux.ftrace"
        target_buffer: 0
        ftrace_config {
            ftrace_events: "sched/sched_blocked_reason"
        }
    }
}
```

With this configured, when clicking on a thread state slice in uninterruptible
sleep you will see in the bottom bar a field named “blocked_function”. Instead
of clicking on individual slices, you can run a query to summarize the data:

```sql
SELECT blocked_function, COUNT(thread_state.id), SUM(dur)
FROM thread_state
JOIN thread USING (utid)
JOIN process USING (upid)
WHERE process.name = "com.google.android.youtube"
GROUP BY blocked_function
ORDER BY SUM(dur) DESC;
```

## Find app startups blocked on monitor contention

Demonstrates:

- `PARTITION` to subdivide a table of slices by the value of a column.
- `SPAN_JOIN` to create spans from the intersection of data from two tables.

In Android Java and Kotlin, “monitor contention” is when a thread attempts to
enter a `synchronized` section or call a `synchronized` method but another
thread has already acquired the lock (aka monitor) used for synchronization. The
example below demonstrates finding monitor contention slices that happened while
an app was starting that blocked the app’s main thread, thus delaying the app’s
startup.

```sql
INCLUDE PERFETTO MODULE android.monitor_contention;
INCLUDE PERFETTO MODULE android.startup.startups;

-- Join package and process information for startups
DROP VIEW IF EXISTS startups;
CREATE VIEW startups AS
SELECT startup_id, ts, dur, upid
FROM android_startups
JOIN android_startup_processes USING(startup_id);

-- Intersect monitor contention with startups in the same process.
-- This ensures that we only look at monitor contention in apps
-- that were starting up, and only during their startup phase.
DROP TABLE IF EXISTS monitor_contention_during_startup;
CREATE VIRTUAL TABLE monitor_contention_during_startup
USING SPAN_JOIN(android_monitor_contention PARTITIONED upid, startups PARTITIONED upid);

SELECT
  process_name,
  -- Convert duration from nanoseconds to milliseconds
  SUM(dur) / 1000000 AS sum_dur_ms,
  COUNT(*) AS count_contention
FROM monitor_contention_during_startup
WHERE is_blocked_thread_main
GROUP BY process_name
ORDER BY SUM(dur) DESC;
```

## Process scheduling groups as debug tracks

Demonstrates:

- Projecting a string column into one or more columns using substring
  substitution.
- Creating a custom Debug Tracks in the Perfetto Timeline view.
- Views.
- `PARTITION` to subdivide slices by another column value.
- `LEAD` to find the next event in a partition by timestamp order.

Android’s `system_server` will classify different app processes as belonging to
different scheduling groups. This is used to direct more system resources
towards more user-facing or otherwise latency-sensitive apps (such as “top” or
“foreground” apps) and away from other processes that are doing
latency-insensitive tasks in the background.

`system_server` will emit slices in the format of:

```
setProcessGroup <process> to <group>
```

With PerfettoSQL you can turn these strings into structured data:

```sql
INCLUDE PERFETTO MODULE slices.with_context;
SELECT
  ts,
  dur,
  SUBSTR(name, INSTR(name, ' ') + 1, INSTR(name, ' to ') - INSTR(name, ' ') - 1) as process_name,
  SUBSTR(name, INSTR(name, ' to ') + 4) AS group_id
FROM thread_slice
WHERE process_name = 'system_server'
AND thread_name = 'OomAdjuster'
AND name LIKE 'setProcessGroup %';
```

Using debug tracks you can add this information to the timeline. Press “Show
timeline”. In the bottom bar, press “Show debug track” and configure:

- Track type: counter
- ts: `ts`
- value: `group_id`
- pivot: `process_name`

![](/docs/images/debug-track-setprocessgroup-simple.png)

Press “Show” and you’ll see debug tracks generated from the results:
![](/docs/images/debug-track-setprocessgroup-simple-result.png)

The integer values for groups are enumerated in `SchedPolicy` in
[`system/core/libprocessgroup/include/processgroup/sched_policy.h`](https://android.googlesource.com/platform/system/core/+/main/libprocessgroup/include/processgroup/sched_policy.h).
You can project the numeric values into string names:

```sql
INCLUDE PERFETTO MODULE slices.with_context;
SELECT
  ts,
  dur,
  SUBSTR(name, INSTR(name, ' ') + 1, INSTR(name, ' to ') - INSTR(name, ' ') - 1) as process_name,
  -- Resolve SchedPolicy
  CASE SUBSTR(name, INSTR(name, ' to ') + 4)
    WHEN '-1' THEN 'SP_DEFAULT'
    WHEN '0' THEN 'SP_BACKGROUND'
    WHEN '1' THEN 'SP_FOREGROUND'
    WHEN '2' THEN 'SP_SYSTEM'
    WHEN '3' THEN 'SP_AUDIO_APP'
    WHEN '4' THEN 'SP_AUDIO_SYS'
    WHEN '5' THEN 'SP_TOP_APP'
    WHEN '6' THEN 'SP_RT_APP'
    WHEN '7' THEN 'SP_RESTRICTED'
    WHEN '8' THEN 'SP_FOREGROUND_WINDOW'
    ELSE SUBSTR(name, INSTR(name, ' to ') + 4)
  END AS group_name
FROM thread_slice
WHERE process_name = 'system_server'
AND thread_name = 'OomAdjuster'
AND name LIKE 'setProcessGroup %';
```

Configure a debug track: ![](/docs/images/debug-track-setprocessgroup-dur.png)

You’ll see debug tracks with human-readable names:
![](/docs/images/debug-track-setprocessgroup-dur-short-result.png)

You’ll notice a problem \- the durations of these tracks are short. The
durations indicate just the time it took `system_server` to change the process
group for these processes. You might want to see the duration that the process
was in that group, i.e. for the durations to extend until the next group update
or until the end of the trace. You can do this with `LEAD` by finding the next
slice, partitioned over `process_name`.

```sql
INCLUDE PERFETTO MODULE slices.with_context;

-- Create a view so we can refer to next_ts in the following query
DROP VIEW IF EXISTS setProcessGroup;
CREATE VIEW setProcessGroup AS
SELECT
  ts,
  dur,
  SUBSTR(name, INSTR(name, ' ') + 1, INSTR(name, ' to ') - INSTR(name, ' ') - 1) as process_name,
  LEAD(ts) OVER (PARTITION BY SUBSTR(name, INSTR(name, ' ') + 1, INSTR(name, ' to ') - INSTR(name, ' ') - 1) ORDER BY ts) AS next_ts,
  -- Resolve SchedPolicy
  CASE SUBSTR(name, INSTR(name, ' to ') + 4)
    WHEN '-1' THEN 'SP_DEFAULT'
    WHEN '0' THEN 'SP_BACKGROUND'
    WHEN '1' THEN 'SP_FOREGROUND'
    WHEN '2' THEN 'SP_SYSTEM'
    WHEN '3' THEN 'SP_AUDIO_APP'
    WHEN '4' THEN 'SP_AUDIO_SYS'
    WHEN '5' THEN 'SP_TOP_APP'
    WHEN '6' THEN 'SP_RT_APP'
    WHEN '7' THEN 'SP_RESTRICTED'
    WHEN '8' THEN 'SP_FOREGROUND_WINDOW'
    ELSE SUBSTR(name, INSTR(name, ' to ') + 4)
  END AS group_name
FROM thread_slice
WHERE process_name = 'system_server'
AND thread_name = 'OomAdjuster'
AND name LIKE 'setProcessGroup %';

SELECT
  ts,
  dur,
  process_name,
  group_name,
  next_ts,
  IIF(
    next_ts IS NOT NULL,
    -- Duration is from ts to next_ts
    next_ts - ts,
    -- Duration is from ts to the last timestamp seen in this trace
    (SELECT MAX(ts + dur) FROM slice) - ts
  ) AS dur_until_next
FROM setProcessGroup;
```

Configure your debug tracks again:
![](/docs/images/debug-track-setprocessgroup-dur-next.png)

The debug tracks should now look like this:
![](/docs/images/debug-track-setprocessgroup-dur-next-result.png)

Here is an example from a busier trace, where you can see the same process being
assigned to different groups:
![](/docs/images/debug-track-setprocessgroup-final-result.png)
