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

INCLUDE PERFETTO MODULE wattson.cpu.arm_dsu;

INCLUDE PERFETTO MODULE wattson.cpu.pivot;

INCLUDE PERFETTO MODULE wattson.device_infos;

-- Add DSU dependency with ii() if necessary for the device
CREATE PERFETTO TABLE _w_dsu_dependence AS
SELECT
  c.ts,
  c.dur,
  c.freq_0,
  c.idle_0,
  c.freq_1,
  c.idle_1,
  c.freq_2,
  c.idle_2,
  c.freq_3,
  c.idle_3,
  c.freq_4,
  c.idle_4,
  c.freq_5,
  c.idle_5,
  c.freq_6,
  c.idle_6,
  c.freq_7,
  c.idle_7,
  c.cpu0_curve,
  c.cpu1_curve,
  c.cpu2_curve,
  c.cpu3_curve,
  c.cpu4_curve,
  c.cpu5_curve,
  c.cpu6_curve,
  c.cpu7_curve,
  c.l3_hit_count,
  c.l3_miss_count,
  c.no_static,
  c.all_cpu_deep_idle,
  c.static_1d,
  iif(0 IN _cpu_w_dsu_dependency, d.dsu_freq, c.dep_freq_0) AS dep_freq_0,
  iif(0 IN _cpu_w_dsu_dependency, 255, c.dep_policy_0) AS dep_policy_0,
  iif(1 IN _cpu_w_dsu_dependency, d.dsu_freq, c.dep_freq_1) AS dep_freq_1,
  iif(1 IN _cpu_w_dsu_dependency, 255, c.dep_policy_1) AS dep_policy_1,
  iif(2 IN _cpu_w_dsu_dependency, d.dsu_freq, c.dep_freq_2) AS dep_freq_2,
  iif(2 IN _cpu_w_dsu_dependency, 255, c.dep_policy_2) AS dep_policy_2,
  iif(3 IN _cpu_w_dsu_dependency, d.dsu_freq, c.dep_freq_3) AS dep_freq_3,
  iif(3 IN _cpu_w_dsu_dependency, 255, c.dep_policy_3) AS dep_policy_3,
  iif(4 IN _cpu_w_dsu_dependency, d.dsu_freq, c.dep_freq_4) AS dep_freq_4,
  iif(4 IN _cpu_w_dsu_dependency, 255, c.dep_policy_4) AS dep_policy_4,
  iif(5 IN _cpu_w_dsu_dependency, d.dsu_freq, c.dep_freq_5) AS dep_freq_5,
  iif(5 IN _cpu_w_dsu_dependency, 255, c.dep_policy_5) AS dep_policy_5,
  iif(6 IN _cpu_w_dsu_dependency, d.dsu_freq, c.dep_freq_6) AS dep_freq_6,
  iif(6 IN _cpu_w_dsu_dependency, 255, c.dep_policy_6) AS dep_policy_6,
  iif(7 IN _cpu_w_dsu_dependency, d.dsu_freq, c.dep_freq_7) AS dep_freq_7,
  iif(7 IN _cpu_w_dsu_dependency, 255, c.dep_policy_7) AS dep_policy_7
FROM _use_devfreq_for_calc
CROSS JOIN _interval_intersect!(
  (
    _ii_subquery!(_w_dependent_cpus_calc),
    _ii_subquery!(_wattson_dsu_frequency)
  ),
  ()
) AS ii
JOIN _w_dependent_cpus_calc AS c
  ON c._auto_id = id_0
JOIN _wattson_dsu_frequency AS d
  ON d._auto_id = id_1
UNION ALL
-- If no DSU devfreq dependence, just take orginal table as is
SELECT
  ts,
  dur,
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
  cpu0_curve,
  cpu1_curve,
  cpu2_curve,
  cpu3_curve,
  cpu4_curve,
  cpu5_curve,
  cpu6_curve,
  cpu7_curve,
  l3_hit_count,
  l3_miss_count,
  no_static,
  all_cpu_deep_idle,
  static_1d,
  dep_freq_0,
  dep_policy_0,
  dep_freq_1,
  dep_policy_1,
  dep_freq_2,
  dep_policy_2,
  dep_freq_3,
  dep_policy_3,
  dep_freq_4,
  dep_policy_4,
  dep_freq_5,
  dep_policy_5,
  dep_freq_6,
  dep_policy_6,
  dep_freq_7,
  dep_policy_7
FROM _skip_devfreq_for_calc
CROSS JOIN _w_dependent_cpus_calc;
