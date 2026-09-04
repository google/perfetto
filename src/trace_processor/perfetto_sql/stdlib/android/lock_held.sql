--
-- Copyright 2026 The Android Open Source Project
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

INCLUDE PERFETTO MODULE android.monitor_contention;

INCLUDE PERFETTO MODULE intervals.intersect;

INCLUDE PERFETTO MODULE intervals.overlap;

INCLUDE PERFETTO MODULE slices.with_context;

-- Slices matching `<lock>_lock_held` with the suffix stripped from `lock_name`.
CREATE PERFETTO TABLE _android_lock_held_slices AS
SELECT
  id,
  ts,
  iif(dur = -1, trace_end() - ts, dur) AS dur,
  dur = -1 AS is_incomplete,
  replace(name, '_lock_held', '') AS lock_name,
  utid
FROM thread_slice
WHERE
  name GLOB '*_lock_held';

-- Raw held intervals after merging recursive holds on the same thread
-- and truncating holds when acquired by a successor thread.
CREATE PERFETTO TABLE _android_lock_held_raw AS
WITH
  merged AS (
    SELECT ts, dur, lock_name, utid
    FROM interval_merge_overlapping_partitioned!((
        SELECT ts, dur, lock_name, utid FROM _android_lock_held_slices
      ), (lock_name, utid))
  ),
  held AS (
    SELECT
      held.id,
      merged.ts,
      merged.dur,
      held.is_incomplete,
      merged.lock_name,
      merged.utid
    FROM merged
    JOIN _android_lock_held_slices AS held
      ON held.lock_name = merged.lock_name
      AND held.utid = merged.utid
      AND held.ts = merged.ts
  )
SELECT
  id,
  ts,
  min(lead(ts, 1, ts + dur) OVER (PARTITION BY lock_name ORDER BY ts), ts + dur)
  - ts AS dur,
  is_incomplete,
  lock_name,
  utid
FROM held;

-- Intervals during which a named lock was held by a thread.
-- Overlapping holds on the same thread (recursive locks) are merged.
-- Holds on exclusive locks are truncated when acquired by a successor thread.
CREATE PERFETTO TABLE android_lock_held(
  -- Slice id of the slice starting this hold.
  id JOINID(slice.id),
  -- Timestamp at which the lock was acquired.
  ts TIMESTAMP,
  -- Duration for which the lock was held.
  dur DURATION,
  -- Whether this hold was unreleased before the end of the trace.
  is_incomplete BOOL,
  -- Name of the lock (matching android_monitor_contention.lock_name).
  lock_name STRING,
  -- Thread holding the lock.
  utid JOINID(thread.id),
  -- Java method executed by the holding thread during contention, if any.
  blocking_method STRING
)
AS
WITH
  intersections AS (
    SELECT ii.id_0 AS held_id, min(mc.short_blocking_method) AS blocking_method
    FROM _interval_intersect!(
      (
        _android_lock_held_raw,
        (
          SELECT id, ts, dur, blocking_utid AS utid
          FROM android_monitor_contention
          WHERE blocking_utid IS NOT NULL
        )
      ),
      (utid)
    ) AS ii
    JOIN android_monitor_contention AS mc
      ON mc.id = ii.id_1
    GROUP BY
      held_id
  )
SELECT
  raw.id,
  raw.ts,
  raw.dur,
  raw.is_incomplete,
  raw.lock_name,
  raw.utid,
  intersections.blocking_method
FROM _android_lock_held_raw AS raw
LEFT JOIN intersections
  ON intersections.held_id = raw.id;
