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

-- Collect all GC slices. There's typically one enclosing slice but sometimes the
-- CompactionPhase is outside the nesting and we need to include that.
CREATE PERFETTO PIPELINE _gc_slice AS
FROM thread_slice AS slice
|> WHERE depth = 0
|> SELECT
     id AS gc_id,
     name AS gc_name,
     lead(name) OVER (PARTITION BY track_id ORDER BY ts) AS compact_name,
     lead(dur) OVER (PARTITION BY track_id ORDER BY ts) AS compact_dur,
     ts AS gc_ts,
     iif(dur = -1, trace_end() - slice.ts, slice.dur) AS gc_dur,
     ts,
     dur,
     tid,
     utid,
     pid,
     upid,
     thread_name,
     process_name
|> WHERE gc_name GLOB '*concurrent*GC'
|> SELECT
     gc_id,
     gc_name,
     gc_ts,
     ts,
     gc_dur
     + iif(
       compact_name = 'CompactionPhase'
       OR compact_name = 'Background concurrent copying GC',
       compact_dur,
       0
     ) AS gc_dur,
     gc_dur
     + iif(
       compact_name = 'CompactionPhase'
       OR compact_name = 'Background concurrent copying GC',
       compact_dur,
       0
     ) AS dur,
     utid,
     tid,
     upid,
     pid,
     thread_name,
     process_name;

-- Extract the heap counter into <ts, dur, upid>
CREATE PERFETTO PIPELINE _gc_heap_counter AS
FROM counter AS c
|> JOIN process_counter_track AS t ON c.track_id = t.id
|> JOIN process USING (upid)
|> WHERE t.name = 'Heap size (KB)'
|> SELECT
     c.ts,
     coalesce(lead(c.ts) OVER (PARTITION BY c.track_id ORDER BY c.ts), trace_end())
     - c.ts AS dur,
     process.upid,
     cast_int!(c.value) AS value;

-- Find the last heap counter after the GC slice dur. This is the best effort to find the
-- final heap size after GC. The algorithm is like so:
-- 1. Merge end_ts of the GC events with the start_ts of the heap counters.
-- 2. Find the heap counter value right after each GC event.
CREATE PERFETTO PIPELINE _gc_slice_with_final_heap AS
SUBPIPELINE slice_with_last_heap AS (
  FROM _gc_slice
  |> SELECT upid, gc_id, gc_ts + gc_dur AS ts, NULL AS value
  |> UNION ALL (FROM _gc_heap_counter |> SELECT upid, NULL AS gc_id, ts, value)
  |> EXTEND last_value = lead(value) OVER (PARTITION BY upid ORDER BY ts)
  |> WHERE gc_id IS NOT NULL
)
FROM _gc_slice
|> LEFT JOIN slice_with_last_heap USING (gc_id)
|> SELECT _gc_slice.*, last_value;

-- Aggregate the min and max heap across the GC event, taking into account the
-- last heap size derived earlier.
CREATE PERFETTO PIPELINE _gc_slice_heap MATERIALIZED AS
INTERVAL INTERSECTION OF (
  _gc_slice_with_final_heap AS g,
  _gc_heap_counter AS h
) PER upid
|> AGGREGATE
     ANY_VALUE(g.gc_ts) AS ts,
     ANY_VALUE(g.gc_dur) AS dur,
     ANY_VALUE(g.upid) AS upid,
     ANY_VALUE(g.gc_name) AS gc_name,
     ANY_VALUE(g.gc_ts) AS gc_ts,
     ANY_VALUE(g.gc_dur) AS gc_dur,
     ANY_VALUE(g.utid) AS utid,
     ANY_VALUE(g.tid) AS tid,
     ANY_VALUE(g.pid) AS pid,
     ANY_VALUE(g.thread_name) AS thread_name,
     ANY_VALUE(g.process_name) AS process_name,
     ANY_VALUE(g.last_value) AS last_value,
     ANY_VALUE(h.value) AS value,
     CASE
       WHEN ANY_VALUE(g.gc_name) GLOB '*NativeAlloc*' THEN 'native_alloc'
       WHEN ANY_VALUE(g.gc_name) GLOB '*Alloc*' THEN 'alloc'
       WHEN ANY_VALUE(g.gc_name) GLOB '*young*' THEN 'young'
       WHEN ANY_VALUE(g.gc_name) GLOB '*CollectorTransition*' THEN 'collector_transition'
       WHEN ANY_VALUE(g.gc_name) GLOB '*Explicit*' THEN 'explicit'
       ELSE 'full'
     END AS gc_type,
     iif(ANY_VALUE(g.gc_name) GLOB '*mark compact*', 1, 0) AS is_mark_compact,
     max(max(h.value, g.last_value)) / 1e3 AS max_heap_mb,
     min(min(h.value, g.last_value)) / 1e3 AS min_heap_mb
   GROUP BY g.gc_id;

-- All Garbage collection events with a breakdown of the time spent and heap reclaimed.
CREATE PERFETTO PIPELINE android_garbage_collection_events(
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
) MATERIALIZED AS
-- Split GC events by thread states to breakdown the time spent.
FROM _gc_slice_heap AS h
|> INTERVAL SPLIT thread_state AS st PER utid
|> AGGREGATE
     ANY_VALUE(h.tid) AS tid,
     ANY_VALUE(h.pid) AS pid,
     ANY_VALUE(h.utid) AS utid,
     ANY_VALUE(h.upid) AS upid,
     ANY_VALUE(h.thread_name) AS thread_name,
     ANY_VALUE(h.process_name) AS process_name,
     ANY_VALUE(h.gc_type) AS gc_type,
     ANY_VALUE(h.is_mark_compact) AS is_mark_compact,
     ANY_VALUE(h.gc_ts) AS gc_ts,
     ANY_VALUE(h.gc_dur) AS gc_dur,
     sum(dur) AS dur,
     ANY_VALUE(h.max_heap_mb) - ANY_VALUE(h.min_heap_mb) AS reclaimed_mb,
     ANY_VALUE(h.min_heap_mb) AS min_heap_mb,
     ANY_VALUE(h.max_heap_mb) AS max_heap_mb
   GROUP BY h.gc_id, st.state, st.io_wait
|> AGGREGATE
     ANY_VALUE(tid) AS tid,
     ANY_VALUE(pid) AS pid,
     ANY_VALUE(utid) AS utid,
     ANY_VALUE(upid) AS upid,
     ANY_VALUE(thread_name) AS thread_name,
     ANY_VALUE(process_name) AS process_name,
     ANY_VALUE(gc_type) AS gc_type,
     ANY_VALUE(is_mark_compact) AS is_mark_compact,
     ANY_VALUE(reclaimed_mb) AS reclaimed_mb,
     ANY_VALUE(min_heap_mb) AS min_heap_mb,
     ANY_VALUE(max_heap_mb) AS max_heap_mb,
     ANY_VALUE(gc_ts) AS gc_ts,
     ANY_VALUE(gc_dur) AS gc_dur,
     sum(iif(state = 'Running', dur, 0)) AS gc_running_dur,
     sum(iif(state = 'R' OR state = 'R+', dur, 0)) AS gc_runnable_dur,
     sum(iif(state = 'D' AND io_wait = 1, dur, 0)) AS gc_unint_io_dur,
     sum(iif(state = 'D' AND io_wait != 1, dur, 0)) AS gc_unint_non_io_dur,
     sum(iif(state = 'S', dur, 0)) AS gc_int_dur
   GROUP BY gc_id;

-- A window of the trace to use for GC stats.
-- We can't reliably use trace_dur(), because often it spans far outside the
-- range of relevant data. Instead pick the window based on when we have
-- 'Heap size (KB)' data available.
CREATE PERFETTO PIPELINE _gc_stats_window MATERIALIZED AS
FROM counter AS c
|> LEFT JOIN process_counter_track AS t ON c.track_id = t.id
|> WHERE t.name = 'Heap size (KB)'
|> AGGREGATE
     min(ts) AS gc_stats_window_start,
     max(ts) AS gc_stats_window_end,
     max(ts) - min(ts) AS gc_stats_window_dur;

-- Count heap allocations by summing positive changes to the 'Heap size (KB)'
-- counter.
CREATE PERFETTO PIPELINE _gc_heap_allocated MATERIALIZED AS
FROM counter AS c
|> LEFT JOIN process_counter_track AS t ON c.track_id = t.id
|> WHERE t.name = 'Heap size (KB)'
|> SELECT
     upid,
     ts,
     ts - lag(ts) OVER (PARTITION BY upid ORDER BY ts) AS dur,
     value,
     CASE
       WHEN lag(c.value) OVER (PARTITION BY upid ORDER BY ts) < c.value THEN c.value
       - lag(c.value) OVER (PARTITION BY upid ORDER BY ts)
       ELSE 0
     END AS allocated;

-- Intersection of startup events and gcs, for understanding what GCs are
-- happining during app startup.
CREATE PERFETTO PIPELINE _gc_during_android_startup MATERIALIZED AS
SUBPIPELINE startups_for_intersect AS (
  FROM android_startups
  -- b/384732321
  |> WHERE dur > 0
  |> SELECT ts, dur, startup_id AS id
)
SUBPIPELINE gcs_for_intersect AS (
  FROM android_garbage_collection_events
  |> SELECT gc_ts AS ts, gc_dur AS dur, gc_id AS id
)
INTERVAL INTERSECTION OF (
  startups_for_intersect AS x,
  gcs_for_intersect AS y
)
|> SELECT ts, dur, x.id AS startup_id, y.id AS gc_id;

-- Estimate heap utilization across the trace.
-- We weight the utilization by gc_period, which is meant to represent the time
-- since the previous GC. We approximate gc_period as time to the start of the
-- process or metrics utilization in case there is no previous GC for the
-- process.
CREATE PERFETTO PIPELINE _gc_heap_utilization MATERIALIZED AS
-- The first GC for a process doesn't have a previous GC to calculate
-- gc_period from. Create pretend GCs at the start of each process to use
-- for computing gc_period for all the real GCs.
SUBPIPELINE before_first_gcs AS (
  FROM process, _gc_stats_window
  |> SELECT
       upid,
       CASE
         WHEN start_ts IS NULL THEN gc_stats_window_start
         ELSE max(start_ts, gc_stats_window_start)
       END AS gc_ts,
       0 AS gc_dur,
       0 AS min_heap_mb,
       0 AS max_heap_mb
)
FROM android_garbage_collection_events
|> SELECT upid, gc_ts, gc_dur, min_heap_mb, max_heap_mb
|> UNION (FROM before_first_gcs)
|> SELECT
     upid,
     gc_ts + gc_dur
     - lag(gc_ts + gc_dur) OVER (PARTITION BY upid ORDER BY gc_ts) AS gc_period,
     min_heap_mb,
     max_heap_mb
|> WHERE min_heap_mb IS NOT NULL
|> AGGREGATE
     sum(gc_period * min_heap_mb) / 1e9 AS heap_live_mbs,
     sum(gc_period * max_heap_mb) / 1e9 AS heap_total_mbs
   GROUP BY upid;

-- Summary stats about how garbage collection is behaving for a process,
-- including causes, costs and other information relevant for tuning the
-- garbage collector.
CREATE PERFETTO PIPELINE _android_garbage_collection_process_stats(
  -- Upid of process the stats are for.
  upid JOINID(process.id),
  -- The start of the window of time that the stats cover in the trace.
  ts TIMESTAMP,
  -- The duration of the window of time that the stats cover in the trace.
  dur DURATION,
  -- Megabyte-seconds of heap size of the process, used in the calculation
  -- of heap_size_mb.
  heap_size_mbs DOUBLE,
  -- Average heap size of the process, in MB.
  heap_size_mb DOUBLE,
  -- Total number of bytes allocated by the process in the window if interest.
  heap_allocated_mb DOUBLE,
  -- Rate of heap allocations in MB per second.
  heap_allocation_rate DOUBLE,
  -- Megabyte-seconds of live heap for processes that had GC events.
  heap_live_mbs DOUBLE,
  -- Megabyte-seconds of total heap for processes that had GC events.
  heap_total_mbs DOUBLE,
  -- Average heap utilization for the process.
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
  -- Time GC is running in the process during startup of some other app. Used
  -- in the calculation of gc_during_android_startup_rate.
  gc_during_android_startup_dur DURATION,
  -- Total startup time in the trace, used to normalize
  -- the gc_during_android_startup_rate.
  total_android_startup_dur DURATION,
  -- Time GC in this process is running during app startup, as a fraction of
  -- startup time in the trace. This gives a sense of how much potential
  -- interference there is between GC and application startup.
  gc_during_android_startup_rate DOUBLE,
  -- A measure of how efficient GC is with regards to gc during application
  -- startup, independent of how aggressively GC is tuned. Larger values
  -- indicate more efficient GC, so larger is better.
  gc_during_android_startup_efficiency DOUBLE
) MATERIALIZED AS
SUBPIPELINE gc_running_stats AS (
  FROM android_garbage_collection_events
  |> AGGREGATE sum(gc_running_dur) AS gc_running_dur GROUP BY upid
)
SUBPIPELINE gc_startup_stats AS (
  FROM _gc_during_android_startup
  |> LEFT JOIN android_garbage_collection_events USING (gc_id)
  |> AGGREGATE sum(dur) AS gc_during_android_startup_dur GROUP BY upid
)
SUBPIPELINE startup_stats AS (
  FROM android_startups
  |> AGGREGATE sum(dur) AS total_android_startup_dur
)
SUBPIPELINE pre_normalized_stats AS (
  FROM _gc_heap_allocated
  |> AGGREGATE
       sum(allocated) / 1e3 AS heap_allocated_mb,
       sum(value * dur) / (1e3 * 1e9) AS heap_size_mbs
     GROUP BY upid
  |> LEFT JOIN gc_running_stats USING (upid)
  |> LEFT JOIN gc_startup_stats USING (upid)
  |> LEFT JOIN _gc_heap_utilization USING (upid)
  |> JOIN _gc_stats_window
  |> JOIN startup_stats
)
SUBPIPELINE normalized_stats AS (
  FROM pre_normalized_stats
  |> SELECT
       upid,
       gc_running_dur * 1.0 / gc_stats_window_dur AS gc_running_rate,
       gc_during_android_startup_dur * 1.0 / total_android_startup_dur AS gc_during_android_startup_rate,
       heap_allocated_mb * 1e9 / gc_stats_window_dur AS heap_allocation_rate,
       heap_size_mbs * 1e9 / gc_stats_window_dur AS heap_size_mb,
       heap_live_mbs / heap_total_mbs AS heap_utilization
)
FROM pre_normalized_stats
|> JOIN normalized_stats USING (upid)
|> SELECT
     upid,
     gc_stats_window_start AS ts,
     gc_stats_window_dur AS dur,
     heap_size_mbs,
     heap_size_mb,
     heap_allocated_mb,
     heap_allocation_rate,
     heap_live_mbs,
     heap_total_mbs,
     heap_utilization,
     gc_running_dur,
     gc_running_rate,
     heap_allocation_rate * heap_utilization
     / (gc_running_rate * (1 - heap_utilization)) AS gc_running_efficiency,
     gc_during_android_startup_dur,
     total_android_startup_dur,
     gc_during_android_startup_rate,
     heap_allocation_rate * heap_utilization
     / (gc_during_android_startup_rate * (1 - heap_utilization)) AS gc_during_android_startup_efficiency;

-- Summary stats about how garbage collection is behaving across the device,
-- including causes, costs and other information relevant for tuning the
-- garbage collector.
CREATE PERFETTO PIPELINE _android_garbage_collection_stats(
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
) MATERIALIZED AS
FROM _android_garbage_collection_process_stats
|> AGGREGATE
     ANY_VALUE(ts) AS ts,
     ANY_VALUE(dur) AS dur,
     sum(heap_size_mbs) AS heap_size_mbs,
     sum(heap_size_mb) AS heap_size_mb,
     sum(heap_allocated_mb) AS heap_allocated_mb,
     sum(heap_allocation_rate) AS heap_allocation_rate,
     sum(heap_live_mbs) AS heap_live_mbs,
     sum(heap_total_mbs) AS heap_total_mbs,
     sum(heap_live_mbs) / sum(heap_total_mbs) AS heap_utilization,
     sum(gc_running_dur) AS gc_running_dur,
     sum(gc_running_rate) AS gc_running_rate,
     sum(gc_during_android_startup_dur) AS gc_during_android_startup_dur,
     ANY_VALUE(total_android_startup_dur) AS total_android_startup_dur,
     sum(gc_during_android_startup_rate) AS gc_during_android_startup_rate
|> SELECT
     *,
     heap_allocation_rate * heap_utilization
     / (gc_running_rate * (1 - heap_utilization)) AS gc_running_efficiency,
     heap_allocation_rate * heap_utilization
     / (gc_during_android_startup_rate * (1 - heap_utilization)) AS gc_during_android_startup_efficiency;
