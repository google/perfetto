--
-- Copyright 2026 The Android Open Source Project
--
-- Licensed under the Apache License, Version 2.0 (the "License");
-- you may not use this file except in compliance with the License.
-- You may obtain a copy of the License at
--
--     http://www.apache.org/licenses/LICENSE-2.0
--
-- Unless required by applicable law or agreed to in writing, software
-- distributed under the License is distributed on an "AS IS" BASIS,
-- WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
-- See the License for the specific language governing permissions and
-- limitations under the License.

INCLUDE PERFETTO MODULE intervals.intersect;

INCLUDE PERFETTO MODULE wattson.cpu.idle;

INCLUDE PERFETTO MODULE wattson.estimates;

INCLUDE PERFETTO MODULE wattson.tasks.attribution;

INCLUDE PERFETTO MODULE wattson.tasks.idle_transitions_attribution;

INCLUDE PERFETTO MODULE wattson.utils;

-- ========================================================
-- MACRO: _wattson_threads_aggregation
--
-- Low-level macro to calculate energy and power attribution per thread/process.
-- ========================================================
CREATE PERFETTO MACRO _wattson_threads_aggregation(
  tasks_table TableOrSubquery,
  window_table TableOrSubquery,
  cpus_table TableOrSubquery
)
RETURNS Pipeline
AS (
  SUBPIPELINE active_summary AS (
    FROM $tasks_table
    |> AGGREGATE
      sum(estimated_mw * dur) / 1e9 AS active_mws,
      sum(estimated_mw * dur) AS total_mw_ns
      GROUP BY
        period_id,
        utid,
        thread_name,
        process_name,
        package_name,
        tid,
        pid,
        upid,
        uid
  )
  -- Idle cost per (period, thread), filtered to the requested CPUs.
  SUBPIPELINE idle_summary AS (
    FROM $window_table AS w
    |> JOIN _filter_idle_attribution(w.ts, w.dur) AS cost
    |> WHERE
      cost.cpu IN (
        SELECT
          cpu
        FROM $cpus_table
      )
    |> AGGREGATE
      sum(cost.idle_cost_mws) AS idle_mws
      GROUP BY w.period_id, cost.utid
  )
  FROM active_summary AS a
  |> JOIN $window_table AS w
    ON a.period_id = w.period_id
  |> LEFT JOIN idle_summary AS i
    ON a.period_id = i.period_id AND a.utid = i.utid
  |> SELECT
    a.period_id,
    w.dur AS period_dur,
    a.utid,
    a.tid,
    a.pid,
    a.upid,
    a.uid,
    coalesce(a.thread_name, 'Thread ' || a.tid) AS thread_name,
    coalesce(a.process_name, '') AS process_name,
    coalesce(a.package_name, '') AS package_name,
    a.active_mws AS estimated_mws,
    -- Fixed power calculation: divide by the specific period duration
    a.total_mw_ns / w.dur AS estimated_mw,
    coalesce(i.idle_mws, 0) AS idle_transitions_mws,
    (
      a.active_mws + coalesce(i.idle_mws, 0)
    ) AS total_mws
);

-- ========================================================
-- MACRO: wattson_threads_aggregation
--
-- Calculates energy and power attribution per thread/process for the
-- given time windows.
--
-- Input:
--   window_table: A table with columns (ts, dur, period_id).
--
-- Output:
--   Flat table with columns:
--     period_id, period_dur, utid, tid, pid,
--     thread_name, process_name,
--     estimated_mws, estimated_mw, idle_transitions_mws, total_mws
-- ========================================================
CREATE PERFETTO MACRO wattson_threads_aggregation(
  -- Intereseted window table with columns:
  -- (ts, dur, period_id).
  window_table TableOrSubquery
)
RETURNS Pipeline
AS (
  -- Per-CPU intersection of the per-task attribution stream with the
  -- caller-supplied window relation, carrying each window's period_id.
  SUBPIPELINE windowed_active_state AS (
    INTERVAL INTERSECTION OF (
      $window_table AS w,
      _estimates_w_tasks_attribution AS tasks
    ) PER cpu
    |> SELECT
      -- The clipped intersection duration (the task's time within this window),
      -- not tasks.dur — a task spanning multiple windows must not double-count.
      dur,
      w.period_id AS period_id,
      tasks.estimated_mw,
      tasks.thread_name,
      tasks.process_name,
      tasks.tid,
      tasks.pid,
      tasks.upid,
      tasks.uid,
      tasks.package_name,
      tasks.utid
  )
  _wattson_threads_aggregation!(
    windowed_active_state,
    $window_table,
    _wattson_cpus
  )
  |> SELECT
    period_id,
    period_dur,
    utid,
    tid,
    pid,
    thread_name,
    process_name,
    package_name,
    estimated_mws,
    estimated_mw,
    idle_transitions_mws,
    total_mws
);

-- ========================================================
-- MACRO: wattson_rails_aggregation
--
-- Flattening and unpivoting of rail data into a standard breakdown.
--
-- Input:
--   window_table: A table with columns (ts, dur, period_id).
--
-- Output:
--   Flat breakdown including CORE, POLICY, DSU and SUBSYSTEM TOTAL.
-- ========================================================
CREATE PERFETTO MACRO wattson_rails_aggregation(
  -- Intereseted window table with columns:
  -- (ts, dur, period_id).
  window_table TableOrSubquery
)
RETURNS Pipeline
AS (
  -- 1. Cache base components (wide, per-period average mW).
  SUBPIPELINE base_components AS (
    FROM _wattson_base_components_avg_mw!($window_table)
  )
  -- 2. Unpivot CPU columns. §4 wide-pivot: one column per CPU into long form
  --    via UNION ALL of scalar SELECTs (SQLite has no UNPIVOT).
  SUBPIPELINE cpu_unpivoted AS (
    FROM base_components
    |> WHERE cpu0_mw IS NOT NULL
    |> SELECT period_id, period_dur, 0 AS cpu_id, cpu0_poli AS policy_id, cpu0_mw AS mw
    |> UNION ALL (
      FROM base_components
      |> WHERE cpu1_mw IS NOT NULL
      |> SELECT period_id, period_dur, 1, cpu1_poli, cpu1_mw
    )
    |> UNION ALL (
      FROM base_components
      |> WHERE cpu2_mw IS NOT NULL
      |> SELECT period_id, period_dur, 2, cpu2_poli, cpu2_mw
    )
    |> UNION ALL (
      FROM base_components
      |> WHERE cpu3_mw IS NOT NULL
      |> SELECT period_id, period_dur, 3, cpu3_poli, cpu3_mw
    )
    |> UNION ALL (
      FROM base_components
      |> WHERE cpu4_mw IS NOT NULL
      |> SELECT period_id, period_dur, 4, cpu4_poli, cpu4_mw
    )
    |> UNION ALL (
      FROM base_components
      |> WHERE cpu5_mw IS NOT NULL
      |> SELECT period_id, period_dur, 5, cpu5_poli, cpu5_mw
    )
    |> UNION ALL (
      FROM base_components
      |> WHERE cpu6_mw IS NOT NULL
      |> SELECT period_id, period_dur, 6, cpu6_poli, cpu6_mw
    )
    |> UNION ALL (
      FROM base_components
      |> WHERE cpu7_mw IS NOT NULL
      |> SELECT period_id, period_dur, 7, cpu7_poli, cpu7_mw
    )
  )
  -- 3. Build basic Flat View (reused for the Total calculation).
  SUBPIPELINE flat_view_raw AS (
    -- A. CPU Cores
    FROM cpu_unpivoted AS c
    |> SELECT
      c.period_id,
      c.period_dur,
      'CPU' AS subsystem,
      'CORE' AS breakdown_type,
      c.cpu_id AS component_id,
      c.policy_id AS parent_id,
      c.mw AS estimated_mw,
      (
        c.mw * c.period_dur / 1e9
      ) AS estimated_mws
    -- B. CPU Policies
    |> UNION ALL (
      FROM cpu_unpivoted AS c
      |> AGGREGATE
        sum(c.mw) AS estimated_mw,
        sum(c.mw * c.period_dur / 1e9) AS estimated_mws
        GROUP BY c.period_id, c.period_dur, c.policy_id
      |> SELECT
        period_id,
        period_dur,
        'CPU' AS subsystem,
        'POLICY' AS breakdown_type,
        policy_id AS component_id,
        NULL AS parent_id,
        estimated_mw,
        estimated_mws
    )
    -- C. DSU/SCU
    |> UNION ALL (
      FROM base_components AS base
      |> WHERE base.dsu_scu_mw IS NOT NULL
      |> SELECT
        base.period_id,
        base.period_dur,
        'CPU' AS subsystem,
        'DSU' AS breakdown_type,
        NULL AS component_id,
        NULL AS parent_id,
        base.dsu_scu_mw AS estimated_mw,
        (
          base.dsu_scu_mw * base.period_dur / 1e9
        ) AS estimated_mws
    )
    -- D. GPU Subsystem
    |> UNION ALL (
      FROM base_components AS base
      |> WHERE base.gpu_mw IS NOT NULL
      |> SELECT
        base.period_id,
        base.period_dur,
        'GPU' AS subsystem,
        'TOTAL' AS breakdown_type,
        NULL AS component_id,
        NULL AS parent_id,
        base.gpu_mw AS estimated_mw,
        (
          base.gpu_mw * base.period_dur / 1e9
        ) AS estimated_mws
    )
    -- E. TPU Subsystem
    |> UNION ALL (
      FROM base_components AS base
      |> WHERE base.tpu_mw IS NOT NULL
      |> SELECT
        base.period_id,
        base.period_dur,
        'TPU' AS subsystem,
        'TOTAL' AS breakdown_type,
        NULL AS component_id,
        NULL AS parent_id,
        base.tpu_mw AS estimated_mw,
        (
          base.tpu_mw * base.period_dur / 1e9
        ) AS estimated_mws
    )
  )
  -- 4. Final output: Raw Data + Computed CPU Total.
  FROM flat_view_raw
  |> UNION ALL (
    -- CPU TOTAL (Auto-calculated)
    -- Sum only Policy and DSU (exclude Cores to avoid double counting)
    FROM flat_view_raw
    |> WHERE
      subsystem = 'CPU' AND breakdown_type IN ('POLICY', 'DSU')
    |> AGGREGATE
      sum(estimated_mw) AS estimated_mw,
      sum(estimated_mws) AS estimated_mws
      GROUP BY period_id, period_dur, subsystem
    |> SELECT
      period_id,
      period_dur,
      subsystem,
      'TOTAL' AS breakdown_type,
      NULL AS component_id,
      NULL AS parent_id,
      estimated_mw,
      estimated_mws
  )
);

-- ========================================================
-- VIEW: _wattson_metric_metadata
--
-- Shared metadata for all Wattson metrics.
-- ========================================================
CREATE PERFETTO PIPELINE wattson_metric_metadata(
  -- Wattson metric version
  metric_version LONG,
  -- Wattson power curve version
  power_model_version LONG,
  -- Wattson estimation will be crude
  -- if missing cpu/idle counter
  is_crude_estimate BOOL
) AS
FROM (VALUES (1)) AS _one(x)
|> SELECT
  4 AS metric_version,
  1 AS power_model_version,
  CAST(NOT EXISTS (SELECT 1 FROM _wattson_cpuidle_counters_exist) AS INTEGER) AS is_crude_estimate;
