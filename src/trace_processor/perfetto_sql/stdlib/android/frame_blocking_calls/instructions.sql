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

-- Attributes Linux perf hardware instruction counts (`HW_INSTRUCTIONS`) to
-- Android CUJ per-frame critical blocking calls (including `hwuiTask`) and
-- aggregates per-frame maximum/mean instruction counts and occurrence counts
-- per completed jank CUJ.
--
-- Only hardware instructions executed locally on the calling thread
-- (`ui_thread`, `RenderThread`, or `hwuiTask*`) while that thread is in the
-- `Running` (`sched`) state are counted. Sleeping/off-CPU portions of a
-- blocking call contribute 0 instructions.
INCLUDE PERFETTO MODULE android.frame_blocking_calls.blocking_calls_aggregation;

INCLUDE PERFETTO MODULE intervals.intersect;

-- Per-thread time bounds of blocking call (`bc`) intervals in completed jank
-- CUJs. Private helper tables below use `bc` as shorthand for blocking calls.
-- Materialized so `sched` and `counter` scans are bounded to the CUJ window
-- and so `cuj_bounds` remains populated even if all blocking calls slept
-- off-CPU.
CREATE PERFETTO TABLE _android_cuj_bc_utid_bounds AS
SELECT utid, min(ts) AS min_ts, max(ts_end) AS max_ts_end
FROM android_cuj_frame_blocking_calls
GROUP BY
  utid;

-- `sched` running intervals filtered to threads and time bounds of CUJ frame
-- blocking calls.
CREATE PERFETTO TABLE _android_cuj_bc_active_sched AS
SELECT s.id, s.ts, s.dur, s.cpu, s.utid
FROM sched AS s
JOIN _android_cuj_bc_utid_bounds AS b ON s.utid = b.utid
WHERE
  s.dur > 0
  AND s.cpu IS NOT NULL
  AND s.ts < b.max_ts_end
  AND s.ts + s.dur > b.min_ts;

-- On-CPU segments of each blocking call interval, tagged with the CPU on which
-- the thread executed. Materialized so joining with `_interval_intersect!`
-- uses indexed lookups on `id`.
CREATE PERFETTO TABLE _android_cuj_bc_on_cpu AS
SELECT
  row_number() OVER (ORDER BY ii.ts, ii.id_0, ii.id_1) AS id,
  ii.ts,
  ii.dur,
  ii.id_0 AS bc_id,
  s.cpu
FROM _interval_intersect!(
  (android_cuj_frame_blocking_calls, _android_cuj_bc_active_sched),
  (utid)
) AS ii
JOIN _android_cuj_bc_active_sched AS s ON s.id = ii.id_1
WHERE
  ii.dur > 0;

-- Per-CPU hardware instruction counter intervals covering the CUJ blocking
-- call window, including the nearest surrounding boundary samples at or before
-- `min_ts` and at or after `max_ts_end`. Materialized so joining with
-- `_interval_intersect!` uses indexed lookups on `id`.
CREATE PERFETTO TABLE _android_cuj_bc_perf_instruction_intervals AS
WITH
  cuj_bounds AS (
    SELECT min(min_ts) AS min_ts, max(max_ts_end) AS max_ts_end
    FROM _android_cuj_bc_utid_bounds
  ),
  perf_tracks AS (
    SELECT id AS track_id, cpu
    FROM perf_counter_track
    WHERE
      cpu IS NOT NULL
      AND (name = 'instructions' OR name GLOB 'instructions:*')
  ),
  track_window AS (
    SELECT
      pt.track_id,
      pt.cpu,
      coalesce(
        (
          SELECT max(c.ts)
          FROM counter AS c
          WHERE
            c.track_id = pt.track_id
            AND c.ts <= cb.min_ts
        ),
        cb.min_ts
      ) AS start_ts,
      coalesce(
        (
          SELECT min(c.ts)
          FROM counter AS c
          WHERE
            c.track_id = pt.track_id
            AND c.ts >= cb.max_ts_end
        ),
        cb.max_ts_end
      ) AS end_ts
    FROM perf_tracks AS pt
    JOIN cuj_bounds AS cb
    WHERE
      cb.min_ts IS NOT NULL
  ),
  perf_instruction_raw AS (
    SELECT
      c.ts,
      lead(c.ts) OVER (PARTITION BY c.track_id ORDER BY c.ts ASC) AS next_ts,
      c.value,
      lead(c.value) OVER (PARTITION BY c.track_id ORDER BY c.ts ASC) AS next_value,
      tw.cpu
    FROM track_window AS tw
    JOIN counter AS c
      ON c.track_id = tw.track_id
      AND c.ts >= tw.start_ts
      AND c.ts <= tw.end_ts
  )
SELECT
  row_number() OVER (ORDER BY ts, cpu) AS id,
  ts,
  next_ts - ts AS dur,
  cpu,
  next_value - value AS instruction_delta
FROM perf_instruction_raw
WHERE
  next_ts > ts
  AND next_value >= value;

-- Per-interval blocking call hardware instruction counts within extended frame
-- boundaries of completed jank CUJs.
--
-- Empty if the trace does not contain Linux perf `instructions` counter
-- intervals covering the CUJ blocking calls. Blocking call intervals that
-- execute entirely off-CPU report `0.0` instructions when perf instruction
-- counters are present.
CREATE PERFETTO TABLE android_cuj_frame_blocking_calls_instructions(
  -- Unique row ID matching `android_cuj_frame_blocking_calls.id`.
  id LONG,
  -- Logical blocking call ID matching
  -- `android_cuj_frame_blocking_calls.call_id`.
  call_id LONG,
  -- Standardized name of the blocking call (or 'hwuiTask').
  name STRING,
  -- Start timestamp of the blocking call interval, clamped to the frame start.
  ts TIMESTAMP,
  -- Duration of the blocking call interval within the frame boundaries.
  dur DURATION,
  -- End timestamp of the blocking call interval, clamped to the frame end.
  ts_end TIMESTAMP,
  -- Unique incremental ID for each CUJ.
  cuj_id LONG,
  -- Name of the CUJ without the 'J<' prefix.
  cuj_name STRING,
  -- Frame (vsync) ID.
  frame_id LONG,
  -- Layer ID associated with the frame.
  layer_id LONG,
  -- Process upid.
  upid JOINID(process.id),
  -- Process name.
  process_name STRING,
  -- Thread utid on which the blocking call interval ran.
  utid JOINID(thread.id),
  -- Estimated local hardware instructions executed on-CPU by `utid` during
  -- this blocking call interval.
  instruction_count DOUBLE
)
AS
WITH
  blocking_call_instructions AS (
    SELECT
      bc_cpu.bc_id,
      sum(pi.instruction_delta * (cast_double!(ii.dur) / pi.dur)) AS instruction_count
    FROM _interval_intersect!(
      (_android_cuj_bc_on_cpu, _android_cuj_bc_perf_instruction_intervals),
      (cpu)
    ) AS ii
    JOIN _android_cuj_bc_on_cpu AS bc_cpu ON bc_cpu.id = ii.id_0
    JOIN _android_cuj_bc_perf_instruction_intervals AS pi ON pi.id = ii.id_1
    WHERE
      ii.dur > 0
    GROUP BY
      bc_cpu.bc_id
  )
SELECT
  bc.id,
  bc.call_id,
  bc.name,
  bc.ts,
  bc.dur,
  bc.ts_end,
  bc.cuj_id,
  bc.cuj_name,
  bc.frame_id,
  bc.layer_id,
  bc.upid,
  bc.process_name,
  bc.utid,
  coalesce(bci.instruction_count, 0.0) AS instruction_count
FROM android_cuj_frame_blocking_calls AS bc
LEFT JOIN blocking_call_instructions AS bci ON bc.id = bci.bc_id
WHERE
  EXISTS (SELECT 1 FROM _android_cuj_bc_perf_instruction_intervals)
ORDER BY
  bc.id;

-- Per-CUJ per-frame summary of hardware instruction counts and occurrence
-- counts for critical blocking calls (including `hwuiTask`) in completed jank
-- CUJs.
--
-- Empty if the trace does not contain Linux perf `instructions` counter
-- intervals covering the CUJ blocking calls.
CREATE PERFETTO TABLE android_cuj_frame_blocking_calls_instructions_per_cuj(
  -- Name of the CUJ without the 'J<' prefix.
  cuj_name STRING,
  -- Process upid.
  upid JOINID(process.id),
  -- Process name.
  process_name STRING,
  -- Standardized name of the blocking call (or 'hwuiTask').
  blocking_call_name STRING,
  -- Maximum total local hardware instructions executed per frame for this
  -- blocking call across frames of the CUJ.
  max_instructions DOUBLE,
  -- Mean local hardware instructions per frame (divided by all frames in the
  -- CUJ) for this blocking call.
  mean_instructions DOUBLE,
  -- Maximum distinct occurrences per frame for this blocking call across
  -- frames of the CUJ.
  max_cnt LONG,
  -- Mean distinct occurrences per frame (divided by all frames in the CUJ) for
  -- this blocking call.
  mean_cnt DOUBLE
)
AS
WITH
  per_frame_aggregate_values AS (
    SELECT
      cuj_name,
      upid,
      process_name,
      name AS blocking_call_name,
      frame_id,
      count(DISTINCT call_id) AS cnt,
      sum(instruction_count) AS total_instructions_per_frame
    FROM android_cuj_frame_blocking_calls_instructions
    GROUP BY
      cuj_id,
      cuj_name,
      upid,
      process_name,
      name,
      frame_id
  ),
  frame_cnt_per_cuj AS (
    SELECT count(*) AS frame_cnt, cuj_name, upid
    FROM android_cuj_frame_boundaries
    GROUP BY
      cuj_name,
      upid
  )
SELECT
  bc.cuj_name,
  bc.upid,
  bc.process_name,
  bc.blocking_call_name,
  cast_double!(max(bc.total_instructions_per_frame)) AS max_instructions,
  cast_double!(sum(bc.total_instructions_per_frame)) / fc.frame_cnt AS mean_instructions,
  max(bc.cnt) AS max_cnt,
  cast_double!(sum(bc.cnt)) / fc.frame_cnt AS mean_cnt
FROM per_frame_aggregate_values AS bc
JOIN frame_cnt_per_cuj AS fc USING (cuj_name, upid)
GROUP BY
  bc.cuj_name,
  bc.upid,
  bc.process_name,
  bc.blocking_call_name
ORDER BY
  bc.cuj_name,
  bc.process_name,
  bc.blocking_call_name,
  bc.upid;
