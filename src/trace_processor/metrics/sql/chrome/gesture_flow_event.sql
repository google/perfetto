--
-- Copyright 2021 The Android Open Source Project
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
--
-- While handling a InputLatency::{{gesture_update}} event a sequence of Flows
-- define the critical path from Beginning to End. This metric breaks down the
-- flows for the same InputLatency::{{gesture_update}} event.
--
-- WARNING: This metric should not be used as a source of truth. It is under
--          active development and the values & meaning might change without
--          notice.

-- Provides the {{prefix}}_jank table which gives us all the {{gesture_update}}
-- events we care about and labels them janky or not.
SELECT RUN_METRIC('chrome/{{prefix}}_jank.sql');

-- We get all latency_info that have valid trace_ids, And we make a synthetic
-- one for the beginning of each {{gesture_update}} event so we can track the
-- time between receiving the input and being converted into a gesture.
--
-- flows with a trace_id of -1 are incomplete and are difficult to reason about
-- (especially if {{gesture_update}} flows end up getting -1). so ignore them
-- for this table.
DROP VIEW IF EXISTS {{prefix}}_latency_info_flow_step_and_ancestors;
CREATE VIEW {{prefix}}_latency_info_flow_step_and_ancestors AS
SELECT
  *
FROM (
    SELECT
      slice.name,
      slice.id,
      slice.ts,
      slice.dur,
      slice.track_id,
      EXTRACT_ARG(slice.arg_set_id, 'chrome_latency_info.trace_id') AS trace_id,
      EXTRACT_ARG(slice.arg_set_id, 'chrome_latency_info.step') AS step,
      COALESCE(ancestor_zero.name, slice.name) AS ancestor_name_zero,
      COALESCE(ancestor_zero.id, slice.id) AS ancestor_id_zero,
      COALESCE(ancestor_zero.ts, slice.ts) AS ancestor_ts_zero,
      COALESCE(ancestor_zero.dur, slice.dur) AS ancestor_dur_zero,
      COALESCE(ancestor_one.name, slice.name) AS ancestor_name_one,
      COALESCE(ancestor_one.id, slice.id) AS ancestor_id_one,
      COALESCE(ancestor_one.ts, slice.ts) AS ancestor_ts_one,
      COALESCE(ancestor_one.dur, slice.dur) AS ancestor_dur_one
    FROM
      slice LEFT JOIN
      ancestor_slice(slice.id) AS ancestor_zero
      ON ancestor_zero.depth = 0 LEFT JOIN
      ancestor_slice(slice.id) AS ancestor_one ON ancestor_one.depth = 1
    WHERE
      slice.name = 'LatencyInfo.Flow'
      AND EXTRACT_ARG(slice.arg_set_id, 'chrome_latency_info.trace_id') != -1
  ) flow JOIN (
    SELECT
      id AS gesture_slice_id,
      ts AS gesture_ts,
      dur AS {{prefix}}_dur,
      track_id AS gesture_track_id,
      trace_id AS {{prefix}}_trace_id,
      jank,
      {{id_field}},
      avg_vsync_interval
    FROM {{prefix}}_jank
  ) gesture ON
    flow.trace_id = gesture.{{prefix}}_trace_id
UNION ALL
SELECT
  'InputLatency::{{gesture_update}}' AS name,
  id,
  ts,
  dur,
  track_id,
  trace_id,
  'AsyncBegin' AS step,
  'InputLatency::{{gesture_update}}' AS ancestor_name_zero,
  id AS ancestor_id_zero,
  ts AS ancestor_ts_zero,
  0 AS ancestor_dur_zero,
  'InputLatency::{{gesture_update}}' AS ancestor_name_one,
  id AS ancestor_id_one,
  ts AS ancestor_ts_one,
  0 AS ancestor_dur_one,
  id AS gesture_slice_id,
  ts AS gesture_ts,
  dur AS {{prefix}}_dur,
  track_id AS gesture_track_id,
  trace_id AS {{prefix}}_trace_id,
  jank,
  {{id_field}},
  avg_vsync_interval
FROM {{prefix}}_jank
ORDER BY {{id_field}} ASC, trace_id ASC, ts ASC;

-- See b/184134310, but "ThreadController active" spans multiple tasks and when
-- the top level parent is this event we should use the second event instead.
DROP VIEW IF EXISTS {{prefix}}_latency_info_flow_step;
CREATE VIEW {{prefix}}_latency_info_flow_step AS
SELECT
  *,
  CASE WHEN ancestor_name_zero != "ThreadController active" THEN
      ancestor_name_zero ELSE ancestor_name_one END AS ancestor_name,
  CASE WHEN ancestor_name_zero != "ThreadController active" THEN
      ancestor_id_zero ELSE ancestor_id_one END AS ancestor_id,
  CASE WHEN ancestor_name_zero != "ThreadController active" THEN
      ancestor_ts_zero ELSE ancestor_ts_one END AS ancestor_ts,
  CASE WHEN ancestor_name_zero != "ThreadController active" THEN
      ancestor_dur_zero ELSE ancestor_dur_one END AS ancestor_dur
FROM {{prefix}}_latency_info_flow_step_and_ancestors;

-- This is a heuristic to figure out which flow event properly joins this
-- {{gesture_update}}. This heuristic is only needed in traces before we added
-- {{id_field}}.
--
-- We select the first |ts| from a flow event after its corresponding
-- {{gesture_update}} has ended. This allows us to use this |ts| to contain all
-- flow events from the start of a particular gesture_slice_id (the slice id of
-- the async event) to that |ts|.
--
-- The reason for this is if these flow events share the same trace_id which can
-- occur if multiple chrome browsers are in the trace (webview & chrome for
-- example). We would normally add flow events from different gestures, but by
-- limiting by the {{gesture_update}} end we can prevent incorrect duplication.
-- This breaks of course if the same trace_id happens at the exact same time in
-- both browsers but this is hopefully unlikely.
DROP VIEW IF EXISTS {{prefix}}_max_latency_info_ts_per_trace_id;
CREATE VIEW {{prefix}}_max_latency_info_ts_per_trace_id AS
SELECT
  gesture_slice_id,
  MIN(ts) AS max_flow_ts
FROM {{prefix}}_latency_info_flow_step
WHERE
  trace_id = {{prefix}}_trace_id
  AND ts > gesture_ts + {{prefix}}_dur
GROUP BY gesture_slice_id;

-- As described by the comments about this uses the heuristic to remove any flow
-- events that aren't contained within the |max_flow_ts| and the beginning of
-- the {{gesture_update}}. This prevents other processes that share the same
-- trace_id from inserting events in the middle.
--
-- Note: Must be a TABLE because it uses a window function which can behave
--       strangely in views.
DROP TABLE IF EXISTS {{prefix}}_latency_info_flow_step_filtered;
CREATE TABLE {{prefix}}_latency_info_flow_step_filtered AS
SELECT
  ROW_NUMBER() OVER (ORDER BY
      flow.{{id_field}} ASC, trace_id ASC, ts ASC) AS row_number,
  *
FROM
  {{prefix}}_latency_info_flow_step flow JOIN
  {{prefix}}_max_latency_info_ts_per_trace_id max_flow ON
    max_flow.gesture_slice_id = flow.gesture_slice_id
WHERE
  ts >= gesture_ts
  AND ts <= max_flow_ts
ORDER BY flow.{{id_field}} ASC, flow.trace_id ASC, flow.ts ASC;

-- Take all the LatencyInfo.Flow events and within a |trace_id| join it with the
-- previous and nextflows. Some events are 'Unknown' when they don't have a step
-- but occur in the middle of the critical path. Most of these are errors though
-- and we've weeded I think all of them out (citation needed).
--
-- Note: Must be a TABLE because it uses a window function which can behave
--       strangely in views.
DROP TABLE IF EXISTS {{prefix}}_latency_info_flow_null_step_removed;
CREATE TABLE {{prefix}}_latency_info_flow_null_step_removed AS
SELECT
  ROW_NUMBER() OVER (ORDER BY
      curr.{{id_field}} ASC, curr.trace_id ASC, curr.ts ASC
    ) AS row_number,
    curr.id,
    curr.ts,
    curr.dur,
    curr.track_id,
    curr.trace_id,
    curr.{{id_field}},
    curr.avg_vsync_interval,
    curr.gesture_slice_id,
    curr.gesture_ts,
    curr.{{prefix}}_dur,
    curr.gesture_track_id,
    curr.jank,
    curr.ancestor_id,
    curr.ancestor_ts,
    curr.ancestor_dur,
    curr.ancestor_ts + curr.ancestor_dur AS ancestor_end,
    COALESCE(
      curr.step,
      CASE WHEN
          prev.{{id_field}} != curr.{{id_field}}
          OR prev.trace_id != curr.trace_id
          OR prev.trace_id IS NULL
          OR prev.step = 'AsyncBegin' THEN
        'Begin'
      ELSE
        CASE WHEN
          next.{{id_field}} != curr.{{id_field}}
          OR next.trace_id != curr.trace_id
          OR next.trace_id IS NULL THEN
          'End'
          ELSE
            'Unknown'
        END
      END
    ) AS step
  FROM
    {{prefix}}_latency_info_flow_step_filtered curr LEFT JOIN
    {{prefix}}_latency_info_flow_step_filtered prev ON
      curr.row_number - 1 = prev.row_number LEFT JOIN
    {{prefix}}_latency_info_flow_step_filtered next ON
      curr.row_number + 1 = next.row_number
  ORDER BY curr.{{id_field}} ASC, curr.trace_id ASC, curr.ts ASC;

-- Now that we've got the steps all named properly we want to join them with the
-- next step so we can compute the difference between the end of the current
-- step and the beginning of the next step.
DROP VIEW IF EXISTS {{prefix}}_flow_event;
CREATE VIEW {{prefix}}_flow_event AS
SELECT
  curr.trace_id,
  curr.id,
  curr.ts,
  curr.dur,
  curr.track_id,
  curr.{{id_field}},
  curr.avg_vsync_interval,
  curr.gesture_slice_id AS {{prefix}}_slice_id,
  curr.gesture_ts AS {{prefix}}_ts,
  curr.{{prefix}}_dur AS {{prefix}}_dur,
  curr.gesture_track_id AS {{prefix}}_track_id,
  curr.jank,
  curr.step,
  curr.ancestor_id,
  curr.ancestor_ts,
  curr.ancestor_dur,
  curr.ancestor_end,
  next.id AS next_id,
  next.ts AS next_ts,
  next.dur AS next_dur,
  next.track_id AS next_track_id,
  next.trace_id AS next_trace_id,
  next.step AS next_step,
  CASE WHEN next.trace_id = curr.trace_id THEN
      next.ancestor_ts
    ELSE
      NULL
  END AS maybe_next_ancestor_ts
FROM
  {{prefix}}_latency_info_flow_null_step_removed curr LEFT JOIN
  {{prefix}}_latency_info_flow_null_step_removed next ON
    curr.row_number + 1 = next.row_number
ORDER BY curr.{{id_field}}, curr.trace_id, curr.ts;
