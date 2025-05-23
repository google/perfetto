# Recipes: Android Trace Analysis

This document provides bite-sized "recipes" for analyzing issues in Perfetto
traces collected from Android. It covers useful PerfettoSQL queries and UI
features to simplify debugging.

## Find top causes for uninterruptible sleep

Demonstrates:

- Joining tables on PerfettoSQL unique IDs.
- SQL aggregation.

```sql
SELECT blocked_function, COUNT(thread_state.id), SUM(dur)
FROM thread_state
JOIN thread USING (utid)
JOIN process USING (upid)
WHERE process.name = "com.google.android.youtube"
GROUP BY blocked_function
ORDER BY SUM(dur) DESC;
```

`blocked_function` requires the following configuration:

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

## Process groups as debug tracks

Demonstrates:

- Projecting a string column into one or more columns using substring
  substitution.
- Creating a custom Debug Tracks in the Perfetto Timeline view.

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

To visualize this data:

1. Go to the Perfetto UI Timeline view.
2. In the bottom bar, click on “Show debug track”.
3. Configure the track with the following parameters:
   - Track type: counter
   - ts: `ts`
   - value: `group_id`
   - pivot: `process_name`

![images/debug-track-recipe.png][image1]

## Find app startups blocked on monitor contention

Demonstrates:

- Using `SPAN_JOIN` to create spans from the intersection of data from two
  tables, partitioned on a joint column.

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
