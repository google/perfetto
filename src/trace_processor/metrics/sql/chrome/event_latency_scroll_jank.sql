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
-- A scroll jank metric based on EventLatency slices.
--
-- We define an update to be janky if comparing forwards or backwards (ignoring
-- coalesced and not shown on the screen updates) a given updates exceeds the duration
-- of its predecessor or successor by 50% of a vsync interval (defaulted to 60 FPS).
--
-- WARNING: This metric should not be used as a source of truth. It is under
--          active development and the values & meaning might change without
--          notice.

SELECT RUN_METRIC('chrome/jank_utilities.sql');
SELECT RUN_METRIC('chrome/event_latency_to_breakdowns.sql');
SELECT RUN_METRIC('chrome/vsync_intervals.sql');

-- Creates table view where each EventLatency event has its upid.
DROP VIEW IF EXISTS event_latency_with_track;
CREATE VIEW event_latency_with_track
AS
SELECT
  slice.*,
  process_track.upid AS upid
FROM slice JOIN process_track
  ON slice.track_id = process_track.id
WHERE slice.name = "EventLatency";

-- Select scroll EventLatency events that were shown on the screen.
-- An update event was shown on the screen if and only if
-- it has a "SubmitCompositorFrameToPresentationCompositorFrame" breakdown.
-- But this logic is not applied for begin events, because a begin event is an artifical marker
-- and never gets shown to the screen because it doesn't contain any update.
-- Also it automaticly only includes non-coalesced EventLatency events,
-- because coalesced ones are not shown on the screen.
DROP VIEW IF EXISTS filtered_scroll_event_latency;
CREATE VIEW filtered_scroll_event_latency
AS
WITH shown_on_display_event_latency_ids AS (
  SELECT
  event_latency_id
  FROM event_latency_breakdowns
  WHERE name = "SubmitCompositorFrameToPresentationCompositorFrame" OR event_type = "GESTURE_SCROLL_BEGIN"
)
SELECT
  event_latency_with_track.id,
  event_latency_with_track.track_id,
  event_latency_with_track.upid,
  event_latency_with_track.ts,
  event_latency_with_track.dur,
  EXTRACT_ARG(event_latency_with_track.arg_set_id, "event_latency.event_type") AS event_type
FROM event_latency_with_track JOIN shown_on_display_event_latency_ids
  ON event_latency_with_track.id = shown_on_display_event_latency_ids.event_latency_id
WHERE
  event_type IN (
    "GESTURE_SCROLL_BEGIN", "GESTURE_SCROLL_UPDATE",
    "INERTIAL_GESTURE_SCROLL_UPDATE", "FIRST_GESTURE_SCROLL_UPDATE");

-- Select begin events and it's next begin event witin the same process (same upid).
--
-- Note: Must be a TABLE because it uses a window function which can behave
--       strangely in views.
DROP TABLE IF EXISTS scroll_event_latency_begins;
CREATE TABLE scroll_event_latency_begins
AS
SELECT
  *,
  LEAD(ts) OVER sorted_begins AS next_gesture_begin_ts
FROM filtered_scroll_event_latency
WHERE event_type = "GESTURE_SCROLL_BEGIN"
WINDOW sorted_begins AS (PARTITION BY upid ORDER BY ts ASC);

-- For each scroll update event finds it's begin event.
-- Pair [upid, next_gesture_begin_ts] represent a gesture key.
-- We need to know the gesture key of gesture scroll to calculate a jank only within this gesture scroll.
-- Because different gesture scrolls can have different properties.
DROP VIEW IF EXISTS scroll_event_latency_updates;
CREATE VIEW scroll_event_latency_updates
AS
SELECT
  filtered_scroll_event_latency.*,
  scroll_event_latency_begins.ts AS gesture_begin_ts,
  scroll_event_latency_begins.next_gesture_begin_ts AS next_gesture_begin_ts
FROM filtered_scroll_event_latency LEFT JOIN scroll_event_latency_begins
  ON filtered_scroll_event_latency.ts >= scroll_event_latency_begins.ts
     AND (filtered_scroll_event_latency.ts < next_gesture_begin_ts OR next_gesture_begin_ts IS NULL)
     AND filtered_scroll_event_latency.upid = scroll_event_latency_begins.upid
WHERE filtered_scroll_event_latency.id != scroll_event_latency_begins.id
      AND filtered_scroll_event_latency.event_type != "GESTURE_SCROLL_BEGIN";

-- Find the last EventLatency scroll update event in the scroll.
-- We will use the last EventLatency event insted of "InputLatency::GestureScrollEnd" event.
-- We need to know when the scroll gesture ends so that we can later calculate
-- the average vsync interval just up to the end of the gesture.
DROP VIEW IF EXISTS scroll_event_latency_updates_ends;
CREATE VIEW scroll_event_latency_updates_ends
AS
SELECT
  id,
  upid,
  gesture_begin_ts,
  ts,
  dur,
  MAX(ts + dur) AS gesture_end_ts
FROM scroll_event_latency_updates
GROUP BY upid, gesture_begin_ts;

DROP VIEW IF EXISTS scroll_event_latency_updates_with_ends;
CREATE VIEW scroll_event_latency_updates_with_ends
AS
SELECT
  scroll_event_latency_updates.*,
  scroll_event_latency_updates_ends.gesture_end_ts AS gesture_end_ts
FROM scroll_event_latency_updates LEFT JOIN scroll_event_latency_updates_ends
  ON scroll_event_latency_updates.upid = scroll_event_latency_updates_ends.upid
    AND scroll_event_latency_updates.gesture_begin_ts = scroll_event_latency_updates_ends.gesture_begin_ts;

-- Creates table where each event contains info about it's previous and next events.
-- We consider only previous and next events from the same scroll id
-- to don't calculate a jank between different scrolls.
--
-- Note: Must be a TABLE because it uses a window function which can behave
--       strangely in views.
DROP TABLE IF EXISTS scroll_event_latency_with_neighbours;
CREATE TABLE scroll_event_latency_with_neighbours
AS
SELECT
  *,
  LEAD(id) OVER sorted_events AS next_id,
  LEAD(ts) OVER sorted_events AS next_ts,
  LEAD(dur) OVER sorted_events AS next_dur,
  LAG(id) OVER sorted_events AS prev_id,
  LAG(ts) OVER sorted_events AS prev_ts,
  LAG(dur) OVER sorted_events AS prev_dur,
  CalculateAvgVsyncInterval(gesture_begin_ts, gesture_end_ts) AS avg_vsync_interval
FROM scroll_event_latency_updates_with_ends
WINDOW sorted_events AS (PARTITION BY upid, next_gesture_begin_ts ORDER BY id ASC, ts ASC);

DROP VIEW IF EXISTS scroll_event_latency_neighbors_jank;
CREATE VIEW scroll_event_latency_neighbors_jank
AS
SELECT
  IsJankyFrame(gesture_begin_ts, gesture_begin_ts, next_ts,
    gesture_begin_ts, gesture_end_ts, dur / avg_vsync_interval, next_dur / avg_vsync_interval) AS next_jank,
  IsJankyFrame(gesture_begin_ts, gesture_begin_ts, prev_ts,
    gesture_begin_ts, gesture_end_ts, dur / avg_vsync_interval, prev_dur / avg_vsync_interval) AS prev_jank,
  scroll_event_latency_with_neighbours.*
FROM scroll_event_latency_with_neighbours;

-- Creates a view where each event contains information about whether it is janky
-- with respect to previous and next events within the same scroll.
DROP VIEW IF EXISTS scroll_event_latency_jank;
CREATE VIEW scroll_event_latency_jank
AS
SELECT
  (next_jank IS NOT NULL AND next_jank) OR (prev_jank IS NOT NULL AND prev_jank) AS jank,
  *
FROM scroll_event_latency_neighbors_jank;
