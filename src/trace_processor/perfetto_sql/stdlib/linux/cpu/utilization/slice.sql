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

INCLUDE PERFETTO MODULE linux.cpu.utilization.general;

INCLUDE PERFETTO MODULE time.conversion;
INCLUDE PERFETTO MODULE intervals.intersect;
INCLUDE PERFETTO MODULE slices.with_context;

-- CPU cycles per each slice.
CREATE PERFETTO TABLE cpu_cycles_per_thread_slice(
  -- Id of a slice. Alias of `slice.id`.
  id INT,
  -- Name of the slice.
  name STRING,
  -- Id of the thread the slice is running on. Alias of `thread.id`.
  utid INT,
  -- Name of the thread.
  thread_name STRING,
  -- Id of the process the slice is running on. Alias of `process.id`.
  upid INT,
  -- Name of the process.
  process_name STRING,
  -- Sum of CPU millicycles. Null if frequency couldn't be fetched for any
  -- period during the runtime of the slice.
  millicycles INT,
  -- Sum of CPU megacycles. Null if frequency couldn't be fetched for any
  -- period during the runtime of the slice.
  megacycles INT
) AS
WITH intersected AS (
  SELECT
    id_0 AS slice_id,
    ii.utid,
    sum(ii.dur) AS dur,
    cast_int!(SUM(ii.dur * freq / 1000)) AS millicycles,
    cast_int!(SUM(ii.dur * freq / 1000) / 1e9) AS megacycles
  FROM _interval_intersect!(
    ((SELECT * FROM thread_slice WHERE dur > 0 AND utid > 0),
    _cpu_freq_per_thread), (utid)) ii
  JOIN _cpu_freq_per_thread f ON f.id = ii.id_1
  WHERE freq IS NOT NULL
  GROUP BY slice_id
)
SELECT
  id,
  ts.name,
  ts.utid,
  ts.thread_name,
  ts.upid,
  ts.process_name,
  millicycles,
  megacycles
FROM thread_slice ts
LEFT JOIN intersected ON slice_id = ts.id AND ts.dur = intersected.dur;

-- CPU cycles per each slice in interval.
CREATE PERFETTO FUNCTION cpu_cycles_per_thread_slice_in_interval(
    -- Start of the interval.
    ts INT,
    -- Duration of the interval.
    dur INT
)
RETURNS TABLE(
  -- Id of a slice. Alias of `slice.id`.
  id INT,
  -- Name of the slice.
  name STRING,
  -- Id of the thread the slice is running on. Alias of `thread.id`.
  utid INT,
  -- Name of the thread.
  thread_name STRING,
  -- Id of the process the slice is running on. Alias of `process.id`.
  upid INT,
  -- Name of the process.
  process_name STRING,
  -- Sum of CPU millicycles. Null if frequency couldn't be fetched for any
  -- period during the runtime of the slice.
  millicycles INT,
  -- Sum of CPU megacycles. Null if frequency couldn't be fetched for any
  -- period during the runtime of the slice.
  megacycles INT
) AS
WITH cut_thread_slice AS (
  SELECT id, ii.ts, ii.dur, thread_slice.*
  FROM _interval_intersect_single!(
    $ts, $dur, 
    (SELECT * FROM thread_slice WHERE dur > 0 AND utid > 0)) ii
  JOIN thread_slice USING (id)
),
intersected AS (
  SELECT
    id_0 AS slice_id,
    ii.utid,
    sum(ii.dur) AS dur,
    cast_int!(SUM(ii.dur * freq / 1000)) AS millicycles,
    cast_int!(SUM(ii.dur * freq / 1000) / 1e9) AS megacycles
  FROM _interval_intersect!(
    (cut_thread_slice, _cpu_freq_per_thread), (utid)) ii
  JOIN _cpu_freq_per_thread f ON f.id = ii.id_1
  WHERE freq IS NOT NULL
  GROUP BY slice_id
)
SELECT
  id,
  ts.name,
  ts.utid,
  ts.thread_name,
  ts.upid,
  ts.process_name,
  millicycles,
  megacycles
FROM cut_thread_slice ts
LEFT JOIN intersected ON slice_id = ts.id AND ts.dur = intersected.dur;