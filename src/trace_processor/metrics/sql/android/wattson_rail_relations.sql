
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

-- This file established the tables that define the relationships between rails
-- and subrails as well as the hierarchical power estimates of each rail

INCLUDE PERFETTO MODULE wattson.curves.ungrouped;

-- Take only the Wattson estimations that are in the window of interest
DROP TABLE IF EXISTS _windowed_wattson;
CREATE VIRTUAL TABLE _windowed_wattson
USING
  SPAN_JOIN({{window_table}}, _system_state_mw);

-- The most basic rail components that form the "building blocks" from which all
-- other rails and components are derived
DROP VIEW IF EXISTS _wattson_base_components_pws;
CREATE PERFETTO VIEW _wattson_base_components_pws AS
SELECT
  -- *_curve is in units of mW, so multiplying by ns gives pWs
  SUM(dur * cpu0_mw) as cpu0_pws,
  SUM(dur * cpu1_mw) as cpu1_pws,
  SUM(dur * cpu2_mw) as cpu2_pws,
  SUM(dur * cpu3_mw) as cpu3_pws,
  SUM(dur * cpu4_mw) as cpu4_pws,
  SUM(dur * cpu5_mw) as cpu5_pws,
  SUM(dur * cpu6_mw) as cpu6_pws,
  SUM(dur * cpu7_mw) as cpu7_pws,
  SUM(dur * dsu_scu_mw) as dsu_scu_pws,
  SUM(dur) as total_dur,
  period_id
FROM _windowed_wattson
GROUP BY period_id;

-- Root node that has all possible components for the CPUSS added together
DROP VIEW IF EXISTS _wattson_cpu_rail_total;
CREATE PERFETTO VIEW _wattson_cpu_rail_total AS
SELECT
  period_id,
  total_dur,
  'cpu_subsystem' as name,
  (
    cpu0_pws + cpu1_pws + cpu2_pws + cpu3_pws +
    cpu4_pws + cpu5_pws + cpu6_pws + cpu7_pws +
    dsu_scu_pws
  ) / total_dur as estimate_mw
FROM _wattson_base_components_pws
GROUP BY period_id;

-- Sub-sub-rail, meaning two levels down from the CPU root node
DROP VIEW IF EXISTS _wattson_cpu_subsubrail_grouped;
CREATE PERFETTO VIEW _wattson_cpu_subsubrail_grouped AS
SELECT
  period_id,
  'cpu0' as name,
  map.policy,
  cpu0_pws / total_dur as estimate_mw
FROM _wattson_base_components_pws
CROSS JOIN _dev_cpu_policy_map as map WHERE map.cpu = 0
GROUP BY period_id
UNION ALL
SELECT
  period_id,
  'cpu1' as name,
  map.policy,
  cpu1_pws / total_dur as estimate_mw
FROM _wattson_base_components_pws
CROSS JOIN _dev_cpu_policy_map as map WHERE map.cpu = 1
GROUP BY period_id
UNION ALL
SELECT
  period_id,
  'cpu2' as name,
  map.policy,
  cpu2_pws / total_dur as estimate_mw
FROM _wattson_base_components_pws
CROSS JOIN _dev_cpu_policy_map as map WHERE map.cpu = 2
GROUP BY period_id
UNION ALL
SELECT
  period_id,
  'cpu3' as name,
  map.policy,
  cpu3_pws / total_dur as estimate_mw
FROM _wattson_base_components_pws
CROSS JOIN _dev_cpu_policy_map as map WHERE map.cpu = 3
GROUP BY period_id
UNION ALL
SELECT
  period_id,
  'cpu4' as name,
  map.policy,
  cpu4_pws / total_dur as estimate_mw
FROM _wattson_base_components_pws
CROSS JOIN _dev_cpu_policy_map as map WHERE map.cpu = 4
GROUP BY period_id
UNION ALL
SELECT
  period_id,
  'cpu5' as name,
  map.policy,
  cpu5_pws / total_dur as estimate_mw
FROM _wattson_base_components_pws
CROSS JOIN _dev_cpu_policy_map as map WHERE map.cpu = 5
GROUP BY period_id
UNION ALL
SELECT
  period_id,
  'cpu6' as name,
  map.policy,
  cpu6_pws / total_dur as estimate_mw
FROM _wattson_base_components_pws
CROSS JOIN _dev_cpu_policy_map as map WHERE map.cpu = 6
GROUP BY period_id
UNION ALL
SELECT
  period_id,
  'cpu7' as name,
  map.policy,
  cpu7_pws / total_dur as estimate_mw
FROM _wattson_base_components_pws
CROSS JOIN _dev_cpu_policy_map as map WHERE map.cpu = 7
GROUP BY period_id;

-- Sub-rail, meaning one level down from the CPU root node
DROP VIEW IF EXISTS _wattson_cpu_subrail_grouped;
CREATE PERFETTO VIEW _wattson_cpu_subrail_grouped AS
SELECT
  period_id,
  NULL as policy,
  'DSU_SCU' as name,
  dsu_scu_pws / total_dur as estimate_mw
FROM _wattson_base_components_pws
GROUP BY period_id
UNION ALL
SELECT
  period_id,
  policy,
  CONCAT('policy', policy) as name,
  SUM(estimate_mw) as estimate_mw
FROM _wattson_cpu_subsubrail_grouped
GROUP BY period_id, policy;

-- Grouped by CPUs, the smallest building block available
DROP VIEW IF EXISTS _cpu_subsubrail_estimate_per_startup_proto;
CREATE PERFETTO VIEW _cpu_subsubrail_estimate_per_startup_proto AS
SELECT
  period_id,
  policy,
  RepeatedField(
    AndroidWattsonRailEstimate(
      'name', name,
      'estimate_mw', estimate_mw
    )
  ) AS proto
FROM _wattson_cpu_subsubrail_grouped
GROUP BY period_id, policy;

-- Grouped by CPU policy
DROP VIEW IF EXISTS _cpu_subrail_estimate_per_startup_proto;
CREATE PERFETTO VIEW _cpu_subrail_estimate_per_startup_proto AS
SELECT
  period_id,
  RepeatedField(
    AndroidWattsonRailEstimate(
      'name', name,
      'estimate_mw', estimate_mw,
      'rail', _cpu_subsubrail_estimate_per_startup_proto.proto
    )
  ) AS proto
FROM _wattson_cpu_subrail_grouped
-- Some subrails will not have any subsubrails, so LEFT JOIN
LEFT JOIN _cpu_subsubrail_estimate_per_startup_proto USING (period_id, policy)
GROUP BY period_id;

-- Grouped into single entry for entirety of CPU system
DROP VIEW IF EXISTS _cpu_rail_estimate_per_startup_proto;
CREATE PERFETTO VIEW _cpu_rail_estimate_per_startup_proto AS
SELECT
  period_id,
  RepeatedField(
    AndroidWattsonRailEstimate(
      'name', name,
      'estimate_mw', estimate_mw,
      'rail', _cpu_subrail_estimate_per_startup_proto.proto
    )
  ) AS proto
FROM _wattson_cpu_rail_total
JOIN _cpu_subrail_estimate_per_startup_proto USING (period_id)
GROUP BY period_id;

