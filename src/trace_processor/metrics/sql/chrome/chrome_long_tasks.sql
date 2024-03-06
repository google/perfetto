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
--
-- WARNING: This metric should not be used as a source of truth. It is under
--          active development and the values & meaning might change without
--          notice.

INCLUDE PERFETTO MODULE chrome.tasks;

-- Extract mojo information for the long-task-tracking scenario for specific
-- names. For example, LongTaskTracker slices may have associated IPC
-- metadata, or InterestingTask slices for input may have associated IPC to
-- determine whether the task is fling/etc.
CREATE OR REPLACE PERFETTO FUNCTION select_long_task_slices(name STRING)
RETURNS TABLE(
  interface_name STRING,
  ipc_hash INT,
  message_type STRING,
  id INT,
  task_name STRING)
AS
WITH slices_with_mojo_data AS (
  SELECT
      EXTRACT_ARG(
        arg_set_id,
        "chrome_mojo_event_info.mojo_interface_tag"
      ) AS interface_name,
      EXTRACT_ARG(
        arg_set_id,
        "chrome_mojo_event_info.ipc_hash"
      ) AS ipc_hash,
      CASE
        WHEN EXTRACT_ARG(arg_set_id, "chrome_mojo_event_info.is_reply") THEN "reply"
        ELSE "message"
      END AS message_type,
      id
  FROM slice
  WHERE
    category GLOB "*scheduler.long_tasks*"
    AND name = $name
)
SELECT
  *,
  printf("%s %s(hash=%s)", interface_name, message_type, ipc_hash) as task_name
FROM slices_with_mojo_data;

CREATE OR REPLACE PERFETTO FUNCTION is_long_choreographer_task(dur LONG)
RETURNS BOOL AS
SELECT $dur >= 4 * 1e6;

-- Note that not all slices will be mojo slices; filter on interface_name IS
-- NOT NULL for mojo slices specifically.
DROP TABLE IF EXISTS long_tasks_extracted_slices;
CREATE PERFETTO TABLE long_tasks_extracted_slices AS
SELECT * FROM SELECT_LONG_TASK_SLICES(/*name*/'LongTaskTracker');

-- Create |long_tasks_internal_tbl| table, which gathers all of the
-- information needed to produce the full name + metadata required
-- for LongTaskTracker slices. Unlike toplevel slices, which will
-- have nested descendants, LongTaskTracker slices will store all of
-- the relevant information within the single slice.
DROP TABLE IF EXISTS long_tasks_internal_tbl;
CREATE PERFETTO TABLE long_tasks_internal_tbl AS
WITH
  raw_extracted_values AS (
    SELECT
      mojo.id,
      mojo.interface_name,
      mojo.ipc_hash,
      mojo.message_type,
      _get_posted_from(s.arg_set_id) as posted_from
    FROM long_tasks_extracted_slices mojo
    JOIN slice s ON mojo.id = s.id
  )
SELECT
  id,
  CASE
    WHEN interface_name IS NOT NULL
      THEN printf('%s %s (hash=%d)', interface_name, message_type, ipc_hash)
    ELSE
      _format_scheduler_task_name(posted_from)
    END AS full_name,
  interface_name IS NOT NULL AS is_mojo
FROM raw_extracted_values;

-- Attach java views to its associated LongTaskTracker slice, as they
-- will be on different tracks. This follows the same logic as creating
-- chrome_slices_with_java_views_internal, differing only in how a
-- descendent is calculated.
DROP VIEW IF EXISTS long_task_slices_with_java_views;
CREATE PERFETTO VIEW long_task_slices_with_java_views AS
WITH
  -- Select UI thread BeginMainFrames frames.
  root_slices AS (
    SELECT *
    FROM _SELECT_BEGIN_MAIN_FRAME_JAVA_SLICES('LongTaskTracker')
    UNION ALL
    SELECT id, "Choreographer" as kind, ts, dur, name
    FROM slice
    WHERE is_long_choreographer_task(dur)
      AND name GLOB "Looper.dispatch: android.view.Choreographer$FrameHandler*"
  ),
  -- Intermediate step to allow us to sort java view names.
  root_slice_and_java_view_not_grouped AS (
    SELECT
      s1.id, s1.kind, s2.name AS java_view_name
    FROM root_slices s1
    JOIN _chrome_java_views s2
      ON (
        s1.ts < s2.ts AND s1.ts + s1.dur > s2.ts + s2.dur)
  )
SELECT
  s1.id,
  s1.kind,
  GROUP_CONCAT(DISTINCT s2.java_view_name) AS java_views
FROM root_slices s1
LEFT JOIN root_slice_and_java_view_not_grouped s2
  USING (id)
GROUP BY s1.id;

DROP VIEW IF EXISTS chrome_long_tasks_internal;
CREATE PERFETTO VIEW chrome_long_tasks_internal AS
WITH -- Generate full names for tasks with java views.
  java_views_tasks AS (
    SELECT
      printf('%s(java_views=%s)', kind, java_views) as full_name,
      _get_java_views_task_type(kind) AS task_type,
      id
    FROM long_task_slices_with_java_views
    WHERE kind = "SingleThreadProxy::BeginMainFrame"
  ),
  scheduler_tasks_with_mojo AS (
    SELECT
      full_name,
      'mojo' as task_type,
      id
    FROM long_tasks_internal_tbl
    WHERE is_mojo
  ),
  navigation_tasks AS (
    SELECT
      -- NOTE: unless Navigation category is enabled and recorded on the same
      -- track as the LongTaskTracker slice, frame type will always be unknown.
      printf('%s (%s)',
        _human_readable_navigation_task_name(full_name),
        IFNULL(_extract_frame_type(id), 'unknown frame type')) AS full_name,
      'navigation_task' AS task_type,
      id
    FROM scheduler_tasks_with_mojo
    WHERE _human_readable_navigation_task_name(full_name) IS NOT NULL
  )
SELECT
  COALESCE(s4.full_name, s3.full_name, s2.full_name, s1.full_name) AS full_name,
  COALESCE(s4.task_type, s3.task_type, s2.task_type, 'scheduler') as task_type,
  s1.id as id
FROM long_tasks_internal_tbl s1
LEFT JOIN scheduler_tasks_with_mojo s2 ON s2.id = s1.id
LEFT JOIN java_views_tasks s3 ON s3.id = s1.id
LEFT JOIN navigation_tasks s4 ON s4.id = s1.id
UNION ALL
-- Choreographer slices won't necessarily be associated with an overlying
-- LongTaskTracker slice, so join them separately.
SELECT
  printf('%s(java_views=%s)', kind, java_views) as full_name,
  _get_java_views_task_type(kind) AS task_type,
  id
FROM long_task_slices_with_java_views
WHERE kind = "Choreographer";

DROP VIEW IF EXISTS chrome_long_tasks;
CREATE PERFETTO VIEW chrome_long_tasks AS
SELECT
  full_name,
  task_type,
  thread.name AS thread_name,
  thread.utid,
  process.name AS process_name,
  thread.upid,
  ts.*
FROM chrome_long_tasks_internal cti
JOIN slice ts USING (id)
JOIN thread_track tt ON ts.track_id = tt.id
JOIN thread USING (utid)
JOIN process USING (upid);
