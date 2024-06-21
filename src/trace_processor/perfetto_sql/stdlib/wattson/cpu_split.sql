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

INCLUDE PERFETTO MODULE linux.cpu.frequency;
INCLUDE PERFETTO MODULE time.conversion;
INCLUDE PERFETTO MODULE wattson.arm_dsu;
INCLUDE PERFETTO MODULE wattson.cpu_idle;
INCLUDE PERFETTO MODULE wattson.curves.utils;
INCLUDE PERFETTO MODULE wattson.device_infos;

CREATE PERFETTO TABLE _cpu_freq
AS
SELECT
  ts,
  dur,
  freq,
  cf.cpu,
  d_map.policy
FROM cpu_frequency_counters as cf
JOIN _dev_cpu_policy_map as d_map
ON cf.cpu = d_map.cpu;

-- Combines idle and freq tables of all CPUs to create system state.
CREATE VIRTUAL TABLE _idle_freq
USING
  SPAN_OUTER_JOIN(
    _cpu_freq partitioned cpu, _adjusted_deep_idle partitioned cpu
  );

-- Add extra column indicating that frequency info are present
CREATE PERFETTO TABLE _valid_window
AS
WITH window_start AS (
  SELECT ts as start_ts
  FROM _idle_freq
  WHERE cpu = 0 and freq GLOB '*[0-9]*'
  ORDER BY ts ASC
  LIMIT 1
),
window_end AS (
  SELECT ts + dur as end_ts
  FROM cpu_frequency_counters
  ORDER by ts DESC
  LIMIT 1
)
SELECT
  start_ts as ts,
  end_ts - start_ts as dur
FROM window_start, window_end;

CREATE VIRTUAL TABLE _idle_freq_filtered
USING
  SPAN_JOIN(_valid_window, _idle_freq);

-- Start matching split CPUs with curves
CREATE PERFETTO TABLE _idle_freq_materialized
AS
SELECT
  iff.ts, iff.dur, iff.cpu, iff.policy, iff.freq, iff.idle, lut.curve_value
FROM _idle_freq_filtered iff
-- Left join since some CPUs may only match the 2D LUT
LEFT JOIN _filtered_curves_1d lut ON
  iff.policy = lut.policy AND
  iff.idle = lut.idle AND
  iff.freq = lut.freq_khz;

CREATE PERFETTO TABLE _stats_cpu0
AS
SELECT
  ts,
  dur,
  curve_value as cpu0_curve,
  freq as freq_0,
  idle as idle_0
FROM _idle_freq_materialized
WHERE cpu = 0;

CREATE PERFETTO TABLE _stats_cpu1
AS
SELECT
  ts,
  dur,
  curve_value as cpu1_curve,
  freq as freq_1,
  idle as idle_1
FROM _idle_freq_materialized
WHERE cpu = 1;

CREATE PERFETTO TABLE _stats_cpu2
AS
SELECT
  ts,
  dur,
  curve_value as cpu2_curve,
  freq as freq_2,
  idle as idle_2
FROM _idle_freq_materialized
WHERE cpu = 2;

CREATE PERFETTO TABLE _stats_cpu3
AS
SELECT
  ts,
  dur,
  curve_value as cpu3_curve,
  freq as freq_3,
  idle as idle_3
FROM _idle_freq_materialized
WHERE cpu = 3;

CREATE PERFETTO TABLE _stats_cpu4
AS
SELECT
  ts,
  dur,
  policy as policy_4,
  curve_value as cpu4_curve,
  freq as freq_4,
  idle as idle_4
FROM _idle_freq_materialized
WHERE cpu = 4;

CREATE PERFETTO TABLE _stats_cpu5
AS
SELECT
  ts,
  dur,
  policy as policy_5,
  curve_value as cpu5_curve,
  freq as freq_5,
  idle as idle_5
FROM _idle_freq_materialized
WHERE cpu = 5;

CREATE PERFETTO TABLE _stats_cpu6
AS
SELECT
  ts,
  dur,
  policy as policy_6,
  curve_value as cpu6_curve,
  freq as freq_6,
  idle as idle_6
FROM _idle_freq_materialized
WHERE cpu = 6;

CREATE PERFETTO TABLE _stats_cpu7
AS
SELECT
  ts,
  dur,
  policy as policy_7,
  curve_value as cpu7_curve,
  freq as freq_7,
  idle as idle_7
FROM _idle_freq_materialized
WHERE cpu = 7;

CREATE VIRTUAL TABLE _stats_cpu01
USING
  SPAN_OUTER_JOIN(_stats_cpu1, _stats_cpu0);

CREATE VIRTUAL TABLE _stats_cpu012
USING
  SPAN_OUTER_JOIN(_stats_cpu2, _stats_cpu01);

CREATE VIRTUAL TABLE _stats_cpu0123
USING
  SPAN_OUTER_JOIN(_stats_cpu3, _stats_cpu012);

CREATE VIRTUAL TABLE _stats_cpu01234
USING
  SPAN_OUTER_JOIN(_stats_cpu4, _stats_cpu0123);

CREATE VIRTUAL TABLE _stats_cpu012345
USING
  SPAN_OUTER_JOIN(_stats_cpu5, _stats_cpu01234);

CREATE VIRTUAL TABLE _stats_cpu0123456
USING
  SPAN_OUTER_JOIN(_stats_cpu6, _stats_cpu012345);

CREATE VIRTUAL TABLE _stats_cpu01234567
USING
  SPAN_OUTER_JOIN(_stats_cpu7, _stats_cpu0123456);

-- get suspend resume state as logged by ftrace.
CREATE PERFETTO TABLE _suspend_slice
AS
SELECT
  ts, dur, TRUE AS suspended
FROM slice
WHERE name GLOB "timekeeping_freeze(0)";

-- Combine suspend information with CPU idle and frequency system states.
CREATE VIRTUAL TABLE _idle_freq_suspend_slice
USING
  SPAN_OUTER_JOIN(_stats_cpu01234567, _suspend_slice);

-- Combine system state so that it has idle, freq, and L3 hit info.
CREATE VIRTUAL TABLE _idle_freq_l3_hit_slice
USING
  SPAN_OUTER_JOIN(_idle_freq_suspend_slice, _arm_l3_hit_rate);

-- Combine system state so that it has idle, freq, L3 hit, and L3 miss info.
CREATE VIRTUAL TABLE _idle_freq_l3_hit_l3_miss_slice
USING
  SPAN_OUTER_JOIN(_idle_freq_l3_hit_slice, _arm_l3_miss_rate);
