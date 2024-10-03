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

INCLUDE PERFETTO MODULE intervals.intersect;
INCLUDE PERFETTO MODULE wattson.cpu_freq;
INCLUDE PERFETTO MODULE wattson.cpu_idle;
INCLUDE PERFETTO MODULE wattson.curves.utils;
INCLUDE PERFETTO MODULE wattson.device_infos;

-- Helper macro for using Perfetto table with interval intersect
CREATE PERFETTO MACRO _ii_subquery(tab TableOrSubquery)
RETURNS TableOrSubquery AS (SELECT _auto_id AS id, * FROM $tab);

-- Wattson estimation is valid from when first CPU0 frequency appears
CREATE PERFETTO TABLE _valid_window
AS
WITH window_start AS (
  SELECT ts as start_ts
  FROM _adjusted_cpu_freq
  WHERE cpu = 0 AND freq IS NOT NULL
  ORDER BY ts ASC
  LIMIT 1
),
window AS (
  SELECT start_ts as ts, trace_end() - start_ts as dur
  FROM window_start
)
SELECT *, 0 as cpu FROM window
UNION ALL
SELECT *, 1 as cpu FROM window
UNION ALL
SELECT *, 2 as cpu FROM window
UNION ALL
SELECT *, 3 as cpu FROM window
UNION ALL
SELECT *, 4 as cpu FROM window
UNION ALL
SELECT *, 5 as cpu FROM window
UNION ALL
SELECT *, 6 as cpu FROM window
UNION ALL
SELECT *, 7 as cpu FROM window;

-- Start matching CPUs with 1D curves based on combination of freq and idle
CREATE PERFETTO TABLE _idle_freq_materialized
AS
SELECT
  ii.ts, ii.dur, ii.cpu, freq.policy, freq.freq, idle.idle, lut.curve_value
FROM _interval_intersect!(
  (
    _ii_subquery!(_valid_window),
    _ii_subquery!(_adjusted_cpu_freq),
    _ii_subquery!(_adjusted_deep_idle)
  ),
  (cpu)
) ii
JOIN _adjusted_cpu_freq AS freq ON freq._auto_id = id_1
JOIN _adjusted_deep_idle AS idle ON idle._auto_id = id_2
-- Left join since some CPUs may only match the 2D LUT
LEFT JOIN _filtered_curves_1d lut ON
  freq.policy = lut.policy AND
  freq.freq = lut.freq_khz AND
  idle.idle = lut.idle;

