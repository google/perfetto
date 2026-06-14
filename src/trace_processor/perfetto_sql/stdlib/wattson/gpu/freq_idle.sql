--
-- Copyright 2025 The Android Open Source Project
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

-- NOTE (psqlnext): the `intervals.intersect` module is DELETED — the
-- `_interval_intersect!`/`_ii_subquery!`/`_auto_id`/`id_N` machinery is
-- `INTERVAL INTERSECTION OF (… AS alias) PER …` with nominal payload access.

INCLUDE PERFETTO MODULE android.gpu.frequency;

INCLUDE PERFETTO MODULE android.gpu.mali_power_state;

INCLUDE PERFETTO MODULE wattson.device_infos;

INCLUDE PERFETTO MODULE wattson.utils;

-- GPU power state which is analogous to CPU idle state
CREATE PERFETTO PIPELINE _pvr_gpu_power_state(
  -- Timestamp
  ts TIMESTAMP,
  -- Duration
  dur DURATION,
  -- GPU power state
  power_state LONG
)
MATERIALIZED AS
FROM slice AS s
|> JOIN track AS t ON s.track_id = t.id
|> WHERE t.name = 'powervr_gpu_power_state'
|> SELECT
     s.ts,
     iif(s.dur = -1, trace_end() - s.ts, s.dur) AS dur,
     -- Map slice names to integer states
     CASE s.name WHEN 'OFF' THEN 0 WHEN 'PG' THEN 1 WHEN 'ON' THEN 2 ELSE -1 END AS power_state;

-- Gapless time slices of GPU freq from trace_start() to trace_end()
CREATE PERFETTO PIPELINE _gapless_gpu_freq MATERIALIZED AS
-- The single gpu_id this device reports under.
SUBPIPELINE device_gpu AS (
  FROM _gpuid_map
  |> JOIN _wattson_device ON _gpuid_map.device = _wattson_device.name
  |> SELECT gpu_id
)
-- Prepend NULL slices up to first freq events
FROM android_gpu_frequency
|> WHERE gpu_id = (FROM device_gpu |> SELECT gpu_id)
|> AGGREGATE
     trace_start() AS ts,
     min(ts) - trace_start() AS dur,
     NULL AS freq,
     NULL AS prev_freq,
     ARG_MIN(ts, next_gpu_freq) AS next_freq,
     ANY_VALUE(gpu_id) AS gpu_id
|> UNION ALL (
     FROM android_gpu_frequency
     |> WHERE gpu_id = (FROM device_gpu |> SELECT gpu_id)
     |> SELECT
          ts,
          dur,
          gpu_freq AS freq,
          prev_gpu_freq AS prev_freq,
          next_gpu_freq AS next_freq,
          gpu_id
   );

-- A single source for GPU power state information
-- from either mali or pvr which are mutually exclusive
CREATE PERFETTO PIPELINE _gpu_power_state AS
FROM android_mali_gpu_power_state
|> SELECT ts, dur, power_state
|> UNION ALL (FROM _pvr_gpu_power_state |> SELECT ts, dur, power_state);

-- Gapless time slices of GPU idle from trace_start() to trace_end()
CREATE PERFETTO PIPELINE _gapless_gpu_power_state MATERIALIZED AS
-- Prepend NULL slices up to first idle events
FROM _gpu_power_state
|> AGGREGATE
     trace_start() AS ts,
     min(ts) - trace_start() AS dur,
     NULL AS power_state
|> UNION ALL (FROM _gpu_power_state |> SELECT ts, dur, power_state);

CREATE PERFETTO PIPELINE _gpu_freq_idle MATERIALIZED AS
-- Co-fragmenting intersection of the gapless freq and gapless power streams; each
-- output fragment carries both operands' payload by alias.
INTERVAL INTERSECTION OF (
  _gapless_gpu_freq AS freq,
  _gapless_gpu_power_state AS idle
)
|> SELECT
     ts,
     dur,
     -- From power perspective, even though driver is reporting freq=0, it actually
     -- is still at the previous frequency but in a shallower idle state.
     --
     -- This logic accounts for the inverse idle state relative to CPU idle states,
     -- and converts the GPU power state to be same scale as CPU idle state for
     -- consistency. (smaller numbers correspond to deeper idle states on Mali, and
     -- larger numbers correspond to deeper idle state on CPUs).
     iif(
       idle.power_state = 2
       AND freq.freq = 0,
       iif(freq.prev_freq = 0, freq.next_freq, freq.prev_freq),
       freq.freq
     ) AS freq,
     iif(idle.power_state = 2 AND freq.freq = 0, 1, idle.power_state) AS power_state;
