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

-- NOTE (psqlnext): the `intervals.intersect` module is DELETED — the whole
-- `_interval_intersect!`/`_ii_subquery!`/`_auto_id`/`id_N` machinery is
-- `INTERVAL INTERSECTION OF (… AS alias) PER …` with nominal payload access.

INCLUDE PERFETTO MODULE wattson.cpu.freq;

INCLUDE PERFETTO MODULE wattson.cpu.hotplug;

INCLUDE PERFETTO MODULE wattson.cpu.idle;

INCLUDE PERFETTO MODULE wattson.curves.utils;

INCLUDE PERFETTO MODULE wattson.device_infos;

-- Start matching CPUs with 1D curves based on combination of freq and idle.
CREATE PERFETTO PIPELINE _idle_freq_materialized
MATERIALIZED AS
-- Wattson estimation is valid from when the first CPU0 frequency appears; the
-- window spans from there to trace end, per cpu. (Was the `_valid_window` table.)
SUBPIPELINE valid_window AS (
  FROM _adjusted_cpu_freq
  |> WHERE cpu = 0 AND freq IS NOT NULL
  |> ORDER BY ts
  |> LIMIT 1
  |> SELECT ts AS start_ts
  |> CROSS JOIN _dev_cpu_policy_map AS map
  |> SELECT start_ts AS ts, trace_end() - start_ts AS dur, map.cpu
)
-- Four-way co-fragmenting intersection per cpu: each output fragment is a region
-- where the valid window, freq, deep-idle and hotplug streams all overlap, each
-- carrying its own payload by alias.
INTERVAL INTERSECTION OF (
  valid_window AS win,
  _adjusted_cpu_freq AS freq,
  _adjusted_deep_idle AS idle,
  _gapless_hotplug_slices AS hotplug
) PER cpu
-- Left join since some CPUs may only match the 2D LUT.
|> LEFT JOIN _filtered_curves_1d AS lut
     ON freq.policy = lut.policy
     AND freq.freq = lut.freq_khz
     AND idle.idle = lut.idle
|> CROSS JOIN _deepest_idle AS deepest
|> SELECT
     ts,
     dur,
     cpu,
     freq.policy AS policy,
     freq.freq AS freq,
     -- Set idle since subsequent calculations are based on number of idle/active
     -- CPUs. If offline, set the CPU to the device specific deepest idle state.
     iif(hotplug.offline, deepest.idle, idle.idle) AS idle,
     -- If CPU is offline, set power estimate to 0.
     iif(hotplug.offline, 0, lut.curve_value) AS curve_value,
     lut.static AS static;
