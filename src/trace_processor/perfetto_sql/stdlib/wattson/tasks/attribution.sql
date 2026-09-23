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

INCLUDE PERFETTO MODULE intervals.intersect;

INCLUDE PERFETTO MODULE wattson.device_infos;

INCLUDE PERFETTO MODULE wattson.estimates;

INCLUDE PERFETTO MODULE wattson.gpu.estimates;

INCLUDE PERFETTO MODULE wattson.tasks.gpu_tasks;

INCLUDE PERFETTO MODULE wattson.tasks.gpu_active_regions;

INCLUDE PERFETTO MODULE wattson.tasks.task_slices;

INCLUDE PERFETTO MODULE wattson.ui.continuous_estimates;

INCLUDE PERFETTO MODULE wattson.utils;

-- Attribute GPU power to each UID based on its active task count.
-- Formula: attributed_mw = total_gpu_mw / active_tasks
CREATE PERFETTO TABLE _gpu_tasks_attribution AS
SELECT
  ii.ts,
  ii.dur,
  t.uid,
  t.gpu_id,
  -- Calculate attributed power (mW) proportionally shared
  iif(ii.id_1 > 0, p.gpu_mw / ii.id_1, 0.0) AS estimated_mw
FROM _interval_intersect!(
  (
    _ii_subquery!(_gpu_tasks),
    (SELECT active_tasks AS id, ts, dur FROM _gpu_active_task_count),
    _ii_subquery!(_gpu_estimates_mw)
  ),
  ()
) AS ii
JOIN _gpu_tasks AS t
  ON t._auto_id = ii.id_0
JOIN _gpu_estimates_mw AS p
  ON p._auto_id = ii.id_2;

CREATE PERFETTO TABLE _unioned_wattson_estimates_mw AS
SELECT ts, dur, 0 AS cpu, cpu0_mw AS estimated_mw
FROM _system_state_cpu0_mw
WHERE
  EXISTS (SELECT cpu FROM _dev_cpu_policy_map WHERE 0 = cpu)
UNION ALL
SELECT ts, dur, 1 AS cpu, cpu1_mw AS estimated_mw
FROM _system_state_cpu1_mw
WHERE
  EXISTS (SELECT cpu FROM _dev_cpu_policy_map WHERE 1 = cpu)
UNION ALL
SELECT ts, dur, 2 AS cpu, cpu2_mw AS estimated_mw
FROM _system_state_cpu2_mw
WHERE
  EXISTS (SELECT cpu FROM _dev_cpu_policy_map WHERE 2 = cpu)
UNION ALL
SELECT ts, dur, 3 AS cpu, cpu3_mw AS estimated_mw
FROM _system_state_cpu3_mw
WHERE
  EXISTS (SELECT cpu FROM _dev_cpu_policy_map WHERE 3 = cpu)
UNION ALL
SELECT ts, dur, 4 AS cpu, cpu4_mw AS estimated_mw
FROM _system_state_cpu4_mw
WHERE
  EXISTS (SELECT cpu FROM _dev_cpu_policy_map WHERE 4 = cpu)
UNION ALL
SELECT ts, dur, 5 AS cpu, cpu5_mw AS estimated_mw
FROM _system_state_cpu5_mw
WHERE
  EXISTS (SELECT cpu FROM _dev_cpu_policy_map WHERE 5 = cpu)
UNION ALL
SELECT ts, dur, 6 AS cpu, cpu6_mw AS estimated_mw
FROM _system_state_cpu6_mw
WHERE
  EXISTS (SELECT cpu FROM _dev_cpu_policy_map WHERE 6 = cpu)
UNION ALL
SELECT ts, dur, 7 AS cpu, cpu7_mw AS estimated_mw
FROM _system_state_cpu7_mw
WHERE
  EXISTS (SELECT cpu FROM _dev_cpu_policy_map WHERE 7 = cpu);

-- Power estimates split by the task that was running.
--
-- Only utid is carried here; descriptive metadata lives in _wattson_task_metadata
-- and is joined once after aggregation.
CREATE PERFETTO TABLE _estimates_w_tasks_attribution AS
SELECT ii.ts, ii.dur, ii.cpu, uw.estimated_mw, s.utid
FROM _interval_intersect!(
  (
    _ii_subquery!(_unioned_wattson_estimates_mw),
    _ii_subquery!(_wattson_task_slices)
  ),
  (cpu)
) AS ii
JOIN _unioned_wattson_estimates_mw AS uw
  ON uw._auto_id = id_0
JOIN _wattson_task_slices AS s
  ON s._auto_id = id_1;

-- Standalone GPU attribution table
CREATE PERFETTO TABLE _gpu_estimates_w_tasks_attribution AS
WITH
  _unique_packages AS (
    SELECT uid, min(package_name) AS package_name FROM package_list GROUP BY uid
  ),
  _combined AS (
    SELECT ts, dur, uid, estimated_mw, 0.0 AS idle_mw
    FROM _gpu_tasks_attribution
    UNION ALL
    SELECT ts, dur, uid, 0.0 AS estimated_mw, estimated_mw AS idle_mw
    FROM _gpu_gap_attribution
  )
SELECT
  g.ts,
  g.dur,
  -2 AS cpu,
  g.uid,
  g.estimated_mw,
  g.idle_mw,
  iif(g.uid = -1, 'GPU Idle', pkg.package_name) AS package_name
FROM _combined AS g
LEFT JOIN _unique_packages AS pkg
  ON g.uid = pkg.uid;

-- List of all physical CPUs that have Wattson estimates
CREATE PERFETTO TABLE _wattson_cpus AS
SELECT DISTINCT cpu
FROM _dev_cpu_policy_map
WHERE
  EXISTS (SELECT 1 FROM _system_state_mw);
