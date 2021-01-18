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
--
-- While handling a InputLatency::GestureScrollUpdate event a sequence of Flows
-- define the critical path from Beginning to End. This metric breaks down the
-- flows for the same InputLatency::GestureScrollUpdate event.
--
-- WARNING: This metric should not be used as a source of truth. It is under
--          active development and the values & meaning might change without
--          notice.

-- Provides the scroll_jank table which gives us all the GestureScrollUpdate
-- events we care about and labels them janky or not.
SELECT RUN_METRIC('chrome/scroll_jank.sql');

-- We get all latency_info that have valid trace_ids, And we make a synthetic
-- one for the beginning of each GestureScrollUpdate event so we can track the
-- time between receiving the input and being converted into a scroll.
--
-- flows with a trace_id of -1 are incomplete and are difficult to reason about
-- (especially if GestureScrollUpdate flows end up getting -1). so ignore them
-- for this table.
DROP VIEW IF EXISTS latency_info_flow_step;
CREATE VIEW latency_info_flow_step AS
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
      COALESCE(ancestor.id, slice.id) AS ancestor_id,
      COALESCE(ancestor.ts, slice.ts) AS ancestor_ts,
      COALESCE(ancestor.dur, slice.dur) AS ancestor_dur
    FROM
      slice LEFT JOIN
      ancestor_slice(slice.id) AS ancestor ON ancestor.depth = 0
    WHERE
      slice.name = 'LatencyInfo.Flow' AND
      EXTRACT_ARG(slice.arg_set_id, 'chrome_latency_info.trace_id') != -1
  ) flow JOIN (
      SELECT
        id AS scroll_slice_id,
        ts AS scroll_ts,
        dur AS scroll_dur,
        track_id AS scroll_track_id,
        trace_id AS scroll_trace_id,
        jank,
        gesture_scroll_id
      FROM scroll_jank
  ) scroll ON
    flow.trace_id = scroll.scroll_trace_id
  UNION ALL
  SELECT
    'InputLatency::GestureScrollUpdate' AS name,
    id,
    ts,
    dur,
    track_id,
    trace_id,
    'AsyncBegin' AS step,
    id AS ancestor_id,
    ts AS ancestor_ts,
    0 AS ancestor_dur,
    id AS scroll_slice_id,
    ts AS scroll_ts,
    dur AS scroll_dur,
    track_id AS scroll_track_id,
    trace_id AS scroll_trace_id,
    jank,
    gesture_scroll_id
  FROM scroll_jank
  ORDER BY gesture_scroll_id ASC, trace_id ASC, ts ASC;

-- This is a heuristic to figure out which flow event properly joins this
-- GestureScrollUpdate. This heuristic is only needed in traces before we added
-- gesture_scroll_id.
--
-- We select the first |ts| from a flow event after its corresponding
-- GestureScrollUpdate has ended. This allows us to use this |ts| to contain all
-- flow events from the start of a particular scroll_slice_id (the slice id of
-- the async event) to that |ts|.
--
-- The reason for this is if these flow events share the same trace_id which can
-- occur if multiple chrome browsers are in the trace (webview & chrome for
-- example). We would normally add flow events from different scrolls, but by
-- limiting by the GestureScrollUpdate end we can prevent incorrect duplication.
-- This breaks of course if the same trace_id happens at the exact same time in
-- both browsers but this is hopefully unlikely.
DROP VIEW IF EXISTS max_latency_info_ts_per_trace_id;
CREATE VIEW max_latency_info_ts_per_trace_id AS
  SELECT
    scroll_slice_id,
    MIN(ts) AS max_flow_ts
  FROM latency_info_flow_step
  WHERE
    trace_id = scroll_trace_id AND
    ts > scroll_ts + scroll_dur
  GROUP BY scroll_slice_id;

-- As described by the comments about this uses the heuristic to remove any flow
-- events that aren't contained within the |max_flow_ts| and the beginning of
-- the GestureScrollUpdate. This prevents other processes that share the same
-- trace_id from inserting events in the middle.
--
-- Note: Must be a TABLE because it uses a window function which can behave
--       strangely in views.
DROP TABLE IF EXISTS latency_info_flow_step_filtered;
CREATE TABLE latency_info_flow_step_filtered AS
  SELECT
    ROW_NUMBER() OVER (ORDER BY
      flow.gesture_scroll_id ASC, trace_id ASC, ts ASC) AS row_number,
    *
  FROM
    latency_info_flow_step flow JOIN
    max_latency_info_ts_per_trace_id max_flow on
    max_flow.scroll_slice_id = flow.scroll_slice_id
  WHERE
    ts >= scroll_ts AND
    ts <= max_flow_ts
  ORDER BY flow.gesture_scroll_id ASC, flow.trace_id ASC, flow.ts ASC;

-- Take all the LatencyInfo.Flow events and within a |trace_id| join it with the
-- previous and nextflows. Some events are 'Unknown' when they don't have a step
-- but occur in the middle of the critical path. Most of these are errors though
-- and we've weeded I think all of them out (citation needed).
--
-- Note: Must be a TABLE because it uses a window function which can behave
--       strangely in views.
DROP TABLE IF EXISTS latency_info_flow_null_step_removed;
CREATE TABLE latency_info_flow_null_step_removed AS
  SELECT
    ROW_NUMBER() OVER (ORDER BY
      curr.gesture_scroll_id ASC, curr.trace_id ASC, curr.ts ASC
    ) AS row_number,
    curr.id,
    curr.ts,
    curr.dur,
    curr.track_id,
    curr.trace_id,
    curr.gesture_scroll_id,
    curr.scroll_slice_id,
    curr.scroll_ts,
    curr.scroll_dur,
    curr.scroll_track_id,
    curr.jank,
    curr.ancestor_id,
    curr.ancestor_ts,
    curr.ancestor_dur,
    curr.ancestor_ts + curr.ancestor_dur AS ancestor_end,
    CASE WHEN curr.step IS NULL THEN
      CASE WHEN
          prev.gesture_scroll_id != curr.gesture_scroll_id OR
          prev.trace_id != curr.trace_id OR
          prev.trace_id IS NULL OR
          prev.step = 'AsyncBegin' THEN
        'Begin'
      ELSE
        CASE WHEN
            next.gesture_scroll_id != curr.gesture_scroll_id OR
            next.trace_id != curr.trace_id OR
            next.trace_id IS NULL THEN
          'End'
        ELSE
         'Unknown'
        END
      END
    ELSE curr.step END AS step
  FROM
    latency_info_flow_step_filtered curr LEFT JOIN
    latency_info_flow_step_filtered prev ON
      curr.row_number - 1 = prev.row_number LEFT JOIN
    latency_info_flow_step_filtered next ON
      curr.row_number + 1 = next.row_number
  ORDER BY curr.gesture_scroll_id ASC, curr.trace_id ASC, curr.ts ASC;

-- Now that we've got the steps all named properly we want to join them with the
-- next step so we can compute the difference between the end of the current
-- step and the beginning of the next step.
DROP VIEW IF EXISTS scroll_flow_event;
CREATE VIEW scroll_flow_event AS
  SELECT
    curr.trace_id,
    curr.id,
    curr.ts,
    curr.dur,
    curr.track_id,
    curr.gesture_scroll_id,
    curr.scroll_slice_id,
    curr.scroll_ts,
    curr.scroll_dur,
    curr.scroll_track_id,
    curr.jank,
    curr.step,
    curr.ancestor_id,
    curr.ancestor_ts,
    curr.ancestor_dur,
    curr.ancestor_end,
    next.id as next_id,
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
    latency_info_flow_null_step_removed curr LEFT JOIN
    latency_info_flow_null_step_removed next ON
    curr.row_number + 1 = next.row_number
  ORDER BY curr.gesture_scroll_id, curr.trace_id, curr.ts;
