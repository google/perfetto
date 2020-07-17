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
--
-- A collection of metrics related to GestureScrollUpdate events.
--
-- We define a GestureScrollUpdate to be janky if comparing forwards or
-- backwards (ignoring coalesced updates) a given GestureScrollUpdate exceeds
-- the duration of its predecessor or successor by 50% of a vsync interval
-- (defaulted to 60 FPS).
--
-- WARNING: This metric should not be used as a source of truth. It is under
--          active development and the values & meaning might change without
--          notice.

-- Get all chrome processes and threads tables set up.
SELECT RUN_METRIC('chrome/chrome_processes.sql');

-- When working on GestureScrollUpdate events we need to ensure we have all the
-- events from the browser, renderer, and GPU processes. This query isn't quite
-- perfect. In system tracing we could have 3 browser processes all in the
-- background and this would match, but for now its the best we can do (renderer
-- and GPU names on android are quite complicated, but this should filter 99% (
-- citation needed) of what we want.
--
-- See b/151077536 for historical context.
CREATE VIEW IF NOT EXISTS sufficient_chrome_processes AS
  SELECT
    CASE WHEN (
      SELECT COUNT(*) FROM chrome_process) = 0
    THEN
      FALSE
    ELSE (
      SELECT COUNT(*) >= 3 FROM (
        SELECT name FROM chrome_process
        WHERE
          name LIKE "Browser" OR
          name LIKE "Renderer" OR
          name LIKE "Gpu" OR
          name LIKE 'com.android.chrome%' OR
          name LIKE 'com.chrome.beta%' OR
          name LIKE 'com.chrome.dev%' OR
          name LIKE 'com.chrome.canary%' OR
          name LIKE 'com.google.android.apps.chrome%' OR
          name LIKE 'org.chromium.chrome%'
        GROUP BY name
    )) END AS have_enough_chrome_processes;

-- A simple table that checks the time between VSync (this can be used to
-- determine if we're scrolling at 90 FPS or 60 FPS.
--
-- Note: In traces without the "Java" category there will be no VSync
--       TraceEvents and this table will be empty.
--
-- Note: Must be a TABLE because it uses a window function which can behave
--       strangely in views.
CREATE TABLE IF NOT EXISTS vsync_intervals AS
  SELECT
    slice_id,
    ts,
    dur,
    track_id,
    LEAD(ts) OVER(PARTITION BY track_id ORDER BY ts) - ts AS time_to_next_vsync
  FROM slice
  WHERE name = "VSync"
  ORDER BY track_id, ts;

-- Get all the GestureScrollBegin and GestureScrollEnd events. We take their
-- IDs to group them together into scrolls later and the timestamp and duration
-- to compute the duration of the scroll.
CREATE VIEW IF NOT EXISTS scroll_begin_and_end AS
  SELECT
    slice.name,
    slice.id,
    slice.ts,
    slice.dur,
    slice.track_id,
    EXTRACT_ARG(arg_set_id, 'chrome_latency_info.gesture_scroll_id')
        AS gesture_scroll_id,
    EXTRACT_ARG(arg_set_id, "chrome_latency_info.trace_id") AS trace_id
  FROM
    slice
  WHERE
    slice.name IN (
      'InputLatency::GestureScrollBegin',
      'InputLatency::GestureScrollEnd'
    )
  ORDER BY ts;

-- Now we take the GestureScrollBegin and the GestureScrollEnd events and join
-- the information into a single row per scroll. We also compute the average
-- Vysnc interval of the scroll (hopefully this would be either 60 FPS for the
-- whole scroll or 90 FPS but that isn't always the case). If the trace doesn't
-- contain the VSync TraceEvent we just fall back on assuming its 60 FPS (this
-- is the 1.6e+7 in the COALESCE which corresponds to 16 ms or 60 FPS).
CREATE VIEW IF NOT EXISTS joined_scroll_begin_and_end AS
  SELECT
    begin.id AS begin_id,
    begin.ts AS begin_ts,
    begin.dur AS begin_dur,
    begin.track_id AS begin_track_id,
    begin.trace_id AS begin_trace_id,
    COALESCE(begin.gesture_scroll_id, begin.trace_id)
        AS begin_gesture_scroll_id,
    end.ts AS end_ts,
    end.ts + end.dur AS end_ts_and_dur,
    end.trace_id AS end_trace_id,
    COALESCE((
      SELECT
        CAST(AVG(time_to_next_vsync) AS FLOAT)
      FROM vsync_intervals in_query
      WHERE
        time_to_next_vsync IS NOT NULL AND
        in_query.ts > begin.ts AND
        in_query.ts < end.ts
    ), 1.6e+7) AS avg_vsync_interval
  FROM scroll_begin_and_end begin JOIN scroll_begin_and_end end ON
    begin.trace_id < end.trace_id AND
    begin.name = 'InputLatency::GestureScrollBegin' AND
    end.name = 'InputLatency::GestureScrollEnd' AND (
      (
        begin.gesture_scroll_id IS NULL AND
        end.trace_id = (
          SELECT MIN(trace_id)
          FROM scroll_begin_and_end in_query
          WHERE
            name = 'InputLatency::GestureScrollEnd' AND
          in_query.trace_id > begin.trace_id
        )
      ) OR
      end.gesture_scroll_id = begin.gesture_scroll_id
    )
  ORDER BY begin.ts;

-- Get the GestureScrollUpdate events by name ordered by the
-- |gesture_scroll_id|, and timestamp. Then compute the number of frames (
-- relative to vsync interval) that each event took. 1.6e+7 is 16 ms in
-- nanoseconds and is used in case there are no VSync events to default to 60
-- fps. We join each GestureScrollUpdate event to the information about it'
-- begin and end events for easy computation later.
--
-- We remove updates with |dur| == -1 because this means we have no end event
-- and can't reasonably determine what it should be. We have separate tracking
-- to ensure this only happens at the end of the trace where its expected.
--
-- Note: Must be a TABLE because it uses a window function which can behave
--       strangely in views.
CREATE TABLE IF NOT EXISTS gesture_scroll_update AS
  SELECT
    ROW_NUMBER() OVER (
      ORDER BY gesture_scroll_id ASC, ts ASC) AS row_number,
    begin_id,
    begin_ts,
    begin_dur,
    begin_track_id,
    begin_trace_id,
    COALESCE(gesture_scroll_id, begin_trace_id) AS gesture_scroll_id,
    CASE WHEN
      end_ts_and_dur > ts + dur THEN
        end_ts_and_dur
      ELSE
        ts + dur
      END AS maybe_scroll_end,
    id,
    ts,
    dur,
    track_id,
    trace_id,
    dur/avg_vsync_interval AS scroll_frames_exact
  FROM joined_scroll_begin_and_end begin_and_end JOIN (
    SELECT
      EXTRACT_ARG(arg_set_id, "chrome_latency_info.trace_id") AS trace_id,
      EXTRACT_ARG(arg_set_id, 'chrome_latency_info.gesture_scroll_id')
          AS gesture_scroll_id,
      *
    FROM
      slice JOIN track ON slice.track_id = track.id
    WHERE
      slice.name = 'InputLatency::GestureScrollUpdate' AND
      slice.dur != -1 AND
      NOT COALESCE(
              EXTRACT_ARG(arg_set_id, "chrome_latency_info.is_coalesced"),
              TRUE)
  ) scroll_update ON
  scroll_update.ts <= begin_and_end.end_ts AND
  scroll_update.ts >= begin_and_end.begin_ts AND
  scroll_update.trace_id > begin_and_end.begin_trace_id AND
  scroll_update.trace_id < begin_and_end.end_trace_id AND (
    scroll_update.gesture_scroll_id IS NULL OR
    scroll_update.gesture_scroll_id = begin_and_end.begin_gesture_scroll_id
  );

-- This takes the GestureScrollUpdate events and joins it to the previous
-- GestureScrollUpdate event (previous row and NULL if there isn't one) and the
-- next GestureScrollUpdate event (next row and again NULL if there isn't one).
-- Then we compute the duration of the event (relative to fps) and see if it
-- increased by more then 0.5 (which is 1/2 of 16 ms at 60 fps, and so on).
--
-- We only compare a GestureScrollUpdate event to another event within the same
-- scroll (gesture_scroll_id == prev/next gesture_scroll_id). This controls
-- somewhat for variability of scrolls.
--
-- Note: Must be a TABLE because it uses a window function which can behave
--       strangely in views.
CREATE TABLE IF NOT EXISTS scroll_jank_maybe_null_prev_and_next AS
  SELECT
    currprev.*,
    CASE WHEN
      currprev.gesture_scroll_id != prev_gesture_scroll_id OR
      prev_ts IS NULL OR
      prev_ts < currprev.begin_ts OR
      prev_ts > currprev.maybe_scroll_end
    THEN
      FALSE
    ELSE
      currprev.scroll_frames_exact > prev_scroll_frames_exact + 0.5
    END AS prev_jank,
    CASE WHEN
      currprev.gesture_scroll_id != next.gesture_scroll_id OR
      next.ts IS NULL OR
      next.ts < currprev.begin_ts OR
      next.ts > currprev.maybe_scroll_end
    THEN
      FALSE
    ELSE
      currprev.scroll_frames_exact > next.scroll_frames_exact + 0.5
    END AS next_jank,
    next.scroll_frames_exact AS next_scroll_frames_exact
  FROM (
    SELECT
      curr.*,
      curr.maybe_scroll_end - curr.begin_ts AS scroll_dur,
      prev.ts AS prev_ts,
      prev.gesture_scroll_id AS prev_gesture_scroll_id,
      prev.scroll_frames_exact AS prev_scroll_frames_exact
    FROM
      gesture_scroll_update curr LEFT JOIN
      gesture_scroll_update prev ON prev.row_number + 1 = curr.row_number
  ) currprev LEFT JOIN
  gesture_scroll_update next ON currprev.row_number + 1 = next.row_number
  ORDER BY currprev.gesture_scroll_id ASC, currprev.ts ASC;

-- This just uses prev_jank and next_jank to see if each GestureScrollUpdate
-- event is a jank.
CREATE VIEW IF NOT EXISTS scroll_jank AS
  SELECT
    *,
    (next_jank IS NOT NULL AND next_jank) OR
    (prev_jank IS NOT NULL AND prev_jank)
    AS jank
  FROM scroll_jank_maybe_null_prev_and_next
  ORDER BY gesture_scroll_id ASC, ts ASC;
