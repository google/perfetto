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

-- Table of suspended and awake slices.
--
-- Selects either the minimal or full ftrace source depending on what's
-- available, marks suspended periods, and complements them to give awake
-- periods.
CREATE PERFETTO PIPELINE android_suspend_state(
  -- Timestamp
  ts TIMESTAMP,
  -- Duration
  dur DURATION,
  -- 'awake' or 'suspended'
  power_state STRING,
  -- Machine identifier for multi-device traces
  machine_id JOINID(machine.id)
)
MATERIALIZED AS
SUBPIPELINE suspend_slice_from_minimal AS (
  FROM track AS t
  |> JOIN slice AS s ON s.track_id = t.id
  |> WHERE t.name = 'Suspend/Resume Minimal'
  |> SELECT
    s.ts,
    s.dur,
    coalesce(
      lead(s.ts) OVER (PARTITION BY t.machine_id ORDER BY s.ts),
      trace_end()
    )
    - s.ts
    - s.dur AS duration_gap,
    t.machine_id
)
SUBPIPELINE suspend_slice_latency AS (
  FROM slice
  |> JOIN track ON slice.track_id = track.id
  |> WHERE
    track.name = 'Suspend/Resume Latency'
    AND (slice.name = 'syscore_resume(0)'
    OR slice.name = 'timekeeping_freeze(0)')
    AND slice.dur != -1
    AND NOT EXISTS (
      SELECT *
      FROM suspend_slice_from_minimal
      WHERE
        suspend_slice_from_minimal.machine_id = track.machine_id
    )
  |> SELECT
    slice.ts,
    slice.dur,
    coalesce(
      lead(slice.ts) OVER (PARTITION BY track.machine_id ORDER BY slice.ts),
      trace_end()
    )
    - slice.ts
    - slice.dur AS duration_gap,
    track.machine_id
)
-- Filter out all the slices that overlapped with the following slices.
-- This happens with data loss where we lose start and end slices for
-- suspends.
FROM suspend_slice_from_minimal
|> UNION ALL (FROM suspend_slice_latency)
|> WHERE duration_gap >= 0
|> SELECT
  ts,
  dur,
  COALESCE(machine_id, 0) AS machine_id,
  'suspended' AS power_state
-- INTERVAL FILL WITHIN trace_bounds tiles each machine's coverage: every
-- suspended slice passes through and the uncovered spans become null-payload
-- fillers, which we label 'awake'. The primary machine (0) is always covered
-- by trace_bounds, so it gets an awake slice even without suspend data.
|> INTERVAL FILL WITHIN trace_bounds PER machine_id
|> SELECT
  ts,
  dur,
  machine_id,
  COALESCE(power_state, 'awake') AS power_state
|> ORDER BY ts;

-- Extracts the duration without counting CPU suspended time from an event.
-- This is the same as converting an event duration from wall clock to monotonic clock.
-- If there was no CPU suspend, the result is same as |dur|.
CREATE PERFETTO FUNCTION _extract_duration_without_suspend(
  -- Timestamp of event.
  ts TIMESTAMP,
  -- Duration of event.
  dur DURATION
)
RETURNS LONG
AS
SELECT to_monotonic($ts + $dur) - to_monotonic($ts);
