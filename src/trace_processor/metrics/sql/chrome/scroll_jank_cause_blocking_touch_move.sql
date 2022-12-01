--
-- Copyright 2020 The Android Open Source Project
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

SELECT RUN_METRIC('chrome/scroll_jank.sql');

-- Below we want to collect TouchMoves and figure out if they blocked any
-- GestureScrollUpdates. This table gets the TouchMove slice and joins it with
-- the data from the first flow event for that TouchMove.
DROP TABLE IF EXISTS touch_move_and_begin_flow;
CREATE TABLE touch_move_and_begin_flow AS
SELECT
  flow.begin_flow_id,
  flow.begin_flow_ts,
  flow.begin_flow_track_id,
  move.*
FROM (
    SELECT
      EXTRACT_ARG(arg_set_id, "chrome_latency_info.trace_id") AS trace_id,
      *
    FROM slice move
    WHERE name = "InputLatency::TouchMove"
  ) move JOIN (
    SELECT
      MIN(id) AS begin_flow_id,
      track_id AS begin_flow_track_id,
      ts AS begin_flow_ts,
      EXTRACT_ARG(arg_set_id, "chrome_latency_info.trace_id")
      AS begin_flow_trace_id
    FROM slice
    WHERE
      name = "LatencyInfo.Flow"
      AND EXTRACT_ARG(arg_set_id, "chrome_latency_info.step") IS NULL
    GROUP BY begin_flow_trace_id
  ) flow ON flow.begin_flow_trace_id = move.trace_id;

-- Now we take the TouchMove and beginning flow event and figure out if there
-- is an end flow event on the same browser track_id. This will allow us to see
-- if it was blocking because if they share the same parent stack then they
-- weren't blocking.
DROP TABLE IF EXISTS touch_move_begin_and_end_flow;
CREATE TABLE touch_move_begin_and_end_flow AS
SELECT
  flow.end_flow_id,
  flow.end_flow_ts,
  flow.end_flow_track_id,
  move.*
FROM touch_move_and_begin_flow move LEFT JOIN (
    SELECT
      MAX(id) AS end_flow_id,
      ts AS end_flow_ts,
      track_id AS end_flow_track_id,
      EXTRACT_ARG(arg_set_id, "chrome_latency_info.trace_id")
      AS end_flow_trace_id
    FROM slice
    WHERE
      name = "LatencyInfo.Flow"
      AND EXTRACT_ARG(arg_set_id, "chrome_latency_info.step") IS NULL
    GROUP BY end_flow_trace_id
  ) flow ON
    flow.end_flow_trace_id = move.trace_id
    AND move.begin_flow_track_id = flow.end_flow_track_id
    AND flow.end_flow_id != move.begin_flow_id
WHERE flow.end_flow_id IS NOT NULL;

-- Now that we have the begin and the end we need to find the parent stack of
-- both. If the end didn't happen on the browser (end is NULL), then we can
-- ignore it because it couldn't have generated a GestureScrollUpdate.
DROP TABLE IF EXISTS touch_move_with_ancestor;
CREATE TABLE touch_move_with_ancestor AS
SELECT
  begin.id AS begin_ancestor_id,
  end.id AS end_ancestor_id,
  end.ts AS end_ancestor_ts,
  end.dur AS end_ancestor_dur,
  end.track_id AS end_ancestor_track_id,
  move.*
FROM
  touch_move_begin_and_end_flow move JOIN
  ancestor_slice(begin_flow_id) begin ON begin.depth = 0 LEFT JOIN
  ancestor_slice(end_flow_id) end ON end.depth = 0;

-- Now take the parent stack for the end and find if a GestureScrollUpdate was
-- launched that share the same parent as the end flow event for the TouchMove.
-- This is the GestureScrollUpdate that the TouchMove blocked (or didn't block)
-- depending on if the begin flow event is in the same stack.
DROP TABLE IF EXISTS blocking_touch_move_with_scroll_update;
CREATE TABLE blocking_touch_move_with_scroll_update AS
SELECT
  move.begin_ancestor_id != move.end_ancestor_id AS blocking_touch_move,
  scroll.scroll_begin_flow_id,
  scroll.scroll_begin_flow_trace_id,
  scroll.scroll_id,
  move.*
FROM touch_move_with_ancestor move LEFT JOIN (
  SELECT in_flow.*, in_scroll.scroll_id FROM (
    SELECT
      MIN(slice.id) AS scroll_begin_flow_id,
      slice.ts AS scroll_begin_flow_ts,
      slice.track_id AS scroll_begin_flow_track_id,
      EXTRACT_ARG(slice.arg_set_id, "chrome_latency_info.trace_id")
      AS scroll_begin_flow_trace_id,
      ancestor.id AS scroll_begin_flow_ancestor_id
    FROM
      slice LEFT JOIN
      ancestor_slice(slice.id) AS ancestor ON ancestor.depth = 0
    WHERE
      slice.name = "LatencyInfo.Flow"
      AND EXTRACT_ARG(slice.arg_set_id, "chrome_latency_info.step") IS NULL
    GROUP BY scroll_begin_flow_trace_id
  ) in_flow JOIN (
    SELECT
      id AS scroll_id,
      EXTRACT_ARG(arg_set_id, "chrome_latency_info.trace_id")
      AS scroll_trace_id
    FROM slice in_scroll
    WHERE
      name = "InputLatency::GestureScrollUpdate"
      AND dur != -1
      AND NOT EXTRACT_ARG(arg_set_id, "chrome_latency_info.is_coalesced")
  ) in_scroll ON
    in_scroll.scroll_trace_id = in_flow.scroll_begin_flow_trace_id
) scroll ON
  scroll.scroll_begin_flow_track_id = move.end_ancestor_track_id
  AND scroll.scroll_begin_flow_ancestor_id = move.end_ancestor_id
  AND scroll.scroll_begin_flow_ts > move.end_ancestor_ts
  AND scroll.scroll_begin_flow_ts < move.end_ancestor_ts + move.end_ancestor_dur
  AND scroll.scroll_begin_flow_id > move.end_ancestor_id
WHERE scroll.scroll_id IS NOT NULL;

-- Now filter out any TouchMoves that weren't during a complete scroll. Most of
-- the other ones will be null anyway since they won't have
-- GestureScrollUpdates.
DROP VIEW IF EXISTS scroll_jank_cause_blocking_touch_move;
CREATE VIEW scroll_jank_cause_blocking_touch_move AS
SELECT
  id,
  ts,
  dur,
  track_id,
  blocking_touch_move,
  scroll_id
FROM joined_scroll_begin_and_end begin_and_end JOIN (
    SELECT
      *
    FROM blocking_touch_move_with_scroll_update
  ) touch ON
    touch.ts <= begin_and_end.end_ts
    AND touch.ts > begin_and_end.begin_ts + begin_and_end.begin_dur
    AND touch.trace_id > begin_and_end.begin_trace_id
    AND touch.trace_id < begin_and_end.end_trace_id;
