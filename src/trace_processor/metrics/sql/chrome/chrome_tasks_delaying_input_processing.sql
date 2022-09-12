--
-- Copyright 2022 The Android Open Source Project
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

-- Script params:
-- {{duration_causing_jank_ms}} : The duration of a single task that would cause
-- jank, by delaying input from being handled on the main thread.

SELECT RUN_METRIC('chrome/chrome_input_to_browser_intervals.sql');

-- Get the tasks that was running for more than 8ms within windows
-- that we could have started processing input but did not on the
-- main thread, because it was blocked by those tasks.
DROP VIEW IF EXISTS chrome_tasks_delaying_input_processing_unaggregated;
CREATE VIEW chrome_tasks_delaying_input_processing_unaggregated AS
SELECT
  tasks.full_name AS full_name,
  tasks.dur / 1e6 AS duration_ms,
  id AS slice_id,
  thread_dur / 1e6 AS thread_dur_ms,
  chrome_input_to_browser_intervals.window_start_id,
  chrome_input_to_browser_intervals.window_end_id
FROM
  (
    (
      SELECT
        chrome_tasks.full_name AS full_name,
        chrome_tasks.dur AS dur,
        chrome_tasks.ts AS ts,
        chrome_tasks.id AS id,
        chrome_tasks.upid AS upid,
        thread_dur
      FROM
        chrome_tasks
      WHERE
        chrome_tasks.dur >= {{duration_causing_jank_ms}} * 1e6
        and chrome_tasks.thread_name = "CrBrowserMain"
    ) tasks
    JOIN chrome_input_to_browser_intervals
      ON tasks.ts + tasks.dur > chrome_input_to_browser_intervals.window_start_ts
      AND tasks.ts + tasks.dur < chrome_input_to_browser_intervals.window_end_ts
      AND tasks.upid = chrome_input_to_browser_intervals.upid
  );

-- Same task can delay multiple GestureUpdates, this step dedups
-- multiple occrences of the same slice_id
DROP VIEW IF EXISTS chrome_tasks_delaying_input_processing;
CREATE VIEW chrome_tasks_delaying_input_processing AS
SELECT
  full_name,
  duration_ms,
  slice_id,
  thread_dur_ms
FROM chrome_tasks_delaying_input_processing_unaggregated
GROUP BY slice_id;

-- Get the tasks that were running for more than 8ms within windows
-- that we could have started processing input but did not on the
-- main thread, because it was blocked by those tasks.
DROP VIEW IF EXISTS chrome_tasks_delaying_input_processing_summary;
CREATE VIEW chrome_tasks_delaying_input_processing_summary AS
SELECT
  full_name AS full_name,
  AVG(duration_ms) AS avg_duration_ms,
  AVG(thread_dur_ms) AS avg_thread_duration_ms,
  MIN(duration_ms) AS min_task_duration,
  MAX(duration_ms) as max_task_duration,
  SUM(duration_ms) AS total_duration_ms,
  SUM(thread_dur_ms) AS total_thread_duration_ms,
  GROUP_CONCAT(slice_id, '-') AS slice_ids,
  COUNT(*) AS count
FROM
  chrome_tasks_delaying_input_processing
GROUP BY
  full_name;