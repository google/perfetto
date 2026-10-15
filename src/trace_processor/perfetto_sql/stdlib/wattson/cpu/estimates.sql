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

INCLUDE PERFETTO MODULE wattson.cpu.pivot;

INCLUDE PERFETTO MODULE wattson.curves.utils;

INCLUDE PERFETTO MODULE wattson.device_infos;

INCLUDE PERFETTO MODULE wattson.utils;

-- Bit shifts for _curve_2d_key
CREATE PERFETTO MACRO _curve_2d_freq_shift()
RETURNS Expr
AS 36;

CREATE PERFETTO MACRO _curve_2d_dep_freq_shift()
RETURNS Expr
AS 12;

CREATE PERFETTO MACRO _curve_2d_dep_policy_shift()
RETURNS Expr
AS 4;

-- Packs (freq_khz, dep_freq, dep_policy, idle) into a 64-bit integer key for
-- single-column index lookups into _curves_2d_lut.
CREATE PERFETTO MACRO _curve_2d_key(
  freq Expr,
  dep_freq Expr,
  dep_policy Expr,
  idle Expr
)
RETURNS Expr
AS (
  (($freq) << _curve_2d_freq_shift!())
  | (($dep_freq) << _curve_2d_dep_freq_shift!())
  | (($dep_policy) << _curve_2d_dep_policy_shift!())
  | _encode_idle!($idle)
);

-- Pre-joins the 2D CPU curves (~450 rows) with L3 and interconnect curves and
-- indexes them by a single 64-bit integer key. This replaces ten multi-column
-- virtual-table index lookups per unique config with eight single-column
-- integer index lookups.
CREATE PERFETTO TABLE _curves_2d_lut AS
SELECT
  _curve_2d_key!(c2d.freq_khz, c2d.dep_freq, c2d.dep_policy, c2d.idle) AS key_2d,
  c2d.static,
  c2d.curve_value,
  l3.l3_hit,
  l3.l3_miss,
  ic.curve_value AS interconnect
FROM _filtered_curves_2d AS c2d
LEFT JOIN _filtered_curves_l3 AS l3
  ON l3.freq_khz = c2d.freq_khz
  AND l3.dep_policy = c2d.dep_policy
  AND l3.dep_freq = c2d.dep_freq
LEFT JOIN _filtered_curves_interconnect AS ic
  ON ic.freq_khz = c2d.freq_khz
  AND ic.dep_policy = c2d.dep_policy
  AND ic.dep_freq = c2d.dep_freq
  AND ic.policy = 0;

CREATE PERFETTO INDEX _curves_2d_lut_idx ON _curves_2d_lut(key_2d);

-- `suspended` is part of `config_hash`, so a config is either wholly
-- suspended or wholly not. That means the suspended-to-zero rule can be
-- applied here, once per config, instead of once per interval downstream.
CREATE PERFETTO TABLE _unique_estimates_mw AS
WITH
  flags AS MATERIALIZED (
    SELECT
      EXISTS (SELECT 1 FROM _cpu_w_dependency_default_vote WHERE cpu = 0) AS c0,
      EXISTS (SELECT 1 FROM _cpu_w_dependency_default_vote WHERE cpu = 1) AS c1,
      EXISTS (SELECT 1 FROM _cpu_w_dependency_default_vote WHERE cpu = 2) AS c2,
      EXISTS (SELECT 1 FROM _cpu_w_dependency_default_vote WHERE cpu = 3) AS c3,
      EXISTS (SELECT 1 FROM _cpu_w_dependency_default_vote WHERE cpu = 4) AS c4,
      EXISTS (SELECT 1 FROM _cpu_w_dependency_default_vote WHERE cpu = 5) AS c5,
      EXISTS (SELECT 1 FROM _cpu_w_dependency_default_vote WHERE cpu = 6) AS c6,
      EXISTS (SELECT 1 FROM _cpu_w_dependency_default_vote WHERE cpu = 7) AS c7,
      EXISTS (SELECT 1 FROM _filtered_curves_dsu_1d) AS has_dsu_1d
  )
SELECT
  base.config_hash,
  iif(base.suspended, 0, coalesce(base.cpu0_curve, lut0.curve_value)) AS cpu0_mw,
  iif(base.suspended, 0, coalesce(base.cpu1_curve, lut1.curve_value)) AS cpu1_mw,
  iif(base.suspended, 0, coalesce(base.cpu2_curve, lut2.curve_value)) AS cpu2_mw,
  iif(base.suspended, 0, coalesce(base.cpu3_curve, lut3.curve_value)) AS cpu3_mw,
  iif(base.suspended, 0, coalesce(base.cpu4_curve, lut4.curve_value)) AS cpu4_mw,
  iif(base.suspended, 0, coalesce(base.cpu5_curve, lut5.curve_value)) AS cpu5_mw,
  iif(base.suspended, 0, coalesce(base.cpu6_curve, lut6.curve_value)) AS cpu6_mw,
  iif(base.suspended, 0, coalesce(base.cpu7_curve, lut7.curve_value)) AS cpu7_mw,
  iif(
    base.suspended,
    0,
    iif(
      0 IN _device_policies
      AND _extract_bit!(policy_cpus_on_mask, 0),
      coalesce(lut0.static, 0),
      0
    )
    + iif(
      1 IN _device_policies
      AND _extract_bit!(policy_cpus_on_mask, 1),
      coalesce(lut1.static, 0),
      0
    )
    + iif(
      2 IN _device_policies
      AND _extract_bit!(policy_cpus_on_mask, 2),
      coalesce(lut2.static, 0),
      0
    )
    + iif(
      3 IN _device_policies
      AND _extract_bit!(policy_cpus_on_mask, 3),
      coalesce(lut3.static, 0),
      0
    )
    + iif(
      4 IN _device_policies
      AND _extract_bit!(policy_cpus_on_mask, 4),
      coalesce(lut4.static, 0),
      0
    )
    + iif(
      5 IN _device_policies
      AND _extract_bit!(policy_cpus_on_mask, 5),
      coalesce(lut5.static, 0),
      0
    )
    + iif(
      6 IN _device_policies
      AND _extract_bit!(policy_cpus_on_mask, 6),
      coalesce(lut6.static, 0),
      0
    )
    + iif(
      7 IN _device_policies
      AND _extract_bit!(policy_cpus_on_mask, 7),
      coalesce(lut7.static, 0),
      0
    )
    + static_1d
  ) AS static_mw,
  iif(base.suspended, 0, lut0.l3_hit) AS l3_hit,
  iif(base.suspended, 0, lut0.l3_miss) AS l3_miss,
  iif(base.suspended, 0, coalesce(dsu_1d_lut.power, lut0.interconnect)) AS interconnect_mw
FROM flags
CROSS JOIN _w_dependent_cpus_unique AS base
LEFT JOIN _curves_2d_lut AS lut0
  ON flags.c0
  AND lut0.key_2d
  = _curve_2d_key!(base.freq_0, base.dep_freq_0, base.dep_policy_0, base.idle_0)
LEFT JOIN _curves_2d_lut AS lut1
  ON flags.c1
  AND lut1.key_2d
  = _curve_2d_key!(base.freq_1, base.dep_freq_1, base.dep_policy_1, base.idle_1)
LEFT JOIN _curves_2d_lut AS lut2
  ON flags.c2
  AND lut2.key_2d
  = _curve_2d_key!(base.freq_2, base.dep_freq_2, base.dep_policy_2, base.idle_2)
LEFT JOIN _curves_2d_lut AS lut3
  ON flags.c3
  AND lut3.key_2d
  = _curve_2d_key!(base.freq_3, base.dep_freq_3, base.dep_policy_3, base.idle_3)
LEFT JOIN _curves_2d_lut AS lut4
  ON flags.c4
  AND lut4.key_2d
  = _curve_2d_key!(base.freq_4, base.dep_freq_4, base.dep_policy_4, base.idle_4)
LEFT JOIN _curves_2d_lut AS lut5
  ON flags.c5
  AND lut5.key_2d
  = _curve_2d_key!(base.freq_5, base.dep_freq_5, base.dep_policy_5, base.idle_5)
LEFT JOIN _curves_2d_lut AS lut6
  ON flags.c6
  AND lut6.key_2d
  = _curve_2d_key!(base.freq_6, base.dep_freq_6, base.dep_policy_6, base.idle_6)
LEFT JOIN _curves_2d_lut AS lut7
  ON flags.c7
  AND lut7.key_2d
  = _curve_2d_key!(base.freq_7, base.dep_freq_7, base.dep_policy_7, base.idle_7)
LEFT JOIN _filtered_curves_dsu_1d AS dsu_1d_lut
  ON flags.has_dsu_1d
  AND dsu_1d_lut.dsu_freq = base.dsu_freq;

-- The most basic components of Wattson, all normalized to be in mW on a per
-- system state basis.
--
-- Nothing here has to test `suspended`: _unique_estimates_mw has already
-- zeroed every term of a suspended config, so the sum below comes out at zero
-- on its own. That check used to run nine times per interval.
CREATE PERFETTO INDEX _unique_estimates_mw_idx ON _unique_estimates_mw(
  config_hash
);

CREATE PERFETTO VIEW _cpu_estimates_mw AS
SELECT
  slices.ts,
  slices.dur,
  base.cpu0_mw,
  base.cpu1_mw,
  base.cpu2_mw,
  base.cpu3_mw,
  base.cpu4_mw,
  base.cpu5_mw,
  base.cpu6_mw,
  base.cpu7_mw,
  base.static_mw
  + (coalesce(slices.l3_hit_count * base.l3_hit, 0)
  + coalesce(slices.l3_miss_count * base.l3_miss, 0))
  * 1000
  / slices.dur
  + coalesce(base.interconnect_mw, 0) AS dsu_scu_mw
FROM _w_cpu_slices AS slices
JOIN _unique_estimates_mw AS base USING (config_hash)
WHERE
  slices.dur > 0;
