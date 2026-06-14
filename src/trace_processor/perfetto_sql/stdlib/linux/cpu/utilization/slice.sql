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

INCLUDE PERFETTO MODULE slices.with_context;

INCLUDE PERFETTO MODULE intervals.intersect;

-- CPU cycles per each slice.
CREATE PERFETTO PIPELINE cpu_cycles_per_thread_slice(
  -- Id of a slice.
  id JOINID(slice.id),
  -- Name of the slice.
  name STRING,
  -- Id of the thread the slice is running on.
  utid JOINID(thread.id),
  -- Name of the thread.
  thread_name STRING,
  -- Id of the process the slice is running on.
  upid JOINID(process.id),
  -- Name of the process.
  process_name STRING,
  -- Sum of CPU millicycles. Null if frequency couldn't be fetched for any
  -- period during the runtime of the slice.
  millicycles LONG,
  -- Sum of CPU megacycles. Null if frequency couldn't be fetched for any
  -- period during the runtime of the slice.
  megacycles LONG
)
MATERIALIZED AS
SUBPIPELINE active_slice AS (
  FROM thread_slice
  |> WHERE dur > 0 AND utid > 0
)
SUBPIPELINE intersected AS (
  INTERVAL INTERSECTION OF (active_slice AS s, _cpu_freq_per_thread AS f) PER utid
  |> WHERE f.freq IS NOT NULL
  |> AGGREGATE
       sum(dur) AS dur,
       cast_int!(SUM(dur * f.freq / 1000)) AS millicycles,
       cast_int!(SUM(dur * f.freq / 1000) / 1e9) AS megacycles
     GROUP BY s.id
  |> SELECT s.id AS slice_id, s.utid, dur, millicycles, megacycles
)
FROM thread_slice AS ts
|> LEFT JOIN intersected
     ON intersected.slice_id = ts.id
     AND ts.dur = intersected.dur
|> SELECT
     ts.id,
     ts.name,
     ts.utid,
     ts.thread_name,
     ts.upid,
     ts.process_name,
     millicycles,
     megacycles;

-- NOTE (psqlnext): parameterized by a scalar window, so this is a
-- pipeline-valued macro. The thread slices are first clipped to the caller's
-- window with `_interval_intersect_single!($ts, $dur, …)` (the mid-pipe clip
-- there is no operator for), then co-fragmented against `_cpu_freq_per_thread`
-- per utid with the `INTERVAL INTERSECTION OF` source operator — exactly the
-- non-parameterized `cpu_cycles_per_thread_slice` pipeline above, but over the
-- clipped slices.

-- CPU cycles per each slice in interval.
--
-- This function is only designed to run over a small number of intervals
-- (10-100 at most). It will be *very slow* for large sets of intervals.
CREATE PERFETTO MACRO cpu_cycles_per_thread_slice_in_interval(
  -- Start of the interval.
  ts Expr,
  -- Duration of the interval.
  dur Expr
)
-- Returns: (id JOINID(slice.id), name STRING, utid JOINID(thread.id),
-- thread_name STRING, upid JOINID(process.id), process_name STRING,
-- millicycles LONG, megacycles LONG).
RETURNS Pipeline AS (
  SUBPIPELINE cut_thread_slice AS (
    _interval_intersect_single!(
      $ts, $dur,
      (SELECT * FROM thread_slice WHERE dur > 0 AND utid > 0))
  )
  SUBPIPELINE intersected AS (
    INTERVAL INTERSECTION OF (cut_thread_slice AS s, _cpu_freq_per_thread AS f) PER utid
    |> WHERE f.freq IS NOT NULL
    |> AGGREGATE
         sum(dur) AS dur,
         cast_int!(SUM(dur * f.freq / 1000)) AS millicycles,
         cast_int!(SUM(dur * f.freq / 1000) / 1e9) AS megacycles
       GROUP BY s.id
    |> SELECT s.id AS slice_id, s.utid, dur, millicycles, megacycles
  )
  FROM cut_thread_slice AS ts
  |> LEFT JOIN intersected
       ON intersected.slice_id = ts.id
       AND ts.dur = intersected.dur
  |> SELECT
       ts.id,
       ts.name,
       ts.utid,
       ts.thread_name,
       ts.upid,
       ts.process_name,
       millicycles,
       megacycles
);
