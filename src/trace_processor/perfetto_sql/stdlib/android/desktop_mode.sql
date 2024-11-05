--
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
--

include perfetto module android.statsd;

-- Desktop Windows with durations they were open.
CREATE PERFETTO TABLE android_desktop_mode_windows (
-- Window add timestamp; NULL if no add event in the trace.
raw_add_ts INT,
-- Window remove timestamp; NULL if no remove event in the trace.
raw_remove_ts INT,
-- timestamp that the window was added; or trace_start() if no add event in the trace.
ts INT,
-- duration the window was open; or until trace_end() if no remove event in the trace.
dur INT,
-- Desktop Window instance ID - unique per window.
instance_id INT,
-- UID of the app running in the window.
uid INT
) AS
WITH
  atoms AS (
    SELECT
      ts,
      extract_arg(arg_set_id, 'desktop_mode_session_task_update.task_event') AS type,
      extract_arg(arg_set_id, 'desktop_mode_session_task_update.instance_id') AS instance_id,
      extract_arg(arg_set_id, 'desktop_mode_session_task_update.uid') AS uid,
      extract_arg(arg_set_id, 'desktop_mode_session_task_update.session_id') AS session_id
    FROM android_statsd_atoms
    WHERE name = 'desktop_mode_session_task_update'),
  dw_statsd_events_add AS (
    SELECT *
    FROM atoms
    WHERE type = 'TASK_ADDED'),
  dw_statsd_events_remove AS (
    SELECT * FROM atoms
    WHERE type = 'TASK_REMOVED'),
  dw_statsd_events_update_by_instance AS (
    SELECT instance_id, session_id, min(uid) AS uid FROM atoms
    WHERE type = 'TASK_INFO_CHANGED' GROUP BY instance_id, session_id),
  dw_windows AS (
    SELECT
      a.ts AS raw_add_ts,
      r.ts AS raw_remove_ts,
      ifnull(a.ts, trace_start()) AS ts,  -- Assume trace_start() if no add event found.
      ifnull(r.ts, trace_end()) - ifnull(a.ts, trace_start()) AS dur,  -- Assume trace_end() if no remove event found.
      ifnull(a.instance_id, r.instance_id) AS instance_id,
      ifnull(a.uid, r.uid) AS uid
    FROM dw_statsd_events_add a
    FULL JOIN dw_statsd_events_remove r USING(instance_id, session_id)),
  -- Assume window was open for the entire trace if we only see change events for the instance ID.
  dw_windows_with_update_events AS (
    SELECT * FROM dw_windows
    UNION
    SELECT NULL, NULL, trace_start(), trace_end() - trace_start(), instance_id, uid
    FROM dw_statsd_events_update_by_instance
    WHERE
    instance_id NOT IN (SELECT instance_id FROM dw_windows))
SELECT * FROM dw_windows_with_update_events;

