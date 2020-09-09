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

SELECT RUN_METRIC('chrome/scroll_flow_event_queuing_delay.sql');

-- This view grabs any slice that could have prevented any GestureScrollUpdate
-- flow event from being run (queuing delays). For RunTask we know that its
-- generic (and thus hard to figure out whats the cause) so we grab the src
-- location to make it more meaningful.
--
-- See b/166441398 & crbug/1094361 for why we remove the -to-End step. In
-- essence -to-End is often reported on the ThreadPool after the fact with
-- explicit timestamps so it being blocked isn't noteworthy.
DROP TABLE IF EXISTS blocking_tasks_queuing_delay;

CREATE TABLE blocking_tasks_queuing_delay AS
  SELECT
    EXTRACT_ARG(arg_set_id, "task.posted_from.file_name") as file,
    EXTRACT_ARG(arg_set_id, "task.posted_from.function_name") as function,
    trace_id,
    queuing_time_ns,
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
    slice ON
        slice.ts + slice.dur > queuing.ancestor_end AND
        queuing.maybe_next_ancestor_ts > slice.ts AND
        slice.track_id = queuing.next_track_id AND
        slice.depth = 0 AND
        queuing.description NOT LIKE
            "InputLatency.LatencyInfo.%ank.STEP_DRAW_AND_SWAP-to-End"
  WHERE
    queuing_time_ns IS NOT NULL AND
    queuing_time_ns > 0;

-- Now for each toplevel task (depth = 0 from above) we want to grab all their
-- children slices. This is done by joining on descendant_slice which is a
-- trace processor defined operator. This will results in 1 row for every
-- descendant slice. So all fields in base.* will be repeated ONCE for each
-- child, but if it has no slice it will occur only once but all the
-- |descendant_.*| fields will be NULL because of the LEFT JOIN.
DROP VIEW IF EXISTS all_descendant_blocking_tasks_queuing_delay;

CREATE VIEW all_descendant_blocking_tasks_queuing_delay AS
  SELECT
    descendant.id AS descendant_id,
    descendant.ts AS descendant_ts,
    descendant.dur AS descendant_dur,
    descendant.name AS descendant_name,
    descendant.parent_id As descendant_parent_id,
    descendant.depth AS descendant_depth,
    base.*
  FROM
    blocking_tasks_queuing_delay base LEFT JOIN
    descendant_slice(base.id) AS descendant;

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
    all_descendant_blocking_tasks_queuing_delay base LEFT JOIN (
      SELECT
        descendant_parent_id,
        COUNT(*) - 1 AS number_of_siblings
      FROM all_descendant_blocking_tasks_queuing_delay
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
    COALESCE(depth.invalid_depth, 10) AS invalid_depth
  FROM
    counted_descendant_blocking_tasks_queuing_delay base LEFT JOIN (
      SELECT
        id,
        MIN(descendant_depth) AS invalid_depth
      FROM counted_descendant_blocking_tasks_queuing_delay
      WHERE number_of_siblings >= 1
      GROUP BY 1
    ) AS depth ON base.id = depth.id;

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
    scroll_slice_id AS scroll_id,
    scroll_ts,
    scroll_dur,
    scroll_track_id,
    jank,
    queuing_time_ns,
    dur_overlapping_ns,
    description,
    replace(file, rtrim(file, replace(file, '/', '')), '') AS file,
    function,
    GROUP_CONCAT(
      CASE WHEN descendant_depth < invalid_depth THEN
        descendant_id
      ELSE
        NULL
      END
    , "-") AS descendant_id,
    GROUP_CONCAT(
      CASE WHEN descendant_depth < invalid_depth THEN
        descendant_ts
      ELSE
        NULL
      END
    , "-") AS descendant_ts,
    GROUP_CONCAT(
      CASE WHEN descendant_depth < invalid_depth THEN
        descendant_dur
      ELSE
        NULL
      END
    , "-") AS descendant_dur,
    GROUP_CONCAT(
      CASE WHEN descendant_depth < invalid_depth THEN
        descendant_name
      ELSE
        NULL
      END, "-") AS descendant_name
  FROM
    blocking_tasks_queuing_delay_with_invalid_depth
  GROUP BY 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15;

-- Create a common name for each "cause" based on the slice stack we found.
DROP VIEW IF EXISTS scroll_jank_cause_queuing_delay_temp;

CREATE VIEW scroll_jank_cause_queuing_delay_temp AS
  SELECT
    CASE WHEN name = "ThreadControllerImpl::RunTask" THEN
      'posted-from-' || function || '()-in-' || file
    ELSE
      name
    END || COALESCE("-" || descendant_name, "") AS location,

    base.*
  FROM descendant_blocking_tasks_queuing_delay base;

-- Figure out the average time taken during non-janky scrolls updates for each
-- TraceEvent (metric_name) stack.
DROP VIEW IF EXISTS scroll_jank_cause_queuing_delay_average_time;

CREATE VIEW scroll_jank_cause_queuing_delay_average_no_jank_time AS
  SELECT
    location,
    AVG(dur_overlapping_ns) as avg_dur_overlapping_ns
  FROM scroll_jank_cause_queuing_delay_temp
  WHERE NOT jank
  GROUP BY 1;

-- Join every row (jank and non-jank with the average non-jank time for the given
-- metric_name).
DROP VIEW IF EXISTS scroll_jank_cause_queuing_delay;

CREATE VIEW scroll_jank_cause_queuing_delay AS
  SELECT
    base.*,
    'InputLatency.LatencyInfo.Flow.QueuingDelay.' ||
    CASE WHEN jank THEN 'Jank' ELSE 'NoJank' END || '.BlockingTasksUs.' ||
      base.location as metric_name,
    COALESCE(avg_no_jank.avg_dur_overlapping_ns, 0)
        AS avg_no_jank_dur_overlapping_ns
  FROM
    scroll_jank_cause_queuing_delay_temp base LEFT JOIN
    scroll_jank_cause_queuing_delay_average_no_jank_time avg_no_jank ON
        base.location = avg_no_jank.location;
