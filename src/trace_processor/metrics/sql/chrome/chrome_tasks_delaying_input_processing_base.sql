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
-- {{task_table_name}} : The table tracking chrome tasks which will be used to
-- determine which chrome tasks are causing the delay. One of chrome_tasks or
-- chrome_long_tasks.
-- {{input_browser_interval_table_name}} : The table tracking chrome input to
-- browser interval. This may differ based on whether the scenario is for
-- topLevel events or LongTask events.

SELECT CREATE_VIEW_FUNCTION(
  '{{function_prefix}}SELECT_SLOW_BROWSER_TASKS()',
  'full_name STRING, dur INT, ts INT, id INT, upid INT, thread_dur INT',
  'SELECT
    task_table.full_name AS full_name,
    task_table.dur AS dur,
    task_table.ts AS ts,
    task_table.id AS id,
    task_table.upid AS upid,
    thread_dur
  FROM
    {{task_table_name}} task_table
  WHERE
    task_table.dur >= {{duration_causing_jank_ms}} * 1e6
    AND task_table.thread_name = "CrBrowserMain"
  '
);

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
  input_tbl.window_start_id,
  input_tbl.window_end_id
FROM ({{function_prefix}}SELECT_SLOW_BROWSER_TASKS()) tasks
JOIN {{input_browser_interval_table_name}} input_tbl
  ON tasks.ts + tasks.dur > input_tbl.window_start_ts
    AND tasks.ts + tasks.dur < input_tbl.window_end_ts
    AND tasks.upid = input_tbl.upid;

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
  MAX(duration_ms) AS max_task_duration,
  SUM(duration_ms) AS total_duration_ms,
  SUM(thread_dur_ms) AS total_thread_duration_ms,
  GROUP_CONCAT(slice_id, '-') AS slice_ids,
  COUNT(*) AS count
FROM
  chrome_tasks_delaying_input_processing
GROUP BY
  full_name;
