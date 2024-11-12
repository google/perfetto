
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

INCLUDE PERFETTO MODULE wattson.curves.estimates;
INCLUDE PERFETTO MODULE wattson.curves.idle_attribution;
INCLUDE PERFETTO MODULE viz.summary.threads_w_processes;

-- Take only the Wattson estimations that are in the window of interest
DROP VIEW IF EXISTS _windowed_wattson;
CREATE PERFETTO VIEW _windowed_wattson AS
SELECT
  ii.ts,
  ii.dur,
  ii.id_1 as period_id,
  ss.cpu0_mw,
  ss.cpu1_mw,
  ss.cpu2_mw,
  ss.cpu3_mw,
  ss.cpu4_mw,
  ss.cpu5_mw,
  ss.cpu6_mw,
  ss.cpu7_mw,
  ss.dsu_scu_mw
FROM _interval_intersect!(
  (
    _ii_subquery!(_system_state_mw),
    (SELECT ts, dur, period_id as id FROM {{window_table}})
  ),
  ()
) ii
JOIN _system_state_mw AS ss ON ss._auto_id = id_0;

-- "Unpivot" the table so that table can by PARTITIONED BY cpu
DROP TABLE IF EXISTS _unioned_windowed_wattson;
CREATE PERFETTO TABLE _unioned_windowed_wattson AS
  SELECT ts, dur, 0 as cpu, cpu0_mw as estimated_mw, period_id
  FROM _windowed_wattson
  WHERE EXISTS (SELECT cpu FROM _dev_cpu_policy_map WHERE 0 = cpu)
  UNION ALL
  SELECT ts, dur, 1 as cpu, cpu1_mw as estimated_mw, period_id
  FROM _windowed_wattson
  WHERE EXISTS (SELECT cpu FROM _dev_cpu_policy_map WHERE 1 = cpu)
  UNION ALL
  SELECT ts, dur, 2 as cpu, cpu2_mw as estimated_mw, period_id
  FROM _windowed_wattson
  WHERE EXISTS (SELECT cpu FROM _dev_cpu_policy_map WHERE 2 = cpu)
  UNION ALL
  SELECT ts, dur, 3 as cpu, cpu3_mw as estimated_mw, period_id
  FROM _windowed_wattson
  WHERE EXISTS (SELECT cpu FROM _dev_cpu_policy_map WHERE 3 = cpu)
  UNION ALL
  SELECT ts, dur, 4 as cpu, cpu4_mw as estimated_mw, period_id
  FROM _windowed_wattson
  WHERE EXISTS (SELECT cpu FROM _dev_cpu_policy_map WHERE 4 = cpu)
  UNION ALL
  SELECT ts, dur, 5 as cpu, cpu5_mw as estimated_mw, period_id
  FROM _windowed_wattson
  WHERE EXISTS (SELECT cpu FROM _dev_cpu_policy_map WHERE 5 = cpu)
  UNION ALL
  SELECT ts, dur, 6 as cpu, cpu6_mw as estimated_mw, period_id
  FROM _windowed_wattson
  WHERE EXISTS (SELECT cpu FROM _dev_cpu_policy_map WHERE 6 = cpu)
  UNION ALL
  SELECT ts, dur, 7 as cpu, cpu7_mw as estimated_mw, period_id
  FROM _windowed_wattson
  WHERE EXISTS (SELECT cpu FROM _dev_cpu_policy_map WHERE 7 = cpu)
  UNION ALL
  SELECT ts, dur, -1 as cpu, dsu_scu_mw as estimated_mw, period_id
  FROM _windowed_wattson;

DROP TABLE IF EXISTS _windowed_threads_system_state;
CREATE PERFETTO TABLE _windowed_threads_system_state AS
SELECT
  ii.ts,
  ii.dur,
  ii.cpu,
  uw.estimated_mw,
  s.thread_name,
  s.process_name,
  s.tid,
  s.pid,
  s.utid,
  uw.period_id
FROM _interval_intersect!(
  (
    _ii_subquery!(_unioned_windowed_wattson),
    _ii_subquery!(_sched_w_thread_process_package_summary)
  ),
  (cpu)
) ii
JOIN _unioned_windowed_wattson AS uw ON uw._auto_id = id_0
JOIN _sched_w_thread_process_package_summary AS s ON s._auto_id = id_1;

-- Get idle overhead attribution per thread
DROP VIEW IF EXISTS _per_thread_idle_attribution;
CREATE PERFETTO VIEW _per_thread_idle_attribution AS
SELECT
  SUM(cost.estimated_mw * cost.dur) / 1e9 as idle_cost_mws,
  cost.utid,
  ii.id_1 as period_id
FROM _interval_intersect!(
  (
    _ii_subquery!(_idle_transition_cost),
    (SELECT ts, dur, period_id as id FROM {{window_table}})
  ),
  ()
) ii
JOIN _idle_transition_cost as cost ON cost._auto_id = id_0
GROUP BY utid, period_id;

-- Group by unique thread ID and disregard CPUs, summing of power over all CPUs
-- and all instances of the thread
DROP VIEW IF EXISTS _wattson_thread_attribution;
CREATE PERFETTO VIEW _wattson_thread_attribution AS
SELECT
  -- active time of thread divided by total time where Wattson is defined
  SUM(estimated_mw * dur) / 1000000000 as estimated_mws,
  (
    SUM(estimated_mw * dur) / (SELECT SUM(dur) from _windowed_wattson)
  ) as estimated_mw,
  idle_cost_mws,
  thread_name,
  process_name,
  tid,
  pid,
  period_id
FROM _windowed_threads_system_state
LEFT JOIN _per_thread_idle_attribution USING (utid, period_id)
GROUP BY utid, period_id
ORDER BY estimated_mw DESC;

-- Create proto format task attribution for each period
DROP VIEW IF EXISTS _wattson_per_task;
CREATE PERFETTO VIEW _wattson_per_task AS
SELECT
  period_id,
  (
    SELECT RepeatedField(
      AndroidWattsonTaskInfo(
        'estimated_mws', ROUND(estimated_mws, 6),
        'estimated_mw', ROUND(estimated_mw, 6),
        'idle_transitions_mws', ROUND(idle_cost_mws, 6),
        'thread_name', thread_name,
        'process_name', process_name,
        'thread_id', tid,
        'process_id', pid
      )
    )
  ) as proto
FROM _wattson_thread_attribution
GROUP BY period_id;

