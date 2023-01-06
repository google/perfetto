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

SELECT RUN_METRIC("common/parent_slice.sql");

SELECT CREATE_FUNCTION(
  '{{function_prefix}}EXTRACT_MOJO_IPC_HASH(slice_id INT)',
  'INT',
  '
    SELECT EXTRACT_ARG(s2.arg_set_id, "chrome_mojo_event_info.ipc_hash")
    FROM descendant_slice($slice_id) s2
    WHERE s2.name="ScopedSetIpcHash"
    ORDER BY s2.id
    LIMIT 1
  '
);

SELECT CREATE_FUNCTION(
  '{{function_prefix}}EXTRACT_FRAME_TYPE(slice_id INT)',
  'INT',
  '
    SELECT EXTRACT_ARG(descendants.arg_set_id, "render_frame_host.frame_type")
    FROM descendant_slice($slice_id) descendants
    WHERE descendants.name IN ("RenderFrameHostImpl::BeginNavigation",
        "RenderFrameHostImpl::DidCommitProvisionalLoad",
        "RenderFrameHostImpl::DidCommitSameDocumentNavigation",
        "RenderFrameHostImpl::DidStopLoading")
    LIMIT 1
  '
);

-- Create |chrome_mojo_slices_tbl| table, containing a subset of slice
-- table with the slices corresponding to mojo messages.
--
-- Note: this might include messages received within a sync mojo call.
DROP TABLE IF EXISTS chrome_mojo_slices_tbl;
CREATE TABLE chrome_mojo_slices_tbl AS
WITH
-- Select all new-style (post crrev.com/c/3270337) mojo slices and
-- generate |full_name| for them.
-- If extended tracing is enabled, the slice name will have the full method
-- name (i.e. "Receive content::mojom::FrameHost::DidStopLoading") and we
-- should use it as a full name.
-- If extended tracing is not enabled, we should include the interface name
-- and method hash into the full name.
new_mojo_slices AS (
  SELECT
    EXTRACT_ARG(s.arg_set_id, "chrome_mojo_event_info.mojo_interface_tag") AS interface_name,
    EXTRACT_ARG(arg_set_id, "chrome_mojo_event_info.ipc_hash") AS ipc_hash,
    CASE name
      WHEN "Receive mojo message" THEN "message"
      WHEN "Receive mojo reply" THEN "reply"
    END AS message_type,
    s.id
  FROM {{slice_table_name}} s
  WHERE
    category = "toplevel"
    AND name GLOB 'Receive *'
),
-- Select old-style slices for channel-associated mojo events.
old_associated_mojo_slices AS (
  SELECT
    s.name AS interface_name,
    {{function_prefix}}EXTRACT_MOJO_IPC_HASH(s.id) AS ipc_hash,
    "message" AS message_type,
    s.id
  FROM {{slice_table_name}} s
  WHERE
    category = "mojom"
    AND name GLOB '*.mojom.*'
),
-- Select old-style slices for non-(channel-associated) mojo events.
old_non_associated_mojo_slices AS (
  SELECT
    COALESCE(
      EXTRACT_ARG(s.arg_set_id, "chrome_mojo_event_info.watcher_notify_interface_tag"),
      EXTRACT_ARG(s.arg_set_id, "chrome_mojo_event_info.mojo_interface_tag")
    ) AS interface_name,
    {{function_prefix}}EXTRACT_MOJO_IPC_HASH(s.id) AS ipc_hash,
    "message" AS message_type,
    s.id
  FROM {{slice_table_name}} s
  WHERE
    category = "toplevel" AND name = "Connector::DispatchMessage"
)
-- Merge all mojo slices.
SELECT * FROM new_mojo_slices
UNION ALL
SELECT * FROM old_associated_mojo_slices
UNION ALL
SELECT * FROM old_non_associated_mojo_slices;

-- As we lookup by ID on |chrome_mojo_slices_tbl| table, add an index on
-- id to make lookups fast.
DROP INDEX IF EXISTS chrome_mojo_slices_idx;
CREATE INDEX chrome_mojo_slices_idx ON chrome_mojo_slices_tbl(id);

-- This table contains a list of slices corresponding to the _representative_ Chrome Java views.
-- These are the outermost Java view slices after filtering out generic framework views
-- (like FitWindowsLinearLayout) and selecting the outermost slices from the remaining ones.
DROP TABLE IF EXISTS chrome_java_views_internal;
CREATE TABLE chrome_java_views_internal AS
WITH
-- .draw, .onLayout and .onMeasure parts of the java view names don't add much, strip them.
java_slices_with_trimmed_names AS (
  SELECT
    id,
    REPLACE(
      REPLACE(
        REPLACE(
          REPLACE(
            REPLACE(
              s1.name,
              ".draw", ""),
            ".onLayout", ""),
          ".onMeasure", ""),
        ".Layout", ""),
      ".Measure", "") AS name
  FROM
    {{slice_table_name}} s1
  WHERE category = "Java" AND dur > 0
),
-- We filter out generic slices from various UI frameworks which don't tell us much about
-- what exactly this view is doing.
interesting_java_slices AS (
  SELECT
    id, name
  FROM java_slices_with_trimmed_names
  WHERE NOT name IN (
      -- AndroidX.
      "FitWindowsFrameLayout",
      "FitWindowsLinearLayout",
      "ContentFrameLayout",
      "CoordinatorLayout",
      -- Other non-Chrome UI libraries.
      "ComponentHost",
      -- Generic Chrome frameworks.
      "CompositorView:finalizeLayers",
      "CompositorViewHolder",
      "CompositorViewHolder:layout",
      "CompositorViewHolder:updateContentViewChildrenDimension",
      "CoordinatorLayoutForPointer",
      "OptimizedFrameLayout",
      "ViewResourceAdapter:getBitmap",
      "ViewResourceFrameLayout",
      -- Non-specific Chrome slices.
      "AppCompatImageButton",
      "ScrollingBottomViewResourceFrameLayout",
      -- Screenshots get their custom annotations below.
      "ViewResourceAdapter:captureWithHardwareDraw",
      "ViewResourceAdapter:captureWithSoftwareDraw",
      -- Non-bytecode generated slices.
      "LayoutDriver:onUpdate"
    )
)
SELECT
  s1.*,
  -- While the parent slices are too generic to be used by themselves,
  -- they can provide some useful metadata.
  HAS_PARENT_SLICE_WITH_NAME(
    s1.id,
    "ViewResourceAdapter:captureWithSoftwareDraw"
  ) AS is_software_screenshot,
  HAS_PARENT_SLICE_WITH_NAME(
    s1.id,
    "ViewResourceAdapter:captureWithHardwareDraw"
  ) AS is_hardware_screenshot
FROM interesting_java_slices s1
WHERE (SELECT count()
  FROM ancestor_slice(s1.id) s2
  JOIN interesting_java_slices s3 ON s2.id = s3.id) = 0;

-- |chrome_java_views| is a view over |chrome_java_views_internal| table, adding the necessary columns
-- from |slice|.
DROP VIEW IF EXISTS chrome_java_views;
CREATE VIEW chrome_java_views AS
SELECT
  s1.name AS filtered_name,
  s1.is_software_screenshot,
  s1.is_hardware_screenshot,
  s2.*
FROM chrome_java_views_internal s1
JOIN {{slice_table_name}} s2 USING (id);

-- Most of java views will be triggered either by Chrome's BeginMainFrame
-- or by Android's Choreographer.
DROP VIEW IF EXISTS chrome_slices_with_java_views_internal;
CREATE VIEW chrome_slices_with_java_views_internal AS
WITH
-- Select UI thread BeginMainFrames and Choreographer frames.
root_slices AS (
  SELECT
    id,
    (CASE name
        WHEN 'ThreadControllerImpl::RunTask' THEN 'SingleThreadProxy::BeginMainFrame'
        ELSE 'Choreographer'
      END) AS kind
  FROM {{slice_table_name}}
  WHERE
    (name GLOB 'Looper.dispatch: android.view.Choreographer$FrameHandler*')
    OR (name = 'ThreadControllerImpl::RunTask'
      AND EXTRACT_ARG(arg_set_id, 'task.posted_from.file_name') = 'cc/trees/single_thread_proxy.cc'
      AND EXTRACT_ARG(arg_set_id, 'task.posted_from.function_name') = 'ScheduledActionSendBeginMainFrame')
),
-- Intermediate step to allow us to sort java view names.
root_slice_and_java_view_not_grouped AS (
  SELECT
    s1.id, s1.kind, s3.name AS java_view_name
  FROM root_slices s1
  JOIN descendant_slice(s1.id) s2
  JOIN chrome_java_views_internal s3 ON s2.id = s3.id
)
SELECT
  s1.id,
  s1.kind,
  GROUP_CONCAT(DISTINCT s2.java_view_name) AS java_views
FROM root_slices s1
LEFT JOIN root_slice_and_java_view_not_grouped s2 USING (id)
GROUP BY s1.id;

-- Create |chrome_tasks| table, which contains a subset of slice
-- table of the slices which should be considered top-level Chrome tasks with the
-- additional scheduler_type |full_name| column, derived from subevents.
DROP TABLE IF EXISTS chrome_tasks_internal;
CREATE TABLE chrome_tasks_internal AS
WITH
-- Select slices from "toplevel" category which do not have another
-- "toplevel" slice as ancestor. The possible cases include sync mojo messages
-- and tasks in nested runloops.
non_embedded_toplevel_slices AS (
  SELECT * FROM {{slice_table_name}} s
  WHERE
    category IN ("toplevel", "toplevel,viz")
    AND (SELECT count() FROM ancestor_slice(s.id) s2
      WHERE s2.category IN ("toplevel", "toplevel.viz")) = 0
),
-- Select slices from "Java" category which do not have another "Java" or
-- "toplevel" slice as parent. In the longer term they should probably belong
-- to "toplevel" category as well, but for now this will have to do.
non_embedded_java_slices AS (
  SELECT name AS full_name, "java" AS task_type, id
  FROM {{slice_table_name}} s
  WHERE
    category = "Java"
    AND (SELECT count()
      FROM ancestor_slice(s.id) s2
      WHERE s2.category = "toplevel" OR s2.category = "Java") = 0
),
raw_scheduler_tasks AS (
  SELECT
    EXTRACT_ARG(s.arg_set_id, "task.posted_from.file_name") AS posted_from_file_name,
    EXTRACT_ARG(s.arg_set_id, "task.posted_from.function_name") AS posted_from_function_name,
    (CASE name
        WHEN "ThreadControllerImpl::RunTask" THEN "SequenceManager"
        WHEN "ThreadPool_RunTask" THEN "ThreadPool"
      END) AS scheduler_type,
    s.id
  FROM {{slice_table_name}} s
  WHERE
    category = "toplevel"
    AND (name = "ThreadControllerImpl::RunTask" OR name = "ThreadPool_RunTask")
),
scheduler_tasks AS (
  SELECT
    s1.posted_from_file_name || ":" || s1.posted_from_function_name AS posted_from,
    s1.posted_from_file_name,
    s1.posted_from_function_name,
    s1.scheduler_type,
    s1.id
  FROM raw_scheduler_tasks s1
),
-- Generate full names for scheduler tasks.
scheduler_tasks_with_full_names AS (
  SELECT
    printf("RunTask(posted_from=%s)", s.posted_from) AS full_name,
    "scheduler" AS task_type,
    s.id
  FROM scheduler_tasks s
),
-- Generate full names for mojo slices.
mojo_slices AS (
  SELECT
    printf('%s %s (hash=%d)',
      interface_name, message_type, ipc_hash) AS full_name,
    "mojo" AS task_type,
    id
  FROM chrome_mojo_slices_tbl
),
-- Generate full names for tasks with java views.
java_views_tasks AS (
  SELECT
    printf('%s(java_views=%s)', kind, java_views) AS full_name,
    (CASE kind
        WHEN 'Choreographer' THEN 'choreographer'
        WHEN 'SingleThreadProxy::BeginMainFrame' THEN 'ui_thread_begin_main_frame'
      END) AS task_type,
    id
  FROM chrome_slices_with_java_views_internal
),
-- Select scheduler tasks which are used to run mojo messages and use the mojo names
-- as full names for these slices.
-- We restrict this to specific scheduler tasks which are expected to run mojo
-- tasks due to sync mojo events, which also emit similar events.
scheduler_tasks_with_mojo AS (
  SELECT
    (SELECT s3.full_name
      FROM descendant_slice(s1.id) s2
      JOIN mojo_slices s3 USING (id)
      ORDER BY s2.depth LIMIT 1) AS full_name,
    "mojo" AS task_type,
    s1.id
  FROM
    scheduler_tasks s1
  WHERE
    s1.posted_from IN (
      "mojo/public/cpp/system/simple_watcher.cc:Notify",
      "mojo/public/cpp/bindings/lib/connector.cc:PostDispatchNextMessageFromPipe",
      "ipc/ipc_mojo_bootstrap.cc:Accept")
),
navigation_tasks AS (
  SELECT
    printf("%s (%s)",
      CASE
        WHEN full_name = 'content.mojom.FrameHost message (hash=2168461044)' THEN 'FrameHost::BeginNavigation'
        WHEN full_name = 'content.mojom.FrameHost message (hash=3561497419)' THEN 'FrameHost::DidCommitProvisionalLoad'
        WHEN full_name = 'content.mojom.FrameHost message (hash=1421450774)' THEN 'FrameHost::DidCommitSameDocumentNavigation'
        WHEN full_name = 'content.mojom.FrameHost message (hash=368650583)' THEN 'FrameHost::DidStopLoading'
      END,
      IFNULL({{function_prefix}}EXTRACT_FRAME_TYPE(id), 'unknown frame type')) AS full_name,
    'navigation_task' AS task_type,
    id
  FROM (
    SELECT * FROM scheduler_tasks_with_mojo
    WHERE full_name IN ('content.mojom.FrameHost message (hash=2168461044)',
      'content.mojom.FrameHost message (hash=3561497419)',
      'content.mojom.FrameHost message (hash=1421450774)',
      'content.mojom.FrameHost message (hash=368650583)')
    )
),
-- Add scheduler and mojo full names to non-embedded slices from
-- the "toplevel" category, with mojo ones taking precedence.
non_embedded_toplevel_slices_with_full_name AS (
  SELECT
    COALESCE(s5.full_name, s4.full_name, s2.full_name, s3.full_name, s1.name) AS full_name,
    COALESCE(s5.task_type, s4.task_type, s2.task_type, s3.task_type, "other") AS task_type,
    s1.id AS id
  FROM non_embedded_toplevel_slices s1
  LEFT JOIN scheduler_tasks_with_mojo s2 ON s2.id = s1.id
  LEFT JOIN scheduler_tasks_with_full_names s3 ON s3.id = s1.id
  LEFT JOIN java_views_tasks s4 ON s4.id = s1.id
  LEFT JOIN navigation_tasks s5 ON s5.id = s1.id
)
-- Merge slices from toplevel and Java categories.
SELECT * FROM non_embedded_toplevel_slices_with_full_name
UNION ALL
SELECT * FROM non_embedded_java_slices;

DROP VIEW IF EXISTS chrome_tasks;
CREATE VIEW chrome_tasks AS
SELECT
  full_name,
  task_type,
  thread.name AS thread_name,
  thread.utid,
  process.name AS process_name,
  thread.upid,
  ts.*
FROM chrome_tasks_internal cti
JOIN {{slice_table_name}} ts USING (id)
JOIN thread_track tt ON ts.track_id = tt.id
JOIN thread USING (utid)
JOIN process USING (upid);

-- A helper view into Chrome thread slices which don't have a parent task. 
-- TODO(altimin): Use chrome_thread here once it's reliable.
DROP VIEW IF EXISTS chrome_slices_without_parent_task;
CREATE VIEW chrome_slices_without_parent_task AS
SELECT
  s1.*
FROM {{slice_table_name}} s1
LEFT JOIN chrome_tasks s2 USING (id)
WHERE
  (SELECT count()
          FROM ancestor_slice(s1.id) s3
          JOIN chrome_tasks s4 ON s3.id = s4.id) = 0
  AND s2.id IS NULL;
