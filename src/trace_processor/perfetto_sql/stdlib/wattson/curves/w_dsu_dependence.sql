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
INCLUDE PERFETTO MODULE linux.devfreq;
INCLUDE PERFETTO MODULE wattson.cpu_split;
INCLUDE PERFETTO MODULE wattson.curves.utils;
INCLUDE PERFETTO MODULE wattson.device_infos;

CREATE PERFETTO TABLE _cpu_curves AS
SELECT
  ts, dur,
  freq_0, idle_0,
  freq_1, idle_1,
  freq_2, idle_2,
  freq_3, idle_3,
  lut4.curve_value as cpu4_curve,
  lut5.curve_value as cpu5_curve,
  lut6.curve_value as cpu6_curve,
  lut7.curve_value as cpu7_curve,
  l3_hit_count, l3_miss_count,
  no_static,
  MIN(
    no_static,
    IFNULL(idle_4, 1),
    IFNULL(idle_5, 1),
    IFNULL(idle_6, 1),
    IFNULL(idle_7, 1)
  ) as all_cpu_deep_idle
FROM _w_independent_cpus_calc as base
-- _use_devfreq_for_calc short circuits this table if devfreq isn't needed
JOIN _use_devfreq_for_calc
LEFT JOIN _filtered_curves_1d lut4 ON
  base.freq_4 = lut4.freq_khz AND
  base.idle_4 = lut4.idle
LEFT JOIN _filtered_curves_1d lut5 ON
  base.freq_5 = lut5.freq_khz AND
  base.idle_5 = lut5.idle
LEFT JOIN _filtered_curves_1d lut6 ON
  base.freq_6 = lut6.freq_khz AND
  base.idle_6 = lut6.idle
LEFT JOIN _filtered_curves_1d lut7 ON
  base.freq_7 = lut7.freq_khz AND
  base.idle_7 = lut7.idle;

CREATE PERFETTO TABLE _w_dsu_dependence AS
SELECT
  c.ts, c.dur,
  c.freq_0, c.idle_0,
  c.freq_1, c.idle_1,
  c.freq_2, c.idle_2,
  c.freq_3, c.idle_3,
  -- NULL columns needed to match columns of _get_max_vote before UNION
  NULL as cpu0_curve,
  NULL as cpu1_curve,
  NULL as cpu2_curve,
  NULL as cpu3_curve,
  c.cpu4_curve,
  c.cpu5_curve,
  c.cpu6_curve,
  c.cpu7_curve,
  c.l3_hit_count,
  c.l3_miss_count,
  c.no_static,
  c.all_cpu_deep_idle,
  d.dsu_freq as dependent_freq,
  255 as dependent_policy
FROM _interval_intersect!(
  (
    _ii_subquery!(_cpu_curves),
    _ii_subquery!(linux_devfreq_dsu_counter)
  ),
  ()
) ii
JOIN _cpu_curves AS c ON c._auto_id = id_0
JOIN linux_devfreq_dsu_counter AS d on d._auto_id = id_1;

