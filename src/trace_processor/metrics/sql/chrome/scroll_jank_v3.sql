--
-- Copyright 2023 The Android Open Source Project
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

SELECT IMPORT('common.slices');

-- Hardware info is useful when using sql metrics for analysis
-- in BTP.
SELECT IMPORT("chrome.metadata");

-- TODO(b/286198401) move this whole script to stdlib.
-- TODO(b/286198934) move this dependency to stdlib.
SELECT RUN_METRIC('chrome/event_latency_scroll_jank_cause.sql');


-- Grabs all gesture updates that were not coalesced with their
-- respective scroll ids and start/end timestamps.
DROP VIEW IF EXISTS chrome_presented_gesture_scrolls;
CREATE VIEW chrome_presented_gesture_scrolls AS
SELECT
  ts AS start_ts,
  ts + dur AS end_ts,
  id,
  -- TODO(b/250089570) Add trace_id to EventLatency and update this script to use it.
  EXTRACT_ARG(arg_set_id, 'chrome_latency_info.trace_id') AS scroll_update_id,
  EXTRACT_ARG(arg_set_id, 'chrome_latency_info.gesture_scroll_id') AS scroll_id
FROM slice
WHERE name = "InputLatency::GestureScrollUpdate"
      AND EXTRACT_ARG(arg_set_id, 'chrome_latency_info.is_coalesced') = FALSE
      AND dur != -1;


-- Associate every trace_id with it's perceived delta_y on the screen after
-- prediction.
DROP VIEW IF EXISTS chrome_scroll_updates_with_deltas;
CREATE VIEW chrome_scroll_updates_with_deltas AS
SELECT
  EXTRACT_ARG(arg_set_id, 'scroll_deltas.trace_id') AS scroll_update_id,
  EXTRACT_ARG(arg_set_id, 'scroll_deltas.provided_to_compositor_delta_y') AS delta_y
FROM slice
WHERE name = "InputHandlerProxy::HandleGestureScrollUpdate_Result";

-- Extract event latency timestamps, to later use it for joining
-- with gesture scroll updates, as event latencies don't have trace
-- ids associated with it.
DROP VIEW IF EXISTS chrome_gesture_scroll_event_latencies;
CREATE VIEW chrome_gesture_scroll_event_latencies AS
SELECT
  slice.ts AS start_ts,
  slice.id AS event_latency_id,
  slice.dur AS dur,
  DESCENDANT_SLICE_END(slice.id, "LatchToSwapEnd") AS input_latency_end_ts,
  DESCENDANT_SLICE_END(slice.id, "SwapEndToPresentationCompositorFrame") AS presentation_timestamp,
  EXTRACT_ARG(arg_set_id, 'event_latency.event_type') AS event_type
FROM slice
WHERE name = "EventLatency"
      AND event_type in (
          "GESTURE_SCROLL_UPDATE",
          "FIRST_GESTURE_SCROLL_UPDATE",
          "INERTIAL_GESTURE_SCROLL_UPDATE")
      AND HAS_DESCENDANT_SLICE_WITH_NAME(slice.id, "SwapEndToPresentationCompositorFrame");


-- Join presented gesture scrolls with their respective event
-- latencies based on |LatchToSwapEnd| timestamp, as it's the
-- end timestamp for both the gesture scroll update slice and
-- the LatchToSwapEnd slice.
DROP VIEW IF EXISTS chrome_full_frame_view;
CREATE VIEW chrome_full_frame_view AS
SELECT
  frames.id,
  frames.start_ts,
  frames.scroll_id,
  frames.scroll_update_id,
  events.event_latency_id,
  events.dur,
  events.presentation_timestamp
FROM chrome_presented_gesture_scrolls frames
JOIN chrome_gesture_scroll_event_latencies events
  ON frames.start_ts = events.start_ts
  AND events.input_latency_end_ts = frames.end_ts;

DROP VIEW IF EXISTS chrome_full_frame_delta_view;
CREATE VIEW chrome_full_frame_delta_view AS
SELECT
  frames.id,
  frames.start_ts,
  frames.scroll_id,
  frames.scroll_update_id,
  deltas.delta_y,
  frames.event_latency_id,
  frames.dur,
  frames.presentation_timestamp
FROM chrome_full_frame_view frames
LEFT JOIN chrome_scroll_updates_with_deltas deltas
  ON deltas.scroll_update_id = frames.scroll_update_id;

 -- Join the frame view with scroll jank cause and subcause based
 -- on event latency id.
DROP TABLE IF EXISTS chrome_frame_view_with_jank;
CREATE TABLE chrome_frame_view_with_jank AS
SELECT
  frames.*,
  jank_cause.cause_of_jank,
  jank_cause.sub_cause_of_jank
FROM event_latency_scroll_jank_cause jank_cause
RIGHT JOIN chrome_full_frame_delta_view frames
  ON jank_cause.slice_id = frames.event_latency_id;

-- Group all gestures presented at the same timestamp together in
-- a single row.
DROP VIEW IF EXISTS chrome_merged_frame_view_with_jank;
CREATE VIEW chrome_merged_frame_view_with_jank AS
SELECT
  id,
  start_ts,
  scroll_id,
  scroll_update_id,
  GROUP_CONCAT(scroll_update_id,',') AS encapsulated_scroll_ids,
  SUM(delta_y) AS total_delta,
  GROUP_CONCAT(delta_y, ',') AS segregated_delta_y,
  event_latency_id,
  MAX(dur) AS dur,
  presentation_timestamp,
  cause_of_jank,
  sub_cause_of_jank
FROM chrome_frame_view_with_jank
GROUP BY presentation_timestamp
ORDER BY presentation_timestamp;

-- View contains all chrome presented frames during gesture updates
-- while calculating delay since last presented which usually should
-- equal to |VSYNC_INTERVAL| if no jank is present.
-- @column id                      gesture scroll slice id.
-- @column start_ts                OS timestamp of touch move arrival.
-- @column scroll_id               The scroll which the touch belongs to.
-- @column encapsulated_scroll_ids Trace ids of all frames presented in at this vsync.
-- @column total_delta             Summation of all delta_y of all gesture scrolls in this frame.
-- @column segregated_delta_y      All delta y of all gesture scrolls comma separated, summing those gives |total_delta|
-- @column event_latency_id        Event latency id of the presented frame.
-- @column dur                     Duration of the EventLatency.
-- @column presentation_timestamp  Timestamp at which the frame was shown on the screen.
-- @column cause_of_jank           Cause of jank will be present if a frame takes more than 1/2 a vsync than it's neighbours, will be filtered to real positives later.
-- @column sub_cause_of_jank       If the cause is GPU related, a sub cause is present for further breakdown.
-- @column delay_since_last_frame Time elapsed since the previous frame was presented, usually equals |VSYNC| if no frame drops happened.
-- @column delay_since_last_input difference in OS timestamps of inputs in the current and the previous frame.
DROP VIEW IF EXISTS chrome_janky_frame_info_with_delay;
CREATE VIEW chrome_janky_frame_info_with_delay AS
SELECT
  *,
  (presentation_timestamp -
  LAG(presentation_timestamp, 1, presentation_timestamp)
  OVER (PARTITION BY scroll_id ORDER BY presentation_timestamp)) / 1e6 AS delay_since_last_frame,
  (start_ts -
  LAG(start_ts, 1, start_ts)
  OVER (PARTITION BY scroll_id ORDER BY start_ts)) / 1e6 AS delay_since_last_input
FROM chrome_merged_frame_view_with_jank;


-- Calculate |VSYNC_INTERVAL| as the lowest delay between frames larger than zero.
-- TODO(b/286222128): Emit this data from Chrome instead of calculating it.
DROP VIEW IF EXISTS chrome_vsyncs;
CREATE VIEW chrome_vsyncs AS
SELECT
  MIN(delay_since_last_frame) AS vsync_interval
FROM chrome_janky_frame_info_with_delay
WHERE delay_since_last_frame > 0;

-- Filter the frame view only to frames that had missed vsyncs.
-- @column cause_of_jank          The reason the Vsync was missed.
-- @column sub_cause_of_jank      Further breakdown if the root cause was GPU related.
-- @column delay_since_last_frame Time elapsed since the previous frame was presented, will be more than |VSYNC| in this view.
-- @column event_latency_id       Event latency id of the presented frame.
-- @column vsync_interval         Vsync interval at the time of recording the trace.
-- @column hardware_class         Device brand and model.
DROP VIEW IF EXISTS chrome_janky_frames;
CREATE VIEW chrome_janky_frames AS
SELECT
  cause_of_jank,
  sub_cause_of_jank,
  delay_since_last_frame,
  event_latency_id,
  (SELECT vsync_interval FROM chrome_vsyncs) AS vsync_interval,
  CHROME_HARDWARE_CLASS() AS hardware_class
FROM chrome_janky_frame_info_with_delay
WHERE delay_since_last_frame > (select vsync_interval + vsync_interval / 2 from chrome_vsyncs)
      AND delay_since_last_input < (select vsync_interval + vsync_interval / 2 from chrome_vsyncs);

-- Counting all unique frame presentation timestamps.
DROP VIEW IF EXISTS chrome_unique_frame_presentation_ts;
CREATE VIEW chrome_unique_frame_presentation_ts AS
SELECT DISTINCT
presentation_timestamp
FROM chrome_gesture_scroll_event_latencies;

-- Dividing missed frames over total frames to get janky frame percentage.
-- This represents the v3 scroll jank metrics.
-- Reflects Event.Jank.DelayedFramesPercentage UMA metric.
DROP VIEW IF EXISTS chrome_janky_frames_percentage;
CREATE VIEW chrome_janky_frames_percentage AS
SELECT
(SELECT
  COUNT()
 FROM chrome_janky_frames) * 1.0
/ (SELECT
    COUNT()
  FROM chrome_unique_frame_presentation_ts) * 100 AS delayed_frame_percentage;