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

-- Provides unified access to Android App Freeze events from StatsD.
--
-- Suggested minimal config:
--
-- data_sources: {
--     config: {
--         name: "android.statsd"
--         statsd_config: {
--             atom_id: 254  # AppFreezeChanged
--         }
--     }
-- }

-- Table for raw App Freeze state change events from StatsD
CREATE PERFETTO TABLE android_app_freeze_changes(
  -- Timestamp of app freeze change.
  ts TIMESTAMP,
  -- PID of process.
  pid LONG,
  -- Action taken (e.g. FREEZE_APP, UNFREEZE_APP).
  action STRING,
  -- Process name.
  process_name STRING,
  -- Time unfrozen in millis.
  time_unfrozen_millis LONG,
  -- Reason for unfreezing.
  unfreeze_reason STRING
)
AS
SELECT
  s.ts,
  extract_arg(s.arg_set_id, 'app_freeze_changed.pid') AS pid,
  extract_arg(s.arg_set_id, 'app_freeze_changed.action') AS action,
  extract_arg(s.arg_set_id, 'app_freeze_changed.process_name') AS process_name,
  extract_arg(s.arg_set_id, 'app_freeze_changed.time_unfrozen_millis') AS time_unfrozen_millis,
  extract_arg(s.arg_set_id, 'app_freeze_changed.unfreeze_reason_v2') AS unfreeze_reason
FROM slice AS s
JOIN track AS t
  ON s.track_id = t.id
WHERE
  t.name = 'Statsd Atoms'
  AND s.name = 'app_freeze_changed';

-- View to get app freezer state intervals
CREATE PERFETTO VIEW android_app_freeze_state(
  -- Timestamp of app freeze state change.
  ts TIMESTAMP,
  -- Duration of app freeze state.
  dur DURATION,
  -- PID of process.
  pid LONG,
  -- Process name.
  process_name STRING,
  -- Freezer state.
  freezer_state STRING,
  -- Time unfrozen in millis.
  time_unfrozen_millis LONG,
  -- Reason for unfreezing.
  unfreeze_reason STRING
)
AS
SELECT
  ts,
  lead(ts, 1, (SELECT end_ts FROM trace_bounds)) OVER (
    PARTITION BY
      pid
    ORDER BY ts
  )
  - ts AS dur,
  pid,
  process_name,
  -- Directly use the 'action' column
  action AS freezer_state,
  time_unfrozen_millis,
  unfreeze_reason
FROM android_app_freeze_changes;
