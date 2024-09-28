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
  CAST(c.value AS INT) AS value
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
  *,
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
  tid INT,
  -- Pid of process running garbage collection.
  pid INT,
  -- Utid of thread running garbage collection.
  utid INT,
  -- Upid of process running garbage collection.
  upid INT,
  -- Name of thread running garbage collection.
  thread_name STRING,
  -- Name of process running garbage collection.
  process_name STRING,
  -- Type of garbage collection.
  gc_type STRING,
  -- Whether gargage collection is mark compact or copying.
  is_mark_compact INT,
  -- MB reclaimed after garbage collection.
  reclaimed_mb DOUBLE,
  -- Minimum heap size in MB during garbage collection.
  min_heap_mb DOUBLE,
  -- Maximum heap size in MB during garbage collection.
  max_heap_mb DOUBLE,
  -- Garbage collection id.
  gc_id INT,
  -- Garbage collection timestamp.
  gc_ts INT,
  -- Garbage collection wall duration.
  gc_dur INT,
  -- Garbage collection duration spent executing on CPU.
  gc_running_dur INT,
  -- Garbage collection duration spent waiting for CPU.
  gc_runnable_dur INT,
  -- Garbage collection duration spent waiting in the Linux kernel on IO.
  gc_unint_io_dur INT,
  -- Garbage collection duration spent waiting in the Linux kernel without IO.
  gc_unint_non_io_dur INT,
    -- Garbage collection duration spent waiting in interruptible sleep.
  gc_int_dur INT
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
