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

-- NOTE (psqlnext): `counter_leading_intervals!` is `INTERVALS FROM EVENTS … PER
-- track_id CLOSING LAST AT(trace_end())` plus `INTERVAL MERGE CONSECUTIVE BY
-- value` to collapse equal-valued runs. The leading/lagging samples (next_value)
-- are recovered with §4 lane-ordered windowing.

-- GPU frequency counter per GPU.
CREATE PERFETTO PIPELINE android_gpu_frequency(
  -- Timestamp
  ts TIMESTAMP,
  -- Duration
  dur DURATION,
  -- GPU id. Joinable with `gpu_counter_track.gpu_id`.
  gpu_id LONG,
  -- GPU frequency
  gpu_freq LONG,
  -- GPU frequency from previous slice
  prev_gpu_freq LONG,
  -- GPU frequency from next slice
  next_gpu_freq LONG
) MATERIALIZED AS
SUBPIPELINE gpufreq_events AS (
  FROM counter AS c
  |> JOIN gpu_counter_track AS t ON t.id = c.track_id AND t.name = 'gpufreq'
  |> WHERE gpu_id IS NOT NULL
  |> SELECT c.id, c.ts, c.track_id, c.value, t.gpu_id
)
INTERVALS FROM EVENTS gpufreq_events PER track_id CLOSING LAST AT (trace_end())
|> INTERVAL MERGE CONSECUTIVE BY value
|> SELECT
  ts,
  dur,
  gpu_id,
  cast_int!(value) AS gpu_freq,
  cast_int!(LAG(value) OVER (PARTITION BY track_id ORDER BY ts)) AS prev_gpu_freq,
  cast_int!(LEAD(value) OVER (PARTITION BY track_id ORDER BY ts)) AS next_gpu_freq;
