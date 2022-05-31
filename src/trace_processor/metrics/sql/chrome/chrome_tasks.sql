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

-- Create |chrome_scheduler_tasks| table, which contains a subset of thread_slice
-- table with the slices which correspond to tasks executed by Chrome scheduler.
--
-- |chrome_scheduler_tasks_internal| is the cached table containing scheduler-specific bits.
DROP TABLE IF EXISTS chrome_scheduler_tasks_internal;
CREATE TABLE chrome_scheduler_tasks_internal AS
SELECT
  EXTRACT_ARG(s.arg_set_id, "task.posted_from.file_name") as posted_from_file_name,
  EXTRACT_ARG(s.arg_set_id, "task.posted_from.function_name") as posted_from_function_name,
  (CASE name
    WHEN "ThreadControllerImpl::RunTask" THEN "SequenceManager"
    WHEN "ThreadPool_RunTask" THEN "ThreadPool"
  END) as scheduler_type,
  s.id
FROM thread_slice s
WHERE
  category="toplevel" AND
  (name="ThreadControllerImpl::RunTask" or name="ThreadPool_RunTask")
ORDER BY id;

-- |chrome_scheduler_tasks| is a view over |chrome_scheduler_tasks_internal| table, adding
-- columns from |thread_slice| table.
DROP VIEW IF EXISTS chrome_scheduler_tasks;
CREATE VIEW chrome_scheduler_tasks AS
SELECT
  s1.posted_from_file_name || ":" || s1.posted_from_function_name as posted_from,
  s1.posted_from_file_name,
  s1.posted_from_function_name,
  s1.scheduler_type,
  s2.*
FROM chrome_scheduler_tasks_internal s1
JOIN thread_slice s2 USING (id)
ORDER BY id;

-- Create |chrome_mojo_receive_slices| table, containing a subset of thread_slice
-- table with the slices corresponding to received mojo messages.
--
-- Note: this might include messages received within a sync mojo call.
DROP TABLE IF EXISTS chrome_mojo_slices_internal;
CREATE TABLE chrome_mojo_slices_internal AS
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
      EXTRACT_ARG(s.arg_set_id, "chrome_mojo_event_info.mojo_interface_tag") as interface_name,
      EXTRACT_ARG(arg_set_id, "chrome_mojo_event_info.ipc_hash") as ipc_hash,
      (CASE name
        WHEN "Receive mojo message" THEN "message"
        WHEN "Receive mojo reply" THEN "reply"
      END) as message_type,
      s.id
    FROM slice s
    WHERE
      category="toplevel"
      AND name GLOB 'Receive *'
    ORDER BY id
  ),
  -- Select old-style slices for channel-associated mojo events.
  old_associated_mojo_slices AS (
    SELECT
      s.name as interface_name,
      (select
        EXTRACT_ARG(s2.arg_set_id, "chrome_mojo_event_info.ipc_hash")
       FROM descendant_slice(s.id) s2
       WHERE s2.name="ScopedSetIpcHash"
       ORDER BY s2.id
       LIMIT 1) as ipc_hash,
      "message" as message_type,
      s.id
    FROM thread_slice s
    WHERE
      category="mojom"
      AND name GLOB '*.mojom.*'
    ORDER BY id
  ),
  -- Select old-style slices for non-(channel-associated) mojo events.
  old_non_associated_mojo_slices AS (
    SELECT
      COALESCE(
        EXTRACT_ARG(s.arg_set_id, "chrome_mojo_event_info.watcher_notify_interface_tag"),
        EXTRACT_ARG(s.arg_set_id, "chrome_mojo_event_info.mojo_interface_tag")
      ) as interface_name,
      (select
        EXTRACT_ARG(s2.arg_set_id, "chrome_mojo_event_info.ipc_hash")
       FROM descendant_slice(s.id) s2
       WHERE s2.name="ScopedSetIpcHash"
       ORDER BY s2.id
       LIMIT 1) as ipc_hash,
      "message" as message_type,
      s.id
    FROM thread_slice s
    WHERE
      category="toplevel" and name="Connector::DispatchMessage"
    ORDER BY id
  ),
  -- Merge all mojo slices.
  all_mojo_slices_non_sorted AS (
    SELECT * from new_mojo_slices
    UNION
    SELECT * from old_associated_mojo_slices
    UNION
    SELECT * from old_non_associated_mojo_slices
  )
SELECT *
FROM all_mojo_slices_non_sorted
ORDER BY id;

-- |chrome_mojo_slices| is a view over |chrome_mojo_slices_internal| table, adding
-- columns from |thread_slice| table.
DROP VIEW IF EXISTS chrome_mojo_slices;
CREATE VIEW chrome_mojo_slices AS
SELECT
  s1.interface_name,
  s1.ipc_hash,
  s1.message_type,
  s2.*
FROM chrome_mojo_slices_internal s1
JOIN thread_slice s2 USING (id)
ORDER BY id;

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
      ".Measure", "") as name
    FROM
      thread_slice s1
    where category="Java" and dur > 0
  ),
  -- We filter out generic slices from various UI frameworks which don't tell us much about
  -- what exactly this view is doing.
  interesting_java_slices AS (
    SELECT
      id, name
    FROM java_slices_with_trimmed_names
    WHERE not name in (
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
  (SELECT count()
    FROM ancestor_slice(s1.id) s2
    WHERE s2.name="ViewResourceAdapter:captureWithSoftwareDraw"
  )>0 as is_software_screenshot,
  (SELECT count()
    FROM ancestor_slice(s1.id) s2
    WHERE s2.name="ViewResourceAdapter:captureWithHardwareDraw"
  )>0 as is_hardware_screenshot
FROM interesting_java_slices s1
WHERE (select count()
  from ancestor_slice(s1.id) s2
  join interesting_java_slices s3 on s2.id=s3.id)=0
ORDER BY s1.id;

-- |chrome_java_views| is a view over |chrome_java_views_internal| table, adding the necessary columns
-- from |thread_slice|.
DROP VIEW IF EXISTS chrome_java_views;
CREATE VIEW chrome_java_views AS
SELECT
  s1.name as filtered_name,
  s1.is_software_screenshot,
  s1.is_hardware_screenshot,
  s2.*
FROM chrome_java_views_internal s1
JOIN thread_slice s2 using (id);

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
       END) as kind
    FROM thread_slice
    WHERE
      (name GLOB 'Looper.dispatch: android.view.Choreographer$FrameHandler*') OR
      (name='ThreadControllerImpl::RunTask' AND
        EXTRACT_ARG(arg_set_id, 'task.posted_from.file_name')='cc/trees/single_thread_proxy.cc' AND
        EXTRACT_ARG(arg_set_id, 'task.posted_from.function_name')='ScheduledActionSendBeginMainFrame')
    ORDER BY id
  ),
  -- Intermediate step to allow us to sort java view names.
  root_slice_and_java_view_not_grouped AS (
    SELECT
      s1.id, s1.kind, s3.name as java_view_name
    FROM root_slices s1
    JOIN descendant_slice(s1.id) s2
    JOIN chrome_java_views_internal s3 ON s2.id=s3.id
    ORDER BY s1.id, java_view_name
  )
SELECT
  s1.id,
  s1.kind,
  GROUP_CONCAT(DISTINCT s2.java_view_name) as java_views
FROM root_slices s1
LEFT JOIN root_slice_and_java_view_not_grouped s2 USING (id)
GROUP BY s1.id
ORDER BY s1.id;

-- Create |chrome_tasks| table, which contains a subset of thread_slice
-- table of the slices which should be considered top-level Chrome tasks with the
-- additional scheduler_type |full_name| column, derived from subevents.
DROP TABLE IF EXISTS chrome_tasks_internal;
CREATE TABLE chrome_tasks_internal AS
WITH
  -- Select slices from "toplevel" category which do not have another
  -- "toplevel" slice as ancestor. The possible cases include sync mojo messages
  -- and tasks in nested runloops.
  non_embedded_toplevel_slices AS (
     SELECT * FROM thread_slice s
     WHERE
       category IN ("toplevel", "toplevel,viz")
       AND (SELECT count() FROM ancestor_slice(s.id) s2 
            WHERE s2.category IN ("toplevel", "toplevel.viz"))=0
     ORDER BY id
  ),
  -- Select slices from "Java" category which do not have another "Java" or
  -- "toplevel" slice as parent. In the longer term they should probably belong
  -- to "toplevel" category as well, but for now this will have to do.
  non_embedded_java_slices AS (
    SELECT name as full_name, "java" as task_type, id
    FROM thread_slice s
    WHERE
      category="Java"
      AND (SELECT count()
           FROM ancestor_slice(s.id) s2
           WHERE s2.category="toplevel" or s2.category="Java")=0
    ORDER BY id
  ),
  -- Generate full names for scheduler tasks.
  scheduler_tasks AS (
    SELECT
      printf("RunTask(posted_from=%s)", s.posted_from) as full_name,
      "scheduler" as task_type,
      s.id
    FROM chrome_scheduler_tasks s
    ORDER BY id
  ),
  -- Generate full names for mojo slices.
  mojo_slices AS (
    SELECT
      printf('%s %s (hash=%d)',
        interface_name, message_type, ipc_hash) as full_name,
      "mojo" as task_type,
      id
    FROM chrome_mojo_slices
    ORDER BY id
  ),
  -- Generate full names for tasks with java views.
  java_views_tasks AS (
    SELECT
      printf('%s(java_views=%s)', kind, java_views) as full_name,
      (CASE kind
        WHEN 'Choreographer' THEN 'choreographer'
        WHEN 'SingleThreadProxy::BeginMainFrame' THEN 'ui_thread_begin_main_frame'
       END) as task_type,
      id
    FROM chrome_slices_with_java_views_internal
    ORDER BY id
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
        ORDER BY s2.depth LIMIT 1) as full_name,
      "mojo" as task_type,
      s1.id
    FROM
      chrome_scheduler_tasks s1
    WHERE
      s1.posted_from IN (
        "mojo/public/cpp/system/simple_watcher.cc:Notify",
        "mojo/public/cpp/bindings/lib/connector.cc:PostDispatchNextMessageFromPipe",
        "ipc/ipc_mojo_bootstrap.cc:Accept")
    ORDER BY id
  ),
  -- Add scheduler and mojo full names to non-embedded slices from
  -- the "toplevel" category, with mojo ones taking precedence.
  non_embedded_toplevel_slices_with_full_name AS (
    SELECT
       COALESCE(s4.full_name, s2.full_name, s3.full_name, s1.name) AS full_name,
       COALESCE(s4.task_type, s2.task_type, s3.task_type, "other") as task_type,
       s1.id as id
    FROM non_embedded_toplevel_slices s1
    LEFT JOIN scheduler_tasks_with_mojo s2 ON s2.id=s1.id
    LEFT JOIN scheduler_tasks s3 ON s3.id=s1.id
    LEFT JOIN java_views_tasks s4 ON s4.id=s1.id
    ORDER BY id
  ),
  -- Merge slices from toplevel and Java categories.
  non_sorted_tasks AS (
    SELECT * from non_embedded_toplevel_slices_with_full_name
    UNION ALL
    SELECT * from non_embedded_java_slices
  )
SELECT * FROM non_sorted_tasks
ORDER BY id;

DROP VIEW IF EXISTS chrome_tasks;
CREATE VIEW chrome_tasks AS
SELECT
  full_name,
  task_type,
  thread.name as thread_name,
  thread.utid,
  process.name as process_name,
  thread.upid,
  ts.*
FROM chrome_tasks_internal cti
JOIN thread_slice ts USING (id)
JOIN thread_track tt ON ts.track_id=tt.id
JOIN thread USING (utid)
JOIN process USING (upid)
ORDER BY id;

-- A helper view into Chrome thread slices which don't have a parent task. 
-- TODO(altimin): Use chrome_thread here once it's reliable.
DROP VIEW IF EXISTS chrome_slices_without_parent_task;
CREATE VIEW chrome_slices_without_parent_task AS
SELECT
  s1.*
FROM thread_slice s1
LEFT JOIN chrome_tasks s2 USING (id)
WHERE
  (SELECT count()
   FROM ancestor_slice(s1.id) s3
   JOIN chrome_tasks s4 ON s3.id=s4.id)=0
  and s2.id IS NULL
ORDER BY id;
