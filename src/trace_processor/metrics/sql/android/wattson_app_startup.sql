
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

DROP VIEW IF EXISTS _app_startup_window;
CREATE PERFETTO VIEW _app_startup_window AS
SELECT
  ts,
  dur,
  startup_id
FROM android_startups;

SELECT RUN_METRIC(
  'android/wattson_rail_relations.sql',
  'window_table', '_app_startup_window'
);

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
