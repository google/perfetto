
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

INCLUDE PERFETTO MODULE android.startup.startups;
INCLUDE PERFETTO MODULE wattson.curves.ungrouped;

DROP VIEW IF EXISTS _app_startup_window;
CREATE PERFETTO VIEW _app_startup_window AS
SELECT
  ts,
  dur,
  startup_id
FROM android_startups;

DROP TABLE IF EXISTS _windowed_wattson;
CREATE VIRTUAL TABLE _windowed_wattson
USING
  SPAN_JOIN(_app_startup_window, _system_state_curves);

DROP VIEW IF EXISTS _wattson_base_components_pws;
CREATE PERFETTO VIEW _wattson_base_components_pws AS
SELECT
  -- *_curve is in units of mW, so multiplying by ns gives pWs
  SUM(static_curve * dur) as static_pws,
  SUM(cpu0_curve * dur) as cpu0_pws,
  SUM(cpu1_curve * dur) as cpu1_pws,
  SUM(cpu2_curve * dur) as cpu2_pws,
  SUM(cpu3_curve * dur) as cpu3_pws,
  SUM(cpu4_curve * dur) as cpu4_pws,
  SUM(cpu5_curve * dur) as cpu5_pws,
  SUM(cpu6_curve * dur) as cpu6_pws,
  SUM(cpu7_curve * dur) as cpu7_pws,
  -- L3/SCU interconnect is already scaled by 10^6 in the L3 LUTs, so when
  -- converting to pWs need to scale by 10^3
  SUM(l3_hit_value + l3_miss_value) * 1000 as l3_pws,
  SUM(dur) as total_dur,
  startup_id
FROM _windowed_wattson
GROUP BY startup_id;

DROP VIEW IF EXISTS _wattson_cpu_rail_total;
CREATE PERFETTO VIEW _wattson_cpu_rail_total AS
SELECT
  startup_id,
  total_dur,
  'cpu_subsystem' as name,
  (
    cpu0_pws + cpu1_pws + cpu2_pws + cpu3_pws +
    cpu4_pws + cpu5_pws + cpu6_pws + cpu7_pws +
    static_pws + l3_pws
  ) / total_dur as estimate_mw
FROM _wattson_base_components_pws
GROUP BY startup_id;

DROP VIEW IF EXISTS _cpu_rail_estimate_per_startup_proto;
CREATE PERFETTO VIEW _cpu_rail_estimate_per_startup_proto AS
SELECT
  startup_id,
  RepeatedField(
    AndroidWattsonRailEstimate(
      'name', name,
      'estimate_mw', estimate_mw
    )
  ) AS proto
FROM _wattson_cpu_rail_total
GROUP BY startup_id;

DROP VIEW IF EXISTS wattson_app_startup_output;
CREATE PERFETTO VIEW wattson_app_startup_output AS
SELECT AndroidWattsonTimePeriodMetric(
  'metric_version', 1,
  'period_info', (
    SELECT RepeatedField(
      AndroidWattsonEstimateInfo(
        'period_id', startup_id,
        'period_dur', dur,
        'rail', _cpu_rail_estimate_per_startup_proto.proto
      )
    )
    FROM _app_startup_window
    JOIN _cpu_rail_estimate_per_startup_proto USING (startup_id)
  )
);

