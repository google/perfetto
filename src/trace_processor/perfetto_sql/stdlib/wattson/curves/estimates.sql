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

INCLUDE PERFETTO MODULE wattson.cpu_split;
INCLUDE PERFETTO MODULE wattson.curves.utils;
INCLUDE PERFETTO MODULE wattson.curves.w_cpu_dependence;
INCLUDE PERFETTO MODULE wattson.curves.w_dsu_dependence;
INCLUDE PERFETTO MODULE wattson.device_infos;

-- One of the two tables will be empty, depending on whether the device is
-- dependent on devfreq or a different CPU's frequency
CREATE PERFETTO VIEW _curves_w_dependencies(
  ts LONG,
  dur LONG,
  freq_0 INT,
  idle_0 INT,
  freq_1 INT,
  idle_1 INT,
  freq_2 INT,
  idle_2 INT,
  freq_3 INT,
  idle_3 INT,
  cpu0_curve FLOAT,
  cpu1_curve FLOAT,
  cpu2_curve FLOAT,
  cpu3_curve FLOAT,
  cpu4_curve FLOAT,
  cpu5_curve FLOAT,
  cpu6_curve FLOAT,
  cpu7_curve FLOAT,
  l3_hit_count INT,
  l3_miss_count INT,
  no_static INT,
  all_cpu_deep_idle INT,
  dependent_freq INT,
  dependent_policy INT
) AS
-- Table that is dependent on differet CPU's frequency
SELECT * FROM _w_cpu_dependence
UNION ALL
-- Table that is dependent of devfreq frequency
SELECT * FROM _w_dsu_dependence;

-- Final table showing the curves per CPU per slice
CREATE PERFETTO TABLE _system_state_curves
AS
SELECT
  base.ts,
  base.dur,
  -- base.cpu[0-3]_curve will be non-zero if CPU has 1D dependency
  -- base.cpu[0-3]_curve will be zero if device is suspended or deep idle
  -- base.cpu[0-3]_curve will be NULL if 2D dependency required
  COALESCE(base.cpu0_curve, lut0.curve_value) as cpu0_curve,
  COALESCE(base.cpu1_curve, lut1.curve_value) as cpu1_curve,
  COALESCE(base.cpu2_curve, lut2.curve_value) as cpu2_curve,
  COALESCE(base.cpu3_curve, lut3.curve_value) as cpu3_curve,
  -- base.cpu[4-7]_curve will be non-zero if CPU has 1D dependency
  -- base.cpu[4-7]_curve will be zero if device is suspended or deep idle
  -- base.cpu[4-7]_curve will be NULL if CPU doesn't exist on device
  COALESCE(base.cpu4_curve, 0.0) as cpu4_curve,
  COALESCE(base.cpu5_curve, 0.0) as cpu5_curve,
  COALESCE(base.cpu6_curve, 0.0) as cpu6_curve,
  COALESCE(base.cpu7_curve, 0.0) as cpu7_curve,
  IIF(
    no_static = 1,
    0.0,
    COALESCE(static_1d.curve_value, static_2d.curve_value)
  ) as static_curve,
  IIF(
    all_cpu_deep_idle = 1,
    0,
    base.l3_hit_count * l3_hit_lut.curve_value
  ) as l3_hit_value,
  IIF(
    all_cpu_deep_idle = 1,
    0,
    base.l3_miss_count * l3_miss_lut.curve_value
  ) as l3_miss_value
FROM _curves_w_dependencies as base
-- LUT for 2D dependencies
LEFT JOIN _filtered_curves_2d lut0 ON
  lut0.freq_khz = base.freq_0 AND
  lut0.other_policy = base.dependent_policy AND
  lut0.other_freq_khz = base.dependent_freq AND
  lut0.idle = base.idle_0
LEFT JOIN _filtered_curves_2d lut1 ON
  lut1.freq_khz = base.freq_1 AND
  lut1.other_policy = base.dependent_policy AND
  lut1.other_freq_khz = base.dependent_freq AND
  lut1.idle = base.idle_1
LEFT JOIN _filtered_curves_2d lut2 ON
  lut2.freq_khz = base.freq_2 AND
  lut2.other_policy = base.dependent_policy AND
  lut2.other_freq_khz = base.dependent_freq AND
  lut2.idle = base.idle_2
LEFT JOIN _filtered_curves_2d lut3 ON
  lut3.freq_khz = base.freq_3 AND
  lut3.other_policy = base.dependent_policy AND
  lut3.other_freq_khz = base.dependent_freq AND
  lut3.idle = base.idle_3
-- LUT for static curve lookup
LEFT JOIN _filtered_curves_2d static_2d ON
  static_2d.freq_khz = base.freq_0 AND
  static_2d.other_policy = base.dependent_policy AND
  static_2d.other_freq_khz = base.dependent_freq AND
  static_2d.idle = 255
LEFT JOIN _filtered_curves_1d static_1d ON
  static_1d.policy = 0 AND
  static_1d.freq_khz = base.freq_0 AND
  static_1d.idle = 255
-- LUT joins for L3 cache
LEFT JOIN _filtered_curves_l3 l3_hit_lut ON
  l3_hit_lut.freq_khz = base.freq_0 AND
  l3_hit_lut.other_policy = base.dependent_policy AND
  l3_hit_lut.other_freq_khz = base.dependent_freq AND
  l3_hit_lut.action = 'hit'
LEFT JOIN _filtered_curves_l3 l3_miss_lut ON
  l3_miss_lut.freq_khz = base.freq_0 AND
  l3_miss_lut.other_policy = base.dependent_policy AND
  l3_miss_lut.other_freq_khz = base.dependent_freq AND
  l3_miss_lut.action = 'miss';

-- The most basic components of Wattson, all normalized to be in mW on a per
-- system state basis
CREATE PERFETTO TABLE _system_state_mw
AS
SELECT
  ts,
  dur,
  cpu0_curve as cpu0_mw,
  cpu1_curve as cpu1_mw,
  cpu2_curve as cpu2_mw,
  cpu3_curve as cpu3_mw,
  cpu4_curve as cpu4_mw,
  cpu5_curve as cpu5_mw,
  cpu6_curve as cpu6_mw,
  cpu7_curve as cpu7_mw,
  -- LUT for l3 is scaled by 10^6 to save resolution and in units of kWs. Scale
  -- this by 10^3 so when divided by ns, result is in units of mW
  (
    (
      IFNULL(l3_hit_value, 0) + IFNULL(l3_miss_value, 0)
    ) * 1000 / dur
  ) + static_curve as dsu_scu_mw
FROM _system_state_curves;

-- API to get power from each system state in an arbitrary time window
CREATE PERFETTO FUNCTION _windowed_system_state_mw(ts LONG, dur LONG)
RETURNS TABLE(
  cpu0_mw FLOAT,
  cpu1_mw FLOAT,
  cpu2_mw FLOAT,
  cpu3_mw FLOAT,
  cpu4_mw FLOAT,
  cpu5_mw FLOAT,
  cpu6_mw FLOAT,
  cpu7_mw FLOAT,
  dsu_scu_mw FLOAT
) AS
SELECT
  SUM(ss.cpu0_mw * ss.dur) / SUM(ss.dur) AS cpu0_mw,
  SUM(ss.cpu1_mw * ss.dur) / SUM(ss.dur) AS cpu1_mw,
  SUM(ss.cpu2_mw * ss.dur) / SUM(ss.dur) AS cpu2_mw,
  SUM(ss.cpu3_mw * ss.dur) / SUM(ss.dur) AS cpu3_mw,
  SUM(ss.cpu4_mw * ss.dur) / SUM(ss.dur) AS cpu4_mw,
  SUM(ss.cpu5_mw * ss.dur) / SUM(ss.dur) AS cpu5_mw,
  SUM(ss.cpu6_mw * ss.dur) / SUM(ss.dur) AS cpu6_mw,
  SUM(ss.cpu7_mw * ss.dur) / SUM(ss.dur) AS cpu7_mw,
  SUM(ss.dsu_scu_mw * ss.dur) / SUM(ss.dur) AS dsu_scu_mw
FROM _interval_intersect_single!($ts, $dur, _ii_subquery!(_system_state_mw)) ii
JOIN _system_state_mw AS ss ON ss._auto_id = id;
