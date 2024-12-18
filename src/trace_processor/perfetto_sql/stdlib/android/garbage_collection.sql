--
-- Copyright 2024 The Android Open Source Project
--
-- Licensed under the Apache License, Version 2.0 (the "License");
-- you may not use this file except in compliance with the License.
-- You may obtain a copy of the License at
--
--     https://www.apache.org/licenses/LICENSE-2.0
--
-- Unless required by applicable law or agreed to in writing, software
-- distributed under the License is distributed on an "AS IS" BASIS,
-- WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
-- See the License for the specific language governing permissions and
-- limitations under the License.
--

INCLUDE PERFETTO MODULE android.startup.startups;
INCLUDE PERFETTO MODULE intervals.intersect;
INCLUDE PERFETTO MODULE slices.with_context;

-- Collect all GC slices. There's typically one enclosing slice but sometimes the
-- CompactionPhase is outside the nesting and we need to include that.
CREATE PERFETTO VIEW _gc_slice
AS
WITH concurrent AS (
SELECT
  id AS gc_id,
  name AS gc_name,
  LEAD(name) OVER (PARTITION BY track_id ORDER BY ts) AS compact_name,
  LEAD(dur) OVER (PARTITION BY track_id ORDER BY ts) AS compact_dur,
  ts AS gc_ts,
  IIF(dur = -1, trace_end() - slice.ts, slice.dur) AS gc_dur,
  ts,
  dur,
  tid,
  utid,
  pid,
  upid,
  thread_name,
  process_name
FROM thread_slice slice
WHERE depth = 0
) SELECT
  gc_id,
  gc_name,
  ts AS gc_ts,
  ts,
  gc_dur + IIF(
    compact_name = 'CompactionPhase' OR compact_name = 'Background concurrent copying GC',
    compact_dur,
    0) AS gc_dur,
  gc_dur + IIF(
    compact_name = 'CompactionPhase' OR compact_name = 'Background concurrent copying GC',
    compact_dur,
    0) AS dur,
  utid,
  tid,
  upid,
  pid,
  thread_name,
  process_name
FROM concurrent WHERE gc_name GLOB '*concurrent*GC';

-- Extract the heap counter into <ts, dur, upid>
CREATE PERFETTO VIEW _gc_heap_counter
AS
SELECT
  c.ts,
  IFNULL(lead(c.ts) OVER (PARTITION BY track_id ORDER BY c.ts), trace_end()) - ts
    AS dur,
  process.upid,
  cast_int!(c.value ) AS value
FROM counter c
JOIN process_counter_track t
  ON c.track_id = t.id
INNER JOIN process
  USING (upid)
WHERE
  t.name = 'Heap size (KB)';

-- Find the last heap counter after the GC slice dur. This is the best effort to find the
-- final heap size after GC. The algorithm is like so:
-- 1. Merge end_ts of the GC events with the start_ts of the heap counters.
-- 2. Find the heap counter value right after each GC event.
CREATE PERFETTO VIEW _gc_slice_with_final_heap
AS
WITH
  slice_and_heap AS (
    SELECT upid, gc_id, gc_ts + gc_dur AS ts, NULL AS value FROM _gc_slice
    UNION ALL
    SELECT upid, NULL AS gc_id, ts, value FROM _gc_heap_counter
  ),
  next_heap AS (
    SELECT *, lead(value) OVER (PARTITION BY upid ORDER BY ts) AS last_value FROM slice_and_heap
  ),
  slice_with_last_heap AS (
    SELECT * FROM next_heap WHERE gc_id IS NOT NULL
  )
  SELECT _gc_slice.*, last_value FROM _gc_slice LEFT JOIN slice_with_last_heap USING (gc_id);

-- Span join with all the other heap counters to find the overall min and max heap size.
CREATE VIRTUAL TABLE _gc_slice_heap_sp
USING
  SPAN_JOIN(_gc_slice_with_final_heap PARTITIONED upid, _gc_heap_counter PARTITIONED upid);

-- Aggregate the min and max heap across the GC event, taking into account the last heap size
-- derived earlier.
CREATE PERFETTO TABLE _gc_slice_heap
AS
SELECT
  gc_ts as ts,
  gc_dur as dur,
  upid, gc_id, gc_name, gc_ts, gc_dur, utid, tid, pid,
  thread_name, process_name, last_value, value,
  CASE
    WHEN gc_name GLOB '*young*' THEN 'young'
    WHEN gc_name GLOB '*NativeAlloc*' THEN 'native_alloc'
    WHEN gc_name GLOB '*Alloc*' THEN 'alloc'
    WHEN gc_name GLOB '*CollectorTransition*' THEN 'collector_transition'
    WHEN gc_name GLOB '*Explicit*' THEN 'explicit'
    ELSE 'full'
    END AS gc_type,
  IIF(gc_name GLOB '*mark compact*', 1, 0) AS is_mark_compact,
  MAX(MAX(value, last_value))/1e3 AS max_heap_mb,
  MIN(MIN(value, last_value))/1e3 AS min_heap_mb
FROM _gc_slice_heap_sp
GROUP BY gc_id;

-- Span join GC events with thread states to breakdown the time spent.
CREATE VIRTUAL TABLE _gc_slice_heap_thread_state_sp
USING
  SPAN_LEFT_JOIN(_gc_slice_heap PARTITIONED utid, thread_state PARTITIONED utid);

-- All Garbage collection events with a breakdown of the time spent and heap reclaimed.
CREATE PERFETTO TABLE android_garbage_collection_events (
  -- Tid of thread running garbage collection.
  tid LONG,
  -- Pid of process running garbage collection.
  pid LONG,
  -- Utid of thread running garbage collection.
  utid JOINID(thread.id),
  -- Upid of process running garbage collection.
  upid JOINID(process.id),
  -- Name of thread running garbage collection.
  thread_name STRING,
  -- Name of process running garbage collection.
  process_name STRING,
  -- Type of garbage collection.
  gc_type STRING,
  -- Whether gargage collection is mark compact or copying.
  is_mark_compact LONG,
  -- MB reclaimed after garbage collection.
  reclaimed_mb DOUBLE,
  -- Minimum heap size in MB during garbage collection.
  min_heap_mb DOUBLE,
  -- Maximum heap size in MB during garbage collection.
  max_heap_mb DOUBLE,
  -- Garbage collection id.
  gc_id LONG,
  -- Garbage collection timestamp.
  gc_ts TIMESTAMP,
  -- Garbage collection wall duration.
  gc_dur DURATION,
  -- Garbage collection duration spent executing on CPU.
  gc_running_dur DURATION,
  -- Garbage collection duration spent waiting for CPU.
  gc_runnable_dur DURATION,
  -- Garbage collection duration spent waiting in the Linux kernel on IO.
  gc_unint_io_dur DURATION,
  -- Garbage collection duration spent waiting in the Linux kernel without IO.
  gc_unint_non_io_dur DURATION,
    -- Garbage collection duration spent waiting in interruptible sleep.
  gc_int_dur LONG
  )
AS
WITH
  agg_events AS (
    SELECT
      tid,
      pid,
      utid,
      upid,
      thread_name,
      process_name,
      gc_type,
      is_mark_compact,
      gc_id,
      gc_ts,
      gc_dur,
      SUM(dur) AS dur,
      max_heap_mb - min_heap_mb AS reclaimed_mb,
      min_heap_mb,
      max_heap_mb,
      state,
      io_wait
    FROM _gc_slice_heap_thread_state_sp
    GROUP BY gc_id, state, io_wait
  )
SELECT
  tid,
  pid,
  utid,
  upid,
  thread_name,
  process_name,
  gc_type,
  is_mark_compact,
  reclaimed_mb,
  min_heap_mb,
  max_heap_mb,
  gc_id,
  gc_ts,
  gc_dur,
  SUM(IIF(state = 'Running', dur, 0)) AS gc_running_dur,
  SUM(IIF(state = 'R' OR state = 'R+', dur, 0)) AS gc_runnable_dur,
  SUM(IIF(state = 'D' AND io_wait = 1, dur, 0)) AS gc_unint_io_dur,
  SUM(IIF(state = 'D' AND io_wait != 1, dur, 0)) AS gc_unint_non_io_dur,
  SUM(IIF(state = 'S', dur, 0)) AS gc_int_dur
FROM agg_events
GROUP BY gc_id;

-- A window of the trace to use for GC stats.
-- We can't reliably use trace_dur(), because often it spans far outside the
-- range of relevant data. Instead pick the window based on when we have
-- 'Heap size (KB)' data available.
CREATE PERFETTO TABLE _gc_stats_window
AS
SELECT
  MIN(ts) AS gc_stats_window_start,
  MAX(ts) AS gc_stats_window_end,
  MAX(ts) - MIN(ts) AS gc_stats_window_dur
FROM counter AS c
LEFT JOIN process_counter_track AS t on c.track_id = t.id
WHERE t.name='Heap size (KB)';

-- Count heap allocations by summing positive changes to the 'Heap size (KB)'
-- counter.
CREATE PERFETTO TABLE _gc_heap_allocated
AS
SELECT
  ts,
  ts - LAG(ts) OVER (PARTITION BY upid ORDER BY ts) as dur,
  value,
  CASE WHEN LAG(c.value) OVER (PARTITION BY upid ORDER BY ts) < c.value THEN c.value - LAG(c.value) OVER (PARTITION BY upid ORDER BY ts) ELSE 0 END as allocated
FROM counter AS c
LEFT JOIN process_counter_track AS t on c.track_id = t.id
WHERE t.name='Heap size (KB)';

-- Intersection of startup events and gcs, for understanding what GCs are
-- happining during app startup.
CREATE PERFETTO TABLE _gc_during_android_startup AS
WITH
  startups_for_intersect AS (
    SELECT
      ts,
      dur,
      startup_id as id
    FROM android_startups
    WHERE dur > 0   -- b/384732321
  ),
  gcs_for_intersect AS (
    SELECT
      gc_ts as ts,
      gc_dur as dur,
      gc_id as id
    FROM android_garbage_collection_events
  )
SELECT
  ts,
  dur,
  id_0 as startup_id,
  id_1 as gc_id
FROM _interval_intersect!((startups_for_intersect, gcs_for_intersect), ());

-- Estimate heap utilization across the trace.
-- We weight the utilization by gc_period, which is meant to represent the time
-- since the previous GC. We approximate gc_period as time to the start of the
-- process or metrics utilization in case there is no previous GC for the
-- process.
CREATE PERFETTO TABLE _gc_heap_utilization AS
WITH
  -- The first GC for a process doesn't have a previous GC to calculate
  -- gc_period from. Create pretend GCs at the start of each process to use
  -- for computing gc_period for all the real GCs.
  before_first_gcs AS (
    SELECT
      upid,
      CASE
        WHEN start_ts IS NULL THEN gc_stats_window_start
        ELSE MAX(start_ts, gc_stats_window_start)
      END as gc_ts,
      0 as gc_dur,
      0 as min_heap_mb,
      0 as max_heap_mb
    FROM process
    JOIN _gc_stats_window
  ),
  combined_gcs AS (
    SELECT
      upid, gc_ts, gc_dur, min_heap_mb, max_heap_mb
    FROM android_garbage_collection_events
    UNION SELECT * FROM before_first_gcs
  ),
  gc_periods AS (
    SELECT
      gc_ts + gc_dur - LAG(gc_ts + gc_dur) OVER (PARTITION BY upid ORDER BY gc_ts) AS gc_period,
      min_heap_mb,
      max_heap_mb
    FROM combined_gcs
  )
SELECT
  SUM(gc_period * min_heap_mb)/1e9 AS heap_live_mbs,
  SUM(gc_period * max_heap_mb)/1e9 AS heap_total_mbs
FROM gc_periods where min_heap_mb IS NOT NULL;

-- Summary stats about how garbage collection is behaving across the device,
-- including causes, costs and other information relevant for tuning the
-- garbage collector.
CREATE PERFETTO TABLE _android_garbage_collection_stats (
  -- The start of the window of time that the stats cover in the trace.
  ts TIMESTAMP,
  -- The duration of the window of time that the stats cover in the trace.
  dur DURATION,
  -- Megabyte-seconds of heap size across the device, used in the calculation
  -- of heap_size_mb.
  heap_size_mbs DOUBLE,
  -- Combined size of heaps across processes on the device on average, in MB.
  heap_size_mb DOUBLE,
  -- Total number of bytes allocated over the course of the trace.
  heap_allocated_mb DOUBLE,
  -- Combined rate of heap allocations in MB per second. This gives a sense of
  -- how much allocation activity is going on during the trace.
  heap_allocation_rate DOUBLE,
  -- Megabyte-seconds of live heap for processes that had GC events.
  heap_live_mbs DOUBLE,
  -- Megabyte-seconds of total heap for processes that had GC events.
  heap_total_mbs DOUBLE,
  -- Overall heap utilization. This gives a sense of how aggressive GC is
  -- during this trace.
  heap_utilization DOUBLE,
  -- CPU time spent running GC. Used in the calculation of gc_running_rate.
  gc_running_dur DURATION,
  -- CPU time spent doing GC, as a fraction of the duration of the trace.
  -- This gives a sense of the battery cost of GC.
  gc_running_rate DOUBLE,
  -- A measure of how efficient GC is with respect to cpu, independent of how
  -- aggressively GC is tuned. Larger values indicate more efficient GC, so
  -- larger is better.
  gc_running_efficiency DOUBLE,
  -- Time GC is running during app startup. Used in the calculation of
  -- gc_during_android_startup_rate.
  gc_during_android_startup_dur DURATION,
  -- Total startup time in the trace, used to normalize
  -- the gc_during_android_startup_rate.
  total_android_startup_dur DURATION,
  -- Time GC is running during app startup, as a fraction of startup time in
  -- the trace. This gives a sense of how much potential interference there
  -- is between GC and application startup.
  gc_during_android_startup_rate DOUBLE,
  -- A measure of how efficient GC is with regards to gc during application
  -- startup, independent of how aggressively GC is tuned. Larger values
  -- indicate more efficient GC, so larger is better.
  gc_during_android_startup_efficiency DOUBLE
  )
AS
WITH
  gc_running_stats AS (
    SELECT
      SUM(gc_running_dur) AS gc_running_dur
    FROM android_garbage_collection_events
  ),
  heap_size_stats AS (
    SELECT
      SUM(allocated)/1e3 AS heap_allocated_mb,
      SUM(value * dur)/(1e3 * 1e9) AS heap_size_mbs
    FROM _gc_heap_allocated
  ),
  gc_startup_stats AS (
    SELECT
      SUM(dur) AS gc_during_android_startup_dur
    FROM _gc_during_android_startup
  ),
  startup_stats AS (
    SELECT
      SUM(dur) AS total_android_startup_dur
    FROM android_startups
  ),
  pre_normalized_stats AS (
    SELECT *
    FROM _gc_stats_window, gc_running_stats, heap_size_stats,
         gc_startup_stats, startup_stats, _gc_heap_utilization
  ),
  normalized_stats AS (
    SELECT
      gc_running_dur * 1.0 / gc_stats_window_dur AS gc_running_rate,
      gc_during_android_startup_dur * 1.0 / total_android_startup_dur AS gc_during_android_startup_rate,
      heap_allocated_mb * 1e9 / gc_stats_window_dur AS heap_allocation_rate,
      heap_size_mbs * 1e9 / gc_stats_window_dur AS heap_size_mb,
      heap_live_mbs / heap_total_mbs AS heap_utilization
    FROM pre_normalized_stats
  )
SELECT
  gc_stats_window_start AS ts,
  gc_stats_window_dur AS dur,
  heap_size_mbs, heap_size_mb,
  heap_allocated_mb, heap_allocation_rate,
  heap_live_mbs, heap_total_mbs, heap_utilization,
  gc_running_dur, gc_running_rate,
  heap_allocation_rate * heap_utilization / (gc_running_rate * (1 - heap_utilization)) AS gc_running_efficiency,
  gc_during_android_startup_dur,
  total_android_startup_dur,
  gc_during_android_startup_rate,
  heap_allocation_rate * heap_utilization / (gc_during_android_startup_rate * (1 - heap_utilization)) AS gc_during_android_startup_efficiency
FROM pre_normalized_stats, normalized_stats;
