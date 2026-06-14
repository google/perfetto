-- Copyright 2023 The Chromium Authors
-- Use of this source code is governed by a BSD-style license that can be
-- found in the LICENSE file.

-- NOTE (psqlnext): the hand-built window-function coalesce in
-- `chrome_scroll_jank_intervals_v3` is `INTERVAL MERGE OVERLAPPING` over the
-- per-jank presentation intervals.

INCLUDE PERFETTO MODULE chrome.chrome_scrolls;

INCLUDE PERFETTO MODULE chrome.scroll_jank.scroll_jank_v3;
INCLUDE PERFETTO MODULE chrome.event_latency;

-- Selects EventLatency slices that correspond with janks in a scroll. This is
-- based on the V3 version of scroll jank metrics.
CREATE PERFETTO PIPELINE chrome_janky_event_latencies_v3 (
  -- The slice id.
  id LONG,
  -- The start timestamp of the slice.
  ts TIMESTAMP,
  -- The duration of the slice.
  dur DURATION,
  -- The track_id for the slice.
  track_id LONG,
  -- The name of the slice (EventLatency).
  name STRING,
  -- The stage of EventLatency that the caused the jank.
  cause_of_jank STRING,
  -- The stage of cause_of_jank that caused the jank.
  sub_cause_of_jank STRING,
  -- How many vsyncs this frame missed its deadline by.
  delayed_frame_count LONG,
  -- The start timestamp where frame presentation was delayed.
  frame_jank_ts TIMESTAMP,
  -- The duration in ms of the delay in frame presentation.
  frame_jank_dur LONG
)
MATERIALIZED AS
FROM chrome_gesture_scroll_updates AS s
|> JOIN chrome_janky_frames AS e ON s.id = e.event_latency_id
|> SELECT
     s.id,
     s.ts,
     s.dur,
     s.track_id,
     s.name,
     e.cause_of_jank,
     e.sub_cause_of_jank,
     cast_int!((e.delay_since_last_frame/e.vsync_interval) - 1) AS delayed_frame_count,
     cast_int!(s.ts + s.dur - ((e.delay_since_last_frame - e.vsync_interval) * 1e6)) AS frame_jank_ts,
     cast_int!((e.delay_since_last_frame - e.vsync_interval) * 1e6) AS frame_jank_dur;

-- Frame presentation interval is the delta between when the frame was supposed
-- to be presented and when it was actually presented.
CREATE PERFETTO PIPELINE chrome_janky_frame_presentation_intervals (
  -- Unique id.
  id LONG,
  -- The start timestamp of the slice.
  ts TIMESTAMP,
  -- The duration of the slice.
  dur DURATION,
  -- How many vsyncs this frame missed its deadline by.
  delayed_frame_count LONG,
  -- The stage of EventLatency that the caused the jank.
  cause_of_jank STRING,
  -- The stage of cause_of_jank that caused the jank.
  sub_cause_of_jank STRING,
  -- The id of the associated event latency in the slice table.
  event_latency_id LONG
) AS
FROM chrome_janky_event_latencies_v3
|> SELECT
     row_number() OVER (ORDER BY frame_jank_ts) AS id,
     frame_jank_ts AS ts,
     frame_jank_dur AS dur,
     delayed_frame_count,
     cause_of_jank,
     sub_cause_of_jank,
     id AS event_latency_id;

-- Scroll jank frame presentation stats for individual scrolls.
CREATE PERFETTO PIPELINE chrome_scroll_stats (
  -- Id of the individual scroll.
  scroll_id LONG,
  -- The number of frames in the scroll.
  frame_count LONG,
  -- The number of missed vsyncs in the scroll.
  missed_vsyncs LONG,
  -- The number presented frames in the scroll.
  presented_frame_count LONG,
  -- The number of janky frames in the scroll.
  janky_frame_count LONG,
  -- The % of frames that janked in the scroll.
  janky_frame_percent DOUBLE
)
MATERIALIZED AS
SUBPIPELINE vsyncs AS (
  FROM chrome_unique_frame_presentation_ts AS frame
  |> JOIN chrome_scrolls AS scroll
       ON frame.presentation_timestamp >= scroll.ts
       AND frame.presentation_timestamp <= scroll.ts + scroll.dur
  |> AGGREGATE count() AS presented_vsync_count GROUP BY scroll.id
  |> SELECT presented_vsync_count, id AS scroll_id
)
SUBPIPELINE missed_vsyncs AS (
  FROM chrome_janky_frames
  |> AGGREGATE
       cast_int!(SUM((delay_since_last_frame / vsync_interval) - 1)) AS total_missed_vsyncs
     GROUP BY scroll_id
  |> SELECT total_missed_vsyncs, scroll_id
)
SUBPIPELINE frame_stats AS (
  FROM chrome_frames_per_scroll
  |> SELECT
       scroll_id,
       num_frames AS presented_frame_count,
       coalesce(num_janky_frames, 0) AS janky_frame_count,
       round(coalesce(scroll_jank_percentage, 0), 2) AS janky_frame_percent
)
FROM vsyncs
|> LEFT JOIN missed_vsyncs USING (scroll_id)
|> LEFT JOIN frame_stats USING (scroll_id)
|> SELECT
     vsyncs.scroll_id,
     presented_vsync_count + coalesce(total_missed_vsyncs, 0) AS frame_count,
     total_missed_vsyncs AS missed_vsyncs,
     presented_frame_count,
     janky_frame_count,
     janky_frame_percent;

-- Defines slices for all of janky scrolling intervals in a trace.
CREATE PERFETTO PIPELINE chrome_scroll_jank_intervals_v3 (
  -- The unique identifier of the janky interval.
  id LONG,
  -- The start timestamp of the janky interval.
  ts TIMESTAMP,
  -- The duration of the janky interval.
  dur DURATION
)
MATERIALIZED AS
-- Each janky EventLatency contributes a presentation interval; overlapping and
-- abutting intervals coalesce into one janky interval.
FROM chrome_janky_event_latencies_v3 AS s
|> SELECT s.frame_jank_ts AS ts, s.frame_jank_dur AS dur
|> INTERVAL MERGE OVERLAPPING AGGREGATE COUNT(*) AS _cnt
|> SELECT
     row_number() OVER (ORDER BY ts) AS id,
     ts,
     dur;
