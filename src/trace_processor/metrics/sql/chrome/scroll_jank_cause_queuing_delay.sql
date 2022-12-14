--
-- Copyright 2020 The Android Open Source Project
--
-- Licensed under the Apache License, Version 2.0 (the 'License');
-- you may not use this file except in compliance with the License.
-- You may obtain a copy of the License at
--
--     https://www.apache.org/licenses/LICENSE-2.0
--
-- Unless required by applicable law or agreed to in writing, software
-- distributed under the License is distributed on an 'AS IS' BASIS,
-- WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
-- See the License for the specific language governing permissions and
-- limitations under the License.

SELECT RUN_METRIC('chrome/chrome_thread_slice.sql');
SELECT RUN_METRIC('chrome/scroll_flow_event_queuing_delay.sql');

-- See b/184134310 why we remove ThreadController active.
DROP VIEW IF EXISTS blocking_tasks_no_threadcontroller_active;
CREATE VIEW blocking_tasks_no_threadcontroller_active AS
SELECT
  slice.*,
  ancestor.id AS task_ancestor_id,
  ancestor.name AS task_ancestor_name
FROM
  chrome_thread_slice AS slice LEFT JOIN
  ancestor_slice(slice.id) AS ancestor ON ancestor.id = slice.parent_id
WHERE
  slice.name != "ThreadController active"
  AND (slice.depth = 0 OR ancestor.name = "ThreadController active");

-- Sort track ids to optimize joining with slices
-- as engine doesn't do the sort to join in O(LogN)
-- per row by default
-- TODO(243897379): switch this back to a view once we understand why rolling SQLite to
-- 3.39.2 causes slowdowns.
DROP TABLE IF EXISTS chrome_annotated_threads_and_processes;
CREATE TABLE chrome_annotated_threads_and_processes AS
SELECT
  thread_track.id AS track_id,
  chrome_thread.canonical_name AS thread_name,
  chrome_process.process_type AS process_name
FROM
  thread_track JOIN
  chrome_thread JOIN
  chrome_process ON
    thread_track.utid = chrome_thread.utid
    AND chrome_thread.upid = chrome_process.upid
ORDER BY
  track_id ASC;

-- See b/166441398 & crbug/1094361 for why we remove threadpool (originally
-- the -to-End step). In essence -to-End is often reported on the ThreadPool
-- after the fact with explicit timestamps so it being blocked isn't noteworthy.
-- TODO(243897379): switch this back to a view once we understand why rolling SQLite to
-- 3.39.2 causes slowdowns.
DROP TABLE IF EXISTS blocking_chrome_tasks_without_threadpool;
CREATE TABLE blocking_chrome_tasks_without_threadpool AS
SELECT
  slice.*,
  annotations.thread_name AS thread_name,
  annotations.process_name AS process_name
FROM
  blocking_tasks_no_threadcontroller_active AS slice JOIN
  chrome_annotated_threads_and_processes AS annotations ON
    annotations.track_id = slice.track_id
WHERE
  NOT(thread_name GLOB "*ThreadPool*");

-- This view grabs any slice that could have prevented any GestureScrollUpdate
-- flow event from being run (queuing delays). For RunTask we know that its
-- generic (and thus hard to figure out whats the cause) so we grab the src
-- location to make it more meaningful.
--
-- See b/184134310 for why we allow depth = 1 and ancestor.id is null (which
-- implies its a "ThreadController active" slice because we removed it
-- previously).
DROP TABLE IF EXISTS blocking_tasks_queuing_delay;
CREATE TABLE blocking_tasks_queuing_delay AS
SELECT
  EXTRACT_ARG(slice.arg_set_id, "task.posted_from.file_name") AS file,
  EXTRACT_ARG(slice.arg_set_id, "task.posted_from.function_name") AS function,
  trace_id,
  queuing_time_ns,
  avg_vsync_interval,
  next_track_id,
  CASE WHEN queuing.ancestor_end <= slice.ts THEN
      CASE WHEN slice.ts + slice.dur <= queuing.maybe_next_ancestor_ts THEN
        slice.dur
        ELSE
          queuing.maybe_next_ancestor_ts - slice.ts
      END
    ELSE
      CASE WHEN slice.ts + slice.dur <= queuing.maybe_next_ancestor_ts THEN
        slice.ts + slice.dur - queuing.ancestor_end
        ELSE
          queuing.maybe_next_ancestor_ts - queuing.ancestor_end
      END
  END AS dur_overlapping_ns,
  description,
  scroll_slice_id,
  scroll_ts,
  scroll_dur,
  scroll_track_id,
  jank,
  slice.*
FROM
  scroll_flow_event_queuing_delay queuing JOIN
  blocking_chrome_tasks_without_threadpool AS slice ON
    slice.ts + slice.dur > queuing.ancestor_end
    AND queuing.maybe_next_ancestor_ts > slice.ts
    AND slice.track_id = queuing.next_track_id
WHERE
  queuing_time_ns IS NOT NULL
  AND queuing_time_ns > 0;

-- Now for each toplevel task (depth = 0 from above) we want to grab all their
-- children slices. This is done by joining on descendant_slice which is a
-- trace processor defined operator. This will results in 1 row for every
-- descendant slice. So all fields in base.* will be repeated ONCE for each
-- child, but if it has no slice it will occur only once but all the
-- |descendant_.*| fields will be NULL because of the LEFT JOIN.
-- Additionally for mojo events we replace the descendant_name with just the
-- "interface_name" since that is more descriptive for our jank purposes.
DROP VIEW IF EXISTS all_descendant_blocking_tasks_queuing_delay;
CREATE VIEW all_descendant_blocking_tasks_queuing_delay AS
SELECT
  descendant.id AS descendant_id,
  descendant.ts AS descendant_ts,
  descendant.dur AS descendant_dur,
  COALESCE(
    IIF(descendant.arg_set_id IS NOT NULL,
      EXTRACT_ARG(descendant.arg_set_id,
        "chrome_mojo_event_info.watcher_notify_interface_tag"),
      NULL),
    IIF(descendant.arg_set_id IS NOT NULL,
      EXTRACT_ARG(descendant.arg_set_id,
        "chrome_mojo_event_info.mojo_interface_tag"),
      NULL),
    descendant.name) AS descendant_name,
  EXTRACT_ARG(descendant.arg_set_id,
    "chrome_mojo_event_info.ipc_hash") AS descendant_ipc_hash,
  descendant.parent_id AS descendant_parent_id,
  descendant.depth AS descendant_depth,
  descendant.category AS descendant_category,
  base.*
FROM
  blocking_tasks_queuing_delay base LEFT JOIN
  descendant_slice(base.id) AS descendant;

DROP TABLE IF EXISTS all_descendant_blocking_tasks_queuing_delay_with_cpu_time;
CREATE TABLE all_descendant_blocking_tasks_queuing_delay_with_cpu_time AS
SELECT
  cpu.thread_dur AS descendant_thread_dur,
  CAST(cpu.thread_dur AS REAL) / descendant.thread_dur
  AS descendant_cpu_percentage,
  CAST(cpu.thread_dur AS REAL)
  / (descendant.thread_dur
    / (1 << (descendant.descendant_depth - 1))) > 0.5
  AS descendant_cpu_time_above_relative_threshold,
  descendant_dur / descendant.dur AS descendant_dur_percentage,
  descendant_dur
  / (descendant.dur / (1 << (descendant.descendant_depth - 1))) > 0.5
  AS descendant_dur_above_relative_threshold,
  descendant.*
FROM
  all_descendant_blocking_tasks_queuing_delay descendant LEFT JOIN (
    SELECT
      id, thread_dur
    FROM chrome_thread_slice
  ) AS cpu ON
    cpu.id = descendant.descendant_id;

-- Now that we've generated the descendant count how many siblings each row
-- has. Recall that all the top level tasks are repeated but each row represents
-- a descendant slice. This means since we LEFT JOIN we will say a slice has 0
-- siblings if it has no descendants (which is true), and otherwise we will
-- compute the siblings as the count of all slices with the same parent minus
-- the current slice.
DROP VIEW IF EXISTS counted_descendant_blocking_tasks_queuing_delay;
CREATE VIEW counted_descendant_blocking_tasks_queuing_delay AS
SELECT
  base.*,
  COALESCE(single_descendant.number_of_siblings, 0) AS number_of_siblings
FROM
  all_descendant_blocking_tasks_queuing_delay_with_cpu_time base LEFT JOIN (
    SELECT
      descendant_parent_id,
      COUNT(*) - 1 AS number_of_siblings
    FROM all_descendant_blocking_tasks_queuing_delay_with_cpu_time
    WHERE descendant_parent_id IS NOT NULL
    GROUP BY 1
  ) single_descendant ON
    single_descendant.descendant_parent_id = base.descendant_parent_id;

-- Now we group by the |id| which is the top level task id and find the first
-- descendant_depth where we have a sibling. We need this because we only want
-- to include single descendant slices in our metric name to keep it easy to
-- reason about what that code is doing.
DROP VIEW IF EXISTS blocking_tasks_queuing_delay_with_invalid_depth;
CREATE VIEW blocking_tasks_queuing_delay_with_invalid_depth AS
SELECT
  base.*,
  (
    descendant_cpu_time_above_relative_threshold
    AND descendant_cpu_percentage > 0.05
  ) OR (
    descendant_dur_above_relative_threshold
    AND descendant_dur_percentage > 0.05
  ) AS descendant_major_slice,
  COALESCE(depth.invalid_depth, 10) AS invalid_depth
FROM
  counted_descendant_blocking_tasks_queuing_delay base LEFT JOIN (
    SELECT
      id,
      MIN(descendant_depth) AS invalid_depth
    FROM counted_descendant_blocking_tasks_queuing_delay
    WHERE number_of_siblings >= 1
    GROUP BY 1
  ) AS depth ON base.id = depth.id
ORDER BY
  descendant_depth ASC,
  descendant_cpu_percentage DESC,
  descendant_dur_percentage DESC;

-- Now to get back to a single output per top level task we group by all the
-- toplevel fields and aggregate the descendant fields. We only include the
-- descendant if their depth is less than the first depth with siblings (the
-- |invalid_depth|).
DROP VIEW IF EXISTS descendant_blocking_tasks_queuing_delay;
CREATE VIEW descendant_blocking_tasks_queuing_delay AS
SELECT
  id,
  ts,
  dur,
  track_id,
  trace_id,
  name,
  category,
  scroll_slice_id AS scroll_id,
  scroll_ts,
  scroll_dur,
  scroll_track_id,
  jank,
  queuing_time_ns,
  dur_overlapping_ns,
  description,
  replace(file, rtrim(file, replace(file, '/', '')), '') AS file,
  thread_name,
  process_name,
  function,
  avg_vsync_interval,
  GROUP_CONCAT(
    CASE WHEN descendant_depth < invalid_depth OR descendant_major_slice THEN
        descendant_id
      ELSE
        NULL
    END,
    "-") AS descendant_id,
  GROUP_CONCAT(
    CASE WHEN descendant_depth < invalid_depth OR descendant_major_slice THEN
        descendant_ts
      ELSE
        NULL
    END,
    "-") AS descendant_ts,
  GROUP_CONCAT(
    CASE WHEN descendant_depth < invalid_depth OR descendant_major_slice THEN
        descendant_dur
      ELSE
        NULL
    END,
    "-") AS descendant_dur,
  GROUP_CONCAT(
    CASE WHEN descendant_depth < invalid_depth OR descendant_major_slice THEN
        descendant_name
      ELSE
        NULL
    END, "-") AS descendant_name,
  GROUP_CONCAT(
    CASE WHEN descendant_depth < invalid_depth OR descendant_major_slice THEN
        descendant_thread_dur
      ELSE
        NULL
    END,
    "-") AS descendant_thread_dur,
  GROUP_CONCAT(
    CASE WHEN descendant_depth < invalid_depth OR descendant_major_slice THEN
        descendant_cpu_percentage
      ELSE
        NULL
    END,
    "-") AS descendant_cpu_time,
  GROUP_CONCAT(
    CASE WHEN descendant_category = "mojom" THEN
        descendant_name
      ELSE
        NULL
    END,
    "-") AS mojom_name,
  -- All ipc_hashes should be equal so just select the first non-null one.
  MIN(descendant_ipc_hash) AS mojom_ipc_hash,
  GROUP_CONCAT(
    CASE WHEN
        descendant_category = "toplevel"
        AND descendant_name NOT GLOB "*ThreadController*" THEN
        descendant_name
      ELSE
        NULL
    END,
    "-") AS toplevel_name,
  GROUP_CONCAT(
    CASE WHEN descendant_category = "Java" THEN
        descendant_name
      ELSE
        NULL
    END,
    "-") AS java_name
FROM
  blocking_tasks_queuing_delay_with_invalid_depth
GROUP BY 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20
ORDER BY descendant_cpu_percentage DESC;

SELECT CREATE_FUNCTION(
  -- Function prototype: takes a '-' separated list of slice names (formed by
  -- the GROUP_CONCAT above) and returns the first slice if any or NULL
  -- otherwise.
  'GetFirstSliceNameOrNull(name STRING)',
  -- Returns the first slice name or NULL
  'STRING',
  -- Preforms the actual string modification, takes the either the whole string
  -- if there is no '-' or up to the first '-'. SUBSTR returns NULL if $name is
  -- NULL.
  'SELECT SUBSTR($name, 0,
    CASE WHEN INSTR($name, "-") = 0 THEN
      LENGTH($name)+1 ELSE
      INSTR($name, "-")
    END)'
);

SELECT CREATE_FUNCTION(
  -- Function prototype: takes a '-' separated list of slice names (formed by
  -- the GROUP_CONCAT above) and checks for certain important view names and
  -- falls back on GetFirstSliceNameOrNull if it can't find one.
  'GetJavaSliceSummaryOrNull(name STRING)',
  -- Returns the summary of the provided list of java slice names.
  'STRING',
  -- Performs a bunch of GLOB matches in an order, now there could be multiple
  -- matches (both Toolbar & TabList could be true) so the order matters in
  -- tagging since we don't support multiple tagging of values. Ideally we would
  -- determine which one was the longest duration, but this should be sufficient
  -- for now.
  'SELECT
    CASE WHEN $name GLOB "*ToolbarControlContainer*" THEN
      "ToolbarControlContainer"
    WHEN $name GLOB "*ToolbarProgressBar*" THEN
      "ToolbarProgressBar"
    WHEN $name GLOB "*TabGroupUiToolbarView*" THEN
      "TabGroupUiToolbarView"
    WHEN $name GLOB "*TabGridThumbnailView*" THEN
      "TabGridThumbnailView"
    WHEN $name GLOB "*TabGridDialogView*" THEN
      "TabGridDialogView"
    WHEN $name GLOB "*BottomContainer*" THEN
      "BottomContainer"
    WHEN $name GLOB "*FeedSwipeRefreshLayout*" THEN
      "FeedSwipeRefreshLayout"
    WHEN $name GLOB "*AutocompleteEditText*" THEN
      "AutocompleteEditText"
    WHEN $name GLOB "*HomeButton*" THEN
      "HomeButton"
    WHEN $name GLOB "*ToggleTabStackButton*" THEN
      "ToggleTabStackButton"
    WHEN $name GLOB "*ListMenuButton*" THEN
      "ListMenuButton"
    WHEN $name GLOB "*ScrimView*" THEN
      "ScrimView"
    WHEN $name GLOB "*ChromeImageView*" THEN
      "ChromeImageView"
    WHEN $name GLOB "*AppCompatImageView*" THEN
      "AppCompatImageView"
    WHEN $name GLOB "*ChromeImageButton*" THEN
      "ChromeImageButton"
    WHEN $name GLOB "*AppCompatImageButton*" THEN
      "AppCompatImageButton"
    WHEN $name GLOB "*TabListRecyclerView*" THEN
      "TabListRecyclerView"
    ELSE
      GetFirstSliceNameOrNull($name)
    END'
);

SELECT CREATE_FUNCTION(
  -- Function prototype: takes slice name, category and descendant_name and
  -- determines if this event should be classified as unknown or not.
  'UnknownEventOrEmptyString(name STRING, cat STRING, has_descendant STRING)',
  -- Returns either "-UnknownEvent" or "".
  'STRING',
  -- If our current event has a posted from we consider it already categorized
  -- even if we don't have events underneath it. If its java often we won't have
  -- sub events, and finally if its a single event we just use its name there
  -- isn't anything under to use so just leave it at that.
  'SELECT
    CASE WHEN
      $name = "ThreadControllerImpl::RunTask" OR
      $cat = "Java" OR
      $has_descendant IS NULL THEN
        "" ELSE
        "-UnknownEvent"
      END'
);

SELECT CREATE_FUNCTION(
  -- Function prototype: Takes a slice name, function, and file, and determines
  -- if we should use the slice name, or if its a RunTask event uses the
  -- function & file name, however if the RunTask posted from is one of the
  -- simple_watcher paths we collapse them for attributation.
  'TopLevelName(name STRING, function STRING, file STRING)',
  'STRING',
  -- The difference for the mojom functions are:
  --  1) PostDispatchNextMessageFromPipe:
  --         We knew that there is a message in the pipe, didn't try to set up a
  --         SimpleWatcher to monitor when a new one arrives.
  --  2) ArmOrNotify:
  --         We tried to set up SimpleWatcher, but the setup failed as the
  --         message arrived as we were setting this up, so we posted a task
  --         instead.
  --  3) Notify:
  --         SimpleWatcher was set up and after a period of monitoring detected
  --         a new message.
  -- For our jank use case this distinction isn't very useful so we group them
  -- together.
  'SELECT
     CASE WHEN $name = "ThreadControllerImpl::RunTask" THEN
       CASE WHEN $function IN
           ("PostDispatchNextMessageFromPipe", "ArmOrNotify", "Notify") THEN
         "posted-from-mojo-pipe"
        ELSE
         "posted-from-" || $function || "()-in-" || $file
        END
    ELSE
      $name
    END'
);

-- Create a common name for each "cause" based on the slice stack we found.
DROP VIEW IF EXISTS scroll_jank_cause_queuing_delay_temp;
CREATE VIEW scroll_jank_cause_queuing_delay_temp AS
SELECT
  TopLevelName(name, function, file) || COALESCE(
    "-" || descendant_name, "") AS location,
  TopLevelName(name, function, file) || COALESCE(
    "-" || GetFirstSliceNameOrNull(mojom_name)
    || COALESCE("(ipc=" || mojom_ipc_hash || ")", ""),
    "-" || GetFirstSliceNameOrNull(toplevel_name)
    || COALESCE("(ipc=" || mojom_ipc_hash || ")", ""),
    "-" || GetJavaSliceSummaryOrNull(java_name),
    UnknownEventOrEmptyString(name, category, descendant_name)
  ) AS restricted_location,
  base.*
FROM descendant_blocking_tasks_queuing_delay base;

-- Figure out the average time taken during non-janky scrolls updates for each
-- TraceEvent (metric_name) stack.
DROP VIEW IF EXISTS scroll_jank_cause_queuing_delay_average_no_jank_time;
CREATE VIEW scroll_jank_cause_queuing_delay_average_no_jank_time AS
SELECT
  location,
  AVG(dur_overlapping_ns) AS avg_dur_overlapping_ns
FROM scroll_jank_cause_queuing_delay_temp
WHERE NOT jank
GROUP BY 1;

-- Again figure out the average time, but based on a more restricted set of
-- trace events.
DROP VIEW IF EXISTS scroll_jank_cause_queuing_delay_average_no_jank_time_restricted;
CREATE VIEW scroll_jank_cause_queuing_delay_average_no_jank_time_restricted AS
SELECT
  restricted_location,
  AVG(dur_overlapping_ns) AS avg_dur_overlapping_ns_restricted
FROM scroll_jank_cause_queuing_delay_temp
WHERE NOT jank
GROUP BY 1;


-- Join every row (jank and non-jank with the average non-jank time for the
-- given metric_name).
DROP VIEW IF EXISTS scroll_jank_cause_queuing_delay_unannotated;
CREATE VIEW scroll_jank_cause_queuing_delay_unannotated AS
SELECT
  base.*,
  'InputLatency.LatencyInfo.Flow.QueuingDelay.'
  || CASE WHEN jank THEN 'Jank' ELSE 'NoJank' END || '.BlockingTasksUs.'
  || base.location AS metric_name,
  COALESCE(avg_no_jank.avg_dur_overlapping_ns, 0)
  AS avg_no_jank_dur_overlapping_ns
FROM
  scroll_jank_cause_queuing_delay_temp base LEFT JOIN
  scroll_jank_cause_queuing_delay_average_no_jank_time avg_no_jank ON
    base.location = avg_no_jank.location;

-- Join in the restricted set of trace events average as well to form the final output.
DROP VIEW IF EXISTS scroll_jank_cause_queuing_delay;
CREATE VIEW scroll_jank_cause_queuing_delay AS
SELECT
  base.*,
  'QueuingDelay.'
  || CASE WHEN jank THEN 'Jank' ELSE 'NoJank' END || '.BlockingTasksUs.'
  || base.restricted_location AS restricted_metric_name,
  COALESCE(avg_no_jank.avg_dur_overlapping_ns_restricted, 0)
  AS avg_no_jank_dur_overlapping_ns_restricted
FROM
  scroll_jank_cause_queuing_delay_unannotated base LEFT JOIN
  scroll_jank_cause_queuing_delay_average_no_jank_time_restricted avg_no_jank ON
    base.restricted_location = avg_no_jank.restricted_location;
