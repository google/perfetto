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

INCLUDE PERFETTO MODULE wattson.device_infos;

INCLUDE PERFETTO MODULE wattson.gpu.estimates;

INCLUDE PERFETTO MODULE wattson.tasks.gpu_tasks;

INCLUDE PERFETTO MODULE wattson.tasks.gpu_active_regions;

INCLUDE PERFETTO MODULE wattson.tasks.task_slices;

INCLUDE PERFETTO MODULE wattson.ui.continuous_estimates;

INCLUDE PERFETTO MODULE wattson.utils;

-- Attribute GPU power to each UID based on its active task count.
-- Formula: attributed_mw = total_gpu_mw / active_tasks
CREATE PERFETTO PIPELINE _gpu_tasks_attribution MATERIALIZED AS
-- First align tasks with total active task count.
SUBPIPELINE tasks_with_total AS (
  INTERVAL INTERSECTION OF (_gpu_tasks AS t, _gpu_active_task_count AS tot)
  |> SELECT
    row_number() OVER (ORDER BY ts) AS id,
    ts,
    dur,
    t.uid,
    t.gpu_id,
    tot.active_tasks
)
-- Then align with Wattson's power estimates.
INTERVAL INTERSECTION OF (tasks_with_total AS ta, _gpu_estimates_mw AS p)
|> SELECT
  ts,
  dur,
  ta.uid AS uid,
  ta.gpu_id AS gpu_id,
  -- Calculate attributed power (mW) proportionally shared
  iif(ta.active_tasks > 0, p.gpu_mw / ta.active_tasks, 0.0) AS estimated_mw;

CREATE PERFETTO PIPELINE _unioned_wattson_estimates_mw MATERIALIZED AS
FROM _system_state_cpu0_mw
|> WHERE EXISTS (SELECT cpu FROM _dev_cpu_policy_map WHERE 0 = cpu)
|> SELECT ts, dur, 0 AS cpu, cpu0_mw AS estimated_mw
|> UNION ALL (
  FROM _system_state_cpu1_mw
  |> WHERE EXISTS (SELECT cpu FROM _dev_cpu_policy_map WHERE 1 = cpu)
  |> SELECT ts, dur, 1 AS cpu, cpu1_mw AS estimated_mw
)
|> UNION ALL (
  FROM _system_state_cpu2_mw
  |> WHERE EXISTS (SELECT cpu FROM _dev_cpu_policy_map WHERE 2 = cpu)
  |> SELECT ts, dur, 2 AS cpu, cpu2_mw AS estimated_mw
)
|> UNION ALL (
  FROM _system_state_cpu3_mw
  |> WHERE EXISTS (SELECT cpu FROM _dev_cpu_policy_map WHERE 3 = cpu)
  |> SELECT ts, dur, 3 AS cpu, cpu3_mw AS estimated_mw
)
|> UNION ALL (
  FROM _system_state_cpu4_mw
  |> WHERE EXISTS (SELECT cpu FROM _dev_cpu_policy_map WHERE 4 = cpu)
  |> SELECT ts, dur, 4 AS cpu, cpu4_mw AS estimated_mw
)
|> UNION ALL (
  FROM _system_state_cpu5_mw
  |> WHERE EXISTS (SELECT cpu FROM _dev_cpu_policy_map WHERE 5 = cpu)
  |> SELECT ts, dur, 5 AS cpu, cpu5_mw AS estimated_mw
)
|> UNION ALL (
  FROM _system_state_cpu6_mw
  |> WHERE EXISTS (SELECT cpu FROM _dev_cpu_policy_map WHERE 6 = cpu)
  |> SELECT ts, dur, 6 AS cpu, cpu6_mw AS estimated_mw
)
|> UNION ALL (
  FROM _system_state_cpu7_mw
  |> WHERE EXISTS (SELECT cpu FROM _dev_cpu_policy_map WHERE 7 = cpu)
  |> SELECT ts, dur, 7 AS cpu, cpu7_mw AS estimated_mw
)
|> UNION ALL (
  FROM _system_state_dsu_scu_mw
  |> SELECT ts, dur, -1 AS cpu, dsu_scu_mw AS estimated_mw
);

CREATE PERFETTO PIPELINE _estimates_w_tasks_attribution MATERIALIZED AS
INTERVAL INTERSECTION OF
  (_unioned_wattson_estimates_mw AS uw, _wattson_task_slices AS s) PER cpu
|> SELECT
  ts,
  dur,
  uw.cpu AS cpu,
  uw.estimated_mw AS estimated_mw,
  s.thread_name,
  s.process_name,
  s.package_name,
  s.tid,
  s.pid,
  s.uid,
  s.utid,
  s.upid;

-- Standalone GPU attribution table
CREATE PERFETTO PIPELINE _gpu_estimates_w_tasks_attribution MATERIALIZED AS
SUBPIPELINE _unique_packages AS (
  FROM package_list
  |> AGGREGATE min(package_name) AS package_name GROUP BY uid
)
SUBPIPELINE _combined AS (
  FROM _gpu_tasks_attribution
  |> SELECT ts, dur, uid, estimated_mw, 0.0 AS idle_mw
  |> UNION ALL (
    FROM _gpu_gap_attribution
    |> SELECT ts, dur, uid, 0.0 AS estimated_mw, estimated_mw AS idle_mw
  )
)
FROM _combined AS g
|> LEFT JOIN _unique_packages AS pkg ON g.uid = pkg.uid
|> SELECT
  g.ts,
  g.dur,
  -2 AS cpu,
  g.uid,
  g.estimated_mw,
  g.idle_mw,
  iif(g.uid = -1, 'GPU Idle', pkg.package_name) AS package_name;

-- List of all physical CPUs that have Wattson estimates
CREATE PERFETTO PIPELINE _wattson_cpus MATERIALIZED AS
FROM _unioned_wattson_estimates_mw
|> WHERE cpu >= 0
|> SELECT DISTINCT cpu;
