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

-- This metric calculates the number of user inputs during a scroll per frame
-- generated, this can be used to track smoothness.
-- For normal input, we expect 2 inputs per frame, for flings, we expect
-- 1 input per frame as flings are generated once per vsync.
-- The numbers mentioned above are estimates in the ideal case scenario.

INCLUDE PERFETTO MODULE chrome.scroll_jank.utils;
INCLUDE PERFETTO MODULE common.slices;

-- Grab all GestureScrollUpdate slices.
DROP VIEW IF EXISTS chrome_all_scroll_updates;
CREATE PERFETTO VIEW chrome_all_scroll_updates AS
SELECT
  S.id,
  chrome_get_most_recent_scroll_begin_id(ts) AS scroll_id,
  has_descendant_slice_with_name(S.id, "SubmitCompositorFrameToPresentationCompositorFrame")
  AS is_presented,
  ts,
  dur,
  track_id
FROM slice S JOIN args USING(arg_set_id)
WHERE NAME = "EventLatency"
AND args.string_value GLOB "*GESTURE_SCROLL_UPDATE";

-- Count number of input GestureScrollUpdates per scroll.
DROP VIEW IF EXISTS chrome_update_count_per_scroll;
CREATE PERFETTO VIEW chrome_update_count_per_scroll AS
SELECT
  CAST(COUNT() AS FLOAT) AS count,
  scroll_id,
  dur,
  track_id
FROM chrome_all_scroll_updates
GROUP BY scroll_id;

-- Count the number of input GestureScrollUpdates that were converted
-- frames per scroll.
DROP VIEW IF EXISTS chrome_presented_update_count_per_scroll;
CREATE PERFETTO VIEW chrome_presented_update_count_per_scroll AS
SELECT
  CAST(COUNT() AS FLOAT) AS presented_count,
  scroll_id,
  id,
  track_id,
  dur
FROM chrome_all_scroll_updates
WHERE is_presented
GROUP BY scroll_id;

-- Get the average number of inputs per frame per scroll.
DROP VIEW IF EXISTS chrome_avg_scroll_inputs_per_frame;
CREATE PERFETTO VIEW chrome_avg_scroll_inputs_per_frame AS
SELECT
  count / presented_count AS avg_inputs_per_frame_per_scroll,
  scroll_id,
  presented_count
FROM chrome_presented_update_count_per_scroll
JOIN chrome_update_count_per_scroll USING(scroll_id);

-- Get the last scroll update event that wasn't coalesced before the
-- current scroll update.
DROP VIEW IF EXISTS chrome_frame_main_input_id;
CREATE PERFETTO VIEW chrome_frame_main_input_id AS
SELECT
  id,
  scroll_id,
  is_presented,
  ts,
  dur,
  track_id,
  (SELECT
    MAX(id)
    FROM chrome_all_scroll_updates parent_scrolls
    WHERE is_presented
      AND parent_scrolls.ts <= scrolls.ts) AS presented_scroll_id
FROM chrome_all_scroll_updates scrolls;

-- Count the number of inputs per presented frame.
DROP VIEW IF EXISTS chrome_scroll_inputs_per_frame;
CREATE PERFETTO VIEW chrome_scroll_inputs_per_frame AS
SELECT
  COUNT() AS count_for_frame,
  presented_scroll_id,
  ts,
  dur,
  id AS slice_id,
  track_id
FROM
  chrome_frame_main_input_id
GROUP BY presented_scroll_id;
