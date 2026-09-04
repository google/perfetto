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
  utid JOINID(thread.id)
)
AS
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
  MIN(LEAD(ts, 1, ts + dur) OVER (PARTITION BY lock_name ORDER BY ts), ts + dur)
  - ts AS dur,
  is_incomplete,
  lock_name,
  utid
FROM held;
