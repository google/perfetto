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

INCLUDE PERFETTO MODULE android.statsd;

-- Desktop Windows with durations they were open.
CREATE PERFETTO PIPELINE android_desktop_mode_windows(
  -- Window add timestamp; NULL if no add event in the trace.
  raw_add_ts TIMESTAMP,
  -- Window remove timestamp; NULL if no remove event in the trace.
  raw_remove_ts TIMESTAMP,
  -- Timestamp that the window was added; or trace_start() if no add event in the trace.
  ts TIMESTAMP,
  -- Furation the window was open; or until trace_end() if no remove event in the trace.
  dur DURATION,
  -- Desktop Window instance ID - unique per window.
  instance_id LONG,
  -- UID of the app running in the window.
  uid LONG
) MATERIALIZED AS
SUBPIPELINE atoms AS (
  FROM android_statsd_atoms
  |> WHERE name = 'desktop_mode_session_task_update'
  |> SELECT
       ts,
       extract_arg(arg_set_id, 'desktop_mode_session_task_update.task_event') AS type,
       extract_arg(arg_set_id, 'desktop_mode_session_task_update.instance_id') AS instance_id,
       extract_arg(arg_set_id, 'desktop_mode_session_task_update.uid') AS uid,
       extract_arg(arg_set_id, 'desktop_mode_session_task_update.session_id') AS session_id
)
SUBPIPELINE dw_statsd_events_add AS (
  FROM atoms |> WHERE type = 'TASK_ADDED'
)
SUBPIPELINE dw_statsd_events_remove AS (
  FROM atoms |> WHERE type = 'TASK_REMOVED'
)
SUBPIPELINE dw_statsd_reset_event AS (
  FROM atoms
  |> WHERE type = 'TASK_INIT_STATSD'
  |> SELECT ts
  |> UNION (SELECT trace_end() AS ts)
)
FROM dw_statsd_events_add AS a
|> JOIN dw_statsd_events_remove AS r USING (instance_id, session_id) FULL
|> SELECT
     a.ts AS raw_add_ts,
     r.ts AS raw_remove_ts,
     -- Assume trace_start() if no add event found.
     coalesce(a.ts, trace_start()) AS ts,
     -- Assume next reset event or trace_end() if no remove event found.
     coalesce(
       r.ts,
       (
         SELECT min(ts)
         FROM dw_statsd_reset_event
         WHERE ts > coalesce(a.ts, trace_start())
       )
     )
     - coalesce(a.ts, trace_start()) AS dur,
     coalesce(a.instance_id, r.instance_id) AS instance_id,
     coalesce(a.uid, r.uid) AS uid
|> FORK AS dw_windows
-- Assume window was open for the entire trace if we only see change events for the instance ID.
|> UNION (
     FROM atoms
     |> WHERE type = 'TASK_INFO_CHANGED'
     |> AGGREGATE min(uid) AS uid GROUP BY instance_id, session_id
     |> WHERE NOT (instance_id IN (SELECT instance_id FROM dw_windows))
     |> SELECT
          NULL AS raw_add_ts,
          NULL AS raw_remove_ts,
          trace_start() AS ts,
          trace_end() - trace_start() AS dur,
          instance_id,
          uid
   );
