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
INCLUDE PERFETTO MODULE time.conversion;
INCLUDE PERFETTO MODULE wattson.arm_dsu;
INCLUDE PERFETTO MODULE wattson.cpu_freq_idle;
INCLUDE PERFETTO MODULE wattson.curves.utils;
INCLUDE PERFETTO MODULE wattson.device_infos;

-- Helper macro to do pivot function without policy information
CREATE PERFETTO MACRO _stats_wo_policy_subquery(
  cpu Expr, curve_col ColumnName, freq_col ColumnName, idle_col ColumnName
)
RETURNS TableOrSubquery AS (
  SELECT
    ts,
    dur,
    curve_value as $curve_col,
    freq as $freq_col,
    idle as $idle_col
  FROM _idle_freq_materialized
  WHERE cpu = $cpu
);

-- Helper macro to do pivot function with policy information
CREATE PERFETTO MACRO _stats_w_policy_subquery(
  cpu Expr,
  policy_col ColumnName,
  curve_col ColumnName,
  freq_col ColumnName,
  idle_col ColumnName
)
RETURNS TableOrSubquery AS (
  SELECT
    ts,
    dur,
    policy AS $policy_col,
    curve_value as $curve_col,
    freq as $freq_col,
    idle as $idle_col
  FROM _idle_freq_materialized
  WHERE cpu = $cpu
);

CREATE PERFETTO TABLE _stats_cpu0 AS
SELECT * FROM _stats_wo_policy_subquery!(0, cpu0_curve, freq_0, idle_0);

CREATE PERFETTO TABLE _stats_cpu1 AS
SELECT * FROM _stats_wo_policy_subquery!(1, cpu1_curve, freq_1, idle_1);

CREATE PERFETTO TABLE _stats_cpu2 AS
SELECT * FROM _stats_wo_policy_subquery!(2, cpu2_curve, freq_2, idle_2);

CREATE PERFETTO TABLE _stats_cpu3 AS
SELECT * FROM _stats_wo_policy_subquery!(3, cpu3_curve, freq_3, idle_3);

CREATE PERFETTO TABLE _stats_cpu4 AS
SELECT * FROM _stats_w_policy_subquery!(4, policy_4, cpu4_curve, freq_4, idle_4);

CREATE PERFETTO TABLE _stats_cpu5 AS
SELECT * FROM _stats_w_policy_subquery!(5, policy_5, cpu5_curve, freq_5, idle_5);

CREATE PERFETTO TABLE _stats_cpu6 AS
SELECT * FROM _stats_w_policy_subquery!(6, policy_6, cpu6_curve, freq_6, idle_6);

CREATE PERFETTO TABLE _stats_cpu7 AS
SELECT * FROM _stats_w_policy_subquery!(7, policy_7, cpu7_curve, freq_7, idle_7);

CREATE PERFETTO TABLE _stats_cpu0123 AS
SELECT
  ii.ts,
  ii.dur,
  id_0 as cpu0_id, id_1 as cpu1_id, id_2 as cpu2_id, id_3 as cpu3_id
FROM _interval_intersect!(
  (
    _ii_subquery!(_stats_cpu0),
    _ii_subquery!(_stats_cpu1),
    _ii_subquery!(_stats_cpu2),
    _ii_subquery!(_stats_cpu3)
  ),
  ()
) as ii;

CREATE PERFETTO TABLE _stats_cpu4567 AS
SELECT
  ii.ts,
  ii.dur,
  id_0 as cpu4_id, id_1 as cpu5_id, id_2 as cpu6_id, id_3 as cpu7_id
FROM _interval_intersect!(
  (
    _ii_subquery!(_stats_cpu4),
    _ii_subquery!(_stats_cpu5),
    _ii_subquery!(_stats_cpu6),
    _ii_subquery!(_stats_cpu7)
  ),
  ()
) as ii;

-- SPAN OUTER JOIN because sometimes CPU4/5/6/7 are empty tables
CREATE VIRTUAL TABLE _stats_cpu01234567
USING
  SPAN_OUTER_JOIN(_stats_cpu0123, _stats_cpu4567);

-- get suspend resume state as logged by ftrace.
CREATE PERFETTO TABLE _suspend_slice AS
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
