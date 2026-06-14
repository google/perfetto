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

INCLUDE PERFETTO MODULE wattson.utils;

INCLUDE PERFETTO MODULE wattson.cpu.estimates;

INCLUDE PERFETTO MODULE wattson.device_infos;

INCLUDE PERFETTO MODULE wattson.gpu.estimates;

INCLUDE PERFETTO MODULE wattson.tpu.estimates;

-- The most basic components of Wattson, all normalized to be in mW on a per
-- system state basis.
--
-- The original used SPAN_OUTER_JOIN (not partitioned) because, depending on the
-- trace points enabled, one of the operands may be empty; `INTERVAL UNION OF`
-- co-fragments all three coverages side by side (null where an operand is
-- absent), preserving that behaviour.
CREATE PERFETTO PIPELINE _system_state_mw MATERIALIZED AS
INTERVAL UNION OF (_cpu_estimates_mw AS c, _gpu_estimates_mw AS g, _tpu_estimates_mw AS t)
|> SELECT
     ts,
     dur,
     c.cpu0_mw,
     c.cpu1_mw,
     c.cpu2_mw,
     c.cpu3_mw,
     c.cpu4_mw,
     c.cpu5_mw,
     c.cpu6_mw,
     c.cpu7_mw,
     c.dsu_scu_mw,
     g.gpu_mw,
     t.tpu_mw;

-- ========================================================
-- MACRO: _wattson_base_components_avg_mw
--
-- Low-level macro to calculate base power components average mW.
--
-- Input:
--   window_table: A table with columns (ts, dur, period_id).
--
-- Output:
--   Wide table with CPU policy, average power per core, DSU, and GPU.
-- ========================================================
CREATE PERFETTO MACRO _wattson_base_components_avg_mw(
  window_table TableOrSubquery
)
RETURNS Pipeline
AS (
  SUBPIPELINE windows AS (
    FROM $window_table
    |> SELECT period_id AS id, *
  )
  INTERVAL INTERSECTION OF (windows AS win, _system_state_mw AS ss)
  |> AGGREGATE
    cast_double!(sum(dur * ss.cpu0_mw) / nullif(sum(dur), 0)) AS cpu0_mw,
    cast_double!(sum(dur * ss.cpu1_mw) / nullif(sum(dur), 0)) AS cpu1_mw,
    cast_double!(sum(dur * ss.cpu2_mw) / nullif(sum(dur), 0)) AS cpu2_mw,
    cast_double!(sum(dur * ss.cpu3_mw) / nullif(sum(dur), 0)) AS cpu3_mw,
    cast_double!(sum(dur * ss.cpu4_mw) / nullif(sum(dur), 0)) AS cpu4_mw,
    cast_double!(sum(dur * ss.cpu5_mw) / nullif(sum(dur), 0)) AS cpu5_mw,
    cast_double!(sum(dur * ss.cpu6_mw) / nullif(sum(dur), 0)) AS cpu6_mw,
    cast_double!(sum(dur * ss.cpu7_mw) / nullif(sum(dur), 0)) AS cpu7_mw,
    cast_double!(sum(dur * ss.dsu_scu_mw) / nullif(sum(dur), 0)) AS dsu_scu_mw,
    cast_double!(sum(dur * ss.gpu_mw) / nullif(sum(dur), 0)) AS gpu_mw,
    cast_double!(sum(dur * ss.tpu_mw) / nullif(sum(dur), 0)) AS tpu_mw,
    sum(dur) AS period_dur
    GROUP BY win.id
  |> EXTEND
    win.id AS period_id,
    (
      SELECT
        m.policy
      FROM _dev_cpu_policy_map AS m
      WHERE
        m.cpu = 0
    ) AS cpu0_poli,
    (
      SELECT
        m.policy
      FROM _dev_cpu_policy_map AS m
      WHERE
        m.cpu = 1
    ) AS cpu1_poli,
    (
      SELECT
        m.policy
      FROM _dev_cpu_policy_map AS m
      WHERE
        m.cpu = 2
    ) AS cpu2_poli,
    (
      SELECT
        m.policy
      FROM _dev_cpu_policy_map AS m
      WHERE
        m.cpu = 3
    ) AS cpu3_poli,
    (
      SELECT
        m.policy
      FROM _dev_cpu_policy_map AS m
      WHERE
        m.cpu = 4
    ) AS cpu4_poli,
    (
      SELECT
        m.policy
      FROM _dev_cpu_policy_map AS m
      WHERE
        m.cpu = 5
    ) AS cpu5_poli,
    (
      SELECT
        m.policy
      FROM _dev_cpu_policy_map AS m
      WHERE
        m.cpu = 6
    ) AS cpu6_poli,
    (
      SELECT
        m.policy
      FROM _dev_cpu_policy_map AS m
      WHERE
        m.cpu = 7
    ) AS cpu7_poli
);
