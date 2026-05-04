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

-- Provides unified access to Android Process State events from StatsD.
--
-- Suggested minimal config:
--
-- data_sources: {
--     config: {
--         name: "android.statsd"
--         statsd_config: {
--             atom_id: 27  # UidProcessStateChanged
--         }
--     }
-- }

-- Table for Process State Changes from StatsD atom uid_process_state_changed
CREATE PERFETTO TABLE android_process_state_changes (
  -- Timestamp of process state change.
  ts TIMESTAMP,
  -- UID of process.
  uid LONG,
  -- Process state name.
  process_state_name STRING
) AS
SELECT
  s.ts,
  extract_arg(s.arg_set_id, 'uid_process_state_changed.uid') AS uid,
  extract_arg(s.arg_set_id, 'uid_process_state_changed.state') AS process_state_name
FROM slice AS s
JOIN track AS t
  ON s.track_id = t.id
WHERE
  t.name = 'Statsd Atoms' AND s.name = 'uid_process_state_changed';

-- View to get process state intervals, showing how long each process stayed in each state.
CREATE PERFETTO VIEW android_process_state (
  -- Timestamp of process state change.
  ts TIMESTAMP,
  -- Duration of process state.
  dur DURATION,
  -- UID of process.
  uid LONG,
  -- Process state name.
  process_state_name STRING
) AS
SELECT
  ts,
  lead(ts, 1, (
    SELECT
      end_ts
    FROM trace_bounds
  )) OVER (PARTITION BY uid ORDER BY ts) - ts AS dur,
  uid,
  process_state_name
FROM android_process_state_changes;
