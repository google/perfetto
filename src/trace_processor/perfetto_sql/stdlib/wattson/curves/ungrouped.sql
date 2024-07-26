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

INCLUDE PERFETTO MODULE time.conversion;
INCLUDE PERFETTO MODULE wattson.arm_dsu;
INCLUDE PERFETTO MODULE wattson.cpu_split;
INCLUDE PERFETTO MODULE wattson.curves.utils;
INCLUDE PERFETTO MODULE wattson.device_infos;

-- System state table with LUT for CPUs and intermediate values for calculations
CREATE PERFETTO TABLE _w_independent_cpus_calc
AS
SELECT
  base.ts,
  base.dur,
  cast_int!(l3_hit_rate * base.dur) as l3_hit_count,
  cast_int!(l3_miss_rate * base.dur) as l3_miss_count,
  freq_0,
  idle_0,
  freq_1,
  idle_1,
  freq_2,
  idle_2,
  freq_3,
  idle_3,
  freq_4,
  idle_4,
  freq_5,
  idle_5,
  freq_6,
  idle_6,
  freq_7,
  idle_7,
  policy_4,
  policy_5,
  policy_6,
  policy_7,
  IIF(
    suspended = 1,
    1,
    MIN(
      IFNULL(idle_0, 1),
      IFNULL(idle_1, 1),
      IFNULL(idle_2, 1),
      IFNULL(idle_3, 1)
    )
  ) as no_static,
  IIF(suspended = 1, 0, cpu0_curve) as cpu0_curve,
  IIF(suspended = 1, 0, cpu1_curve) as cpu1_curve,
  IIF(suspended = 1, 0, cpu2_curve) as cpu2_curve,
  IIF(suspended = 1, 0, cpu3_curve) as cpu3_curve,
  IIF(suspended = 1, 0, cpu4_curve) as cpu4_curve,
  IIF(suspended = 1, 0, cpu5_curve) as cpu5_curve,
  IIF(suspended = 1, 0, cpu6_curve) as cpu6_curve,
  IIF(suspended = 1, 0, cpu7_curve) as cpu7_curve,
  -- If dependency CPUs are active, then that CPU could contribute static power
  IIF(idle_4 = -1, lut4.curve_value, -1) as static_4,
  IIF(idle_5 = -1, lut5.curve_value, -1) as static_5,
  IIF(idle_6 = -1, lut6.curve_value, -1) as static_6,
  IIF(idle_7 = -1, lut7.curve_value, -1) as static_7
FROM _idle_freq_l3_hit_l3_miss_slice as base
-- Get CPU power curves for CPUs guaranteed on device
JOIN _stats_cpu0 ON _stats_cpu0._auto_id = base.cpu0_id
JOIN _stats_cpu1 ON _stats_cpu1._auto_id = base.cpu1_id
JOIN _stats_cpu2 ON _stats_cpu2._auto_id = base.cpu2_id
JOIN _stats_cpu3 ON _stats_cpu3._auto_id = base.cpu3_id
-- Get CPU power curves for CPUs that aren't always present
LEFT JOIN _stats_cpu4 ON _stats_cpu4._auto_id = base.cpu4_id
LEFT JOIN _stats_cpu5 ON _stats_cpu5._auto_id = base.cpu5_id
LEFT JOIN _stats_cpu6 ON _stats_cpu6._auto_id = base.cpu6_id
LEFT JOIN _stats_cpu7 ON _stats_cpu7._auto_id = base.cpu7_id
-- Match power curves if possible on CPUs that decide 2D dependence
LEFT JOIN _filtered_curves_2d lut4 ON
  _stats_cpu0.freq_0 = lut4.freq_khz AND
  _stats_cpu4.policy_4 = lut4.other_policy AND
  _stats_cpu4.freq_4 = lut4.other_freq_khz AND
  lut4.idle = 255
LEFT JOIN _filtered_curves_2d lut5 ON
  _stats_cpu0.freq_0 = lut5.freq_khz AND
  _stats_cpu5.policy_5 = lut5.other_policy AND
  _stats_cpu5.freq_5 = lut5.other_freq_khz AND
  lut5.idle = 255
LEFT JOIN _filtered_curves_2d lut6 ON
  _stats_cpu0.freq_0 = lut6.freq_khz AND
  _stats_cpu6.policy_6 = lut6.other_policy AND
  _stats_cpu6.freq_6 = lut6.other_freq_khz AND
  lut6.idle = 255
LEFT JOIN _filtered_curves_2d lut7 ON
  _stats_cpu0.freq_0 = lut7.freq_khz AND
  _stats_cpu7.policy_7 = lut7.other_policy AND
  _stats_cpu7.freq_7 = lut7.other_freq_khz AND
  lut7.idle = 255
-- Needs to be at least 1us to reduce inconsequential rows.
WHERE base.dur > time_from_us(1);

-- Find the CPU states creating the max vote
CREATE PERFETTO TABLE _get_max_vote
AS
WITH max_power_tbl AS (
  SELECT
    *,
    -- Indicates if all CPUs are in deep idle
    MIN(
      no_static,
      IFNULL(idle_4, 1),
      IFNULL(idle_5, 1),
      IFNULL(idle_6, 1),
      IFNULL(idle_7, 1)
    ) as all_cpu_deep_idle,
    -- Determines which CPU has highest vote
    MAX(
      static_4,
      static_5,
      static_6,
      static_7
    ) as max_static_vote
  FROM _w_independent_cpus_calc
)
SELECT
  *,
  CASE max_static_vote
    WHEN -1 THEN _get_min_freq_vote()
    WHEN static_4 THEN freq_4
    WHEN static_5 THEN freq_5
    WHEN static_6 THEN freq_6
    WHEN static_7 THEN freq_7
    ELSE 400000
  END max_freq_vote,
  CASE max_static_vote
    WHEN -1 THEN _get_min_policy_vote()
    WHEN static_4 THEN policy_4
    WHEN static_5 THEN policy_5
    WHEN static_6 THEN policy_6
    WHEN static_7 THEN policy_7
    ELSE 4
  END max_policy_vote
FROM max_power_tbl;

-- Final table showing the curves per CPU per slice
CREATE PERFETTO TABLE _system_state_curves
AS
SELECT
  base.ts,
  base.dur,
  COALESCE(lut0.curve_value, cpu0_curve) as cpu0_curve,
  COALESCE(lut1.curve_value, cpu1_curve) as cpu1_curve,
  COALESCE(lut2.curve_value, cpu2_curve) as cpu2_curve,
  COALESCE(lut3.curve_value, cpu3_curve) as cpu3_curve,
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
FROM _get_max_vote as base
-- LUT for 2D dependencies
LEFT JOIN _filtered_curves_2d lut0 ON
  lut0.freq_khz = base.freq_0 AND
  lut0.other_policy = base.max_policy_vote AND
  lut0.other_freq_khz = base.max_freq_vote AND
  lut0.idle = base.idle_0
LEFT JOIN _filtered_curves_2d lut1 ON
  lut1.freq_khz = base.freq_1 AND
  lut1.other_policy = base.max_policy_vote AND
  lut1.other_freq_khz = base.max_freq_vote AND
  lut1.idle = base.idle_1
LEFT JOIN _filtered_curves_2d lut2 ON
  lut2.freq_khz = base.freq_2 AND
  lut2.other_policy = base.max_policy_vote AND
  lut2.other_freq_khz = base.max_freq_vote AND
  lut2.idle = base.idle_2
LEFT JOIN _filtered_curves_2d lut3 ON
  lut3.freq_khz = base.freq_3 AND
  lut3.other_policy = base.max_policy_vote AND
  lut3.other_freq_khz = base.max_freq_vote AND
  lut3.idle = base.idle_3
-- LUT for static curve lookup
LEFT JOIN _filtered_curves_2d static_2d ON
  static_2d.freq_khz = base.freq_0 AND
  static_2d.other_policy = base.max_policy_vote AND
  static_2d.other_freq_khz = base.max_freq_vote AND
  static_2d.idle = 255
LEFT JOIN _filtered_curves_1d static_1d ON
  static_1d.policy = 0 AND
  static_1d.freq_khz = base.freq_0 AND
  static_1d.idle = 255
-- LUT joins for L3 cache
LEFT JOIN _filtered_curves_l3 l3_hit_lut ON
  l3_hit_lut.freq_khz = base.freq_0 AND
  l3_hit_lut.other_policy = base.max_policy_vote AND
  l3_hit_lut.other_freq_khz = base.max_freq_vote AND
  l3_hit_lut.action = 'hit'
LEFT JOIN _filtered_curves_l3 l3_miss_lut ON
  l3_miss_lut.freq_khz = base.freq_0 AND
  l3_miss_lut.other_policy = base.max_policy_vote AND
  l3_miss_lut.other_freq_khz = base.max_freq_vote AND
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

